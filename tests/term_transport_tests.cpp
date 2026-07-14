// MIT License © 2025 Binary Dice Games
// Tests for bdg::bison::rmi::transport::term_transport, which frames
// client->server envelopes as OSC-99-chunked escape sequences and
// server->client envelopes as un-chunked BISON<...> markers over a pair of
// raw fds. See FORMAT.md §5.3 for the wire framing contract.

#include "src/rmi/transport/term_transport.hpp"

#include <gtest/gtest.h>

#include <mutex>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#else
#include <unistd.h>
#endif

using namespace bdg::bison;
using namespace bdg::bison::rmi::transport;

namespace {

bool make_pipe(int fds[2]) {
#if defined(_WIN32)
  return _pipe(fds, 4096, _O_BINARY) == 0;
#else
  return pipe(fds) == 0;
#endif
}

void write_raw(int fd, const std::string& data) {
#if defined(_WIN32)
  ASSERT_EQ(_write(fd, data.data(), static_cast<unsigned int>(data.size())), static_cast<int>(data.size()));
#else
  ASSERT_EQ(write(fd, data.data(), data.size()), static_cast<ssize_t>(data.size()));
#endif
}

long read_raw(int fd, char* buf, size_t len) {
#if defined(_WIN32)
  return _read(fd, buf, static_cast<unsigned int>(len));
#else
  return read(fd, buf, len);
#endif
}

void close_raw(int fd) {
#if defined(_WIN32)
  _close(fd);
#else
  close(fd);
#endif
}

/** @brief A client/server fd pair: two unidirectional pipes forming one full-duplex channel. */
struct duplex_pipes {
  int client_read;
  int client_write;
  int server_read;
  int server_write;
};

bool make_duplex_pipes(duplex_pipes& out) {
  int c2s[2];
  int s2c[2];
  if (!make_pipe(c2s) || !make_pipe(s2c))
    return false;
  out.server_read = c2s[0];
  out.client_write = c2s[1];
  out.client_read = s2c[0];
  out.server_write = s2c[1];
  return true;
}

} // namespace

// ═════════════════════════════════════════════════════════════════════════════
// TermTransport
// ═════════════════════════════════════════════════════════════════════════════

TEST(TermTransport, ClientSendServerReceive) {
  duplex_pipes p{};
  ASSERT_TRUE(make_duplex_pipes(p));

  term_server_transport server_t{p.server_read, p.server_write, term_discard_passthrough};
  server_t.start(dynamic{});
  auto conn = server_t.accept();
  ASSERT_TRUE(conn != nullptr);

  term_client_transport client_t{p.client_read, p.client_write, term_discard_passthrough};
  client_t.open(dynamic{});

  const buffer frame{'H', 'e', 'l', 'l', 'o'};
  client_t.send(frame);

  buffer received;
  ASSERT_TRUE(conn->receive(received, std::chrono::milliseconds{2000}));
  EXPECT_EQ(received, frame);
}

TEST(TermTransport, ServerSendClientReceive) {
  duplex_pipes p{};
  ASSERT_TRUE(make_duplex_pipes(p));

  term_server_transport server_t{p.server_read, p.server_write, term_discard_passthrough};
  server_t.start(dynamic{});
  auto conn = server_t.accept();
  ASSERT_TRUE(conn != nullptr);

  term_client_transport client_t{p.client_read, p.client_write, term_discard_passthrough};
  client_t.open(dynamic{});

  const buffer reply{'O', 'K'};
  conn->send(reply);

  buffer got;
  ASSERT_TRUE(client_t.receive(got, std::chrono::milliseconds{2000}));
  EXPECT_EQ(got, reply);
}

TEST(TermTransport, EmptyFrameRoundTrip) {
  duplex_pipes p{};
  ASSERT_TRUE(make_duplex_pipes(p));

  term_server_transport server_t{p.server_read, p.server_write, term_discard_passthrough};
  server_t.start(dynamic{});
  auto conn = server_t.accept();
  ASSERT_TRUE(conn != nullptr);

  term_client_transport client_t{p.client_read, p.client_write, term_discard_passthrough};
  client_t.open(dynamic{});

  client_t.send(buffer{});

  buffer received;
  ASSERT_TRUE(conn->receive(received, std::chrono::milliseconds{2000}));
  EXPECT_TRUE(received.empty());
}

TEST(TermTransport, LargeFrameSpansMultipleOscChunks) {
  // Comfortably larger than kMaxOscSequenceBytes (4096), forcing send_frame()
  // to split this into more than one \x1b]99;<seq>;<total>;...\x07 sequence.
  // Round-tripping it correctly proves reassembly across chunks works.
  duplex_pipes p{};
  ASSERT_TRUE(make_duplex_pipes(p));

  term_server_transport server_t{p.server_read, p.server_write, term_discard_passthrough};
  server_t.start(dynamic{});
  auto conn = server_t.accept();
  ASSERT_TRUE(conn != nullptr);

  term_client_transport client_t{p.client_read, p.client_write, term_discard_passthrough};
  client_t.open(dynamic{});

  buffer frame;
  frame.reserve(20000);
  for (size_t i = 0; i < 20000; ++i)
    frame.push_back(static_cast<uint8_t>(i % 256));
  client_t.send(frame);

  buffer received;
  ASSERT_TRUE(conn->receive(received, std::chrono::milliseconds{2000}));
  EXPECT_EQ(received, frame);
}

TEST(TermTransport, LargeFrameServerToClientRoundTrip) {
  // Comfortably larger than kMaxOscSequenceBytes (4096) -- the client->server
  // path would have to split a frame this size into multiple OSC-99 chunks,
  // but the server->client path uses the un-chunked BISON<...> marker
  // format, which has no such cap. Round-tripping it correctly proves the
  // marker path handles large payloads in a single frame.
  duplex_pipes p{};
  ASSERT_TRUE(make_duplex_pipes(p));

  term_server_transport server_t{p.server_read, p.server_write, term_discard_passthrough};
  server_t.start(dynamic{});
  auto conn = server_t.accept();
  ASSERT_TRUE(conn != nullptr);

  term_client_transport client_t{p.client_read, p.client_write, term_discard_passthrough};
  client_t.open(dynamic{});

  buffer frame;
  frame.reserve(20000);
  for (size_t i = 0; i < 20000; ++i)
    frame.push_back(static_cast<uint8_t>(i % 256));
  conn->send(frame);

  buffer received;
  ASSERT_TRUE(client_t.receive(received, std::chrono::milliseconds{2000}));
  EXPECT_EQ(received, frame);
}

TEST(TermTransport, IsClosedAfterClose) {
  duplex_pipes p{};
  ASSERT_TRUE(make_duplex_pipes(p));

  term_server_transport server_t{p.server_read, p.server_write, term_discard_passthrough};
  server_t.start(dynamic{});
  auto conn = server_t.accept();
  ASSERT_TRUE(conn != nullptr);

  EXPECT_FALSE(conn->is_closed());
  conn->close();
  EXPECT_TRUE(conn->is_closed());
}

TEST(TermTransport, ShutdownPreventsClientReceive) {
  duplex_pipes p{};
  ASSERT_TRUE(make_duplex_pipes(p));

  term_client_transport client_t{p.client_read, p.client_write, term_discard_passthrough};
  client_t.open(dynamic{});
  client_t.shutdown();

  buffer frame;
  EXPECT_FALSE(client_t.receive(frame, std::chrono::milliseconds{50}));
}

TEST(TermTransport, PassthroughReceivesNonOscBytes) {
  duplex_pipes p{};
  ASSERT_TRUE(make_duplex_pipes(p));

  std::mutex m;
  std::string collected;
  term_passthrough_cb collect = [&](std::string_view chunk) {
    std::lock_guard<std::mutex> lock(m);
    collected.append(chunk);
  };

  term_server_transport server_t{p.server_read, p.server_write, collect};
  server_t.start(dynamic{});
  auto conn = server_t.accept();
  ASSERT_TRUE(conn != nullptr);

  write_raw(p.client_write, "hello prompt: ");

  for (int i = 0; i < 100 && collected.find("hello prompt: ") == std::string::npos; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds{10});

  std::lock_guard<std::mutex> lock(m);
  EXPECT_NE(collected.find("hello prompt: "), std::string::npos);
}

TEST(TermTransport, PassthroughAndFrameCoexist) {
  duplex_pipes p{};
  ASSERT_TRUE(make_duplex_pipes(p));

  std::mutex m;
  std::string collected;
  term_passthrough_cb collect = [&](std::string_view chunk) {
    std::lock_guard<std::mutex> lock(m);
    collected.append(chunk);
  };

  term_server_transport server_t{p.server_read, p.server_write, collect};
  server_t.start(dynamic{});
  auto conn = server_t.accept();
  ASSERT_TRUE(conn != nullptr);

  write_raw(p.client_write, "shell output\n");

  term_client_transport client_t{p.client_read, p.client_write, term_discard_passthrough};
  client_t.open(dynamic{});
  const buffer frame{'p', 'i', 'n', 'g'};
  client_t.send(frame);

  buffer received;
  ASSERT_TRUE(conn->receive(received, std::chrono::milliseconds{2000}));
  EXPECT_EQ(received, frame);

  for (int i = 0; i < 100 && collected.find("shell output\n") == std::string::npos; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds{10});

  std::lock_guard<std::mutex> lock(m);
  EXPECT_NE(collected.find("shell output\n"), std::string::npos);
}

TEST(TermTransport, MalformedOscSequenceIsDiscardedNotDelivered) {
  duplex_pipes p{};
  ASSERT_TRUE(make_duplex_pipes(p));

  term_server_transport server_t{p.server_read, p.server_write, term_discard_passthrough};
  server_t.start(dynamic{});
  auto conn = server_t.accept();
  ASSERT_TRUE(conn != nullptr);

  // Well-formed OSC-99 shape but garbage seq/total fields — should be logged
  // and dropped, not delivered as (or corrupt) a frame.
  write_raw(p.client_write, "\x1b]99;bad;bad;bad\x07");

  buffer received;
  EXPECT_FALSE(conn->receive(received, std::chrono::milliseconds{200}));

  // The connection must still work normally afterwards.
  term_client_transport client_t{p.client_read, p.client_write, term_discard_passthrough};
  client_t.open(dynamic{});
  const buffer frame{'o', 'k'};
  client_t.send(frame);
  ASSERT_TRUE(conn->receive(received, std::chrono::milliseconds{2000}));
  EXPECT_EQ(received, frame);
}

TEST(TermTransport, StalledReassemblyDiscardedWithoutCorruptingNextEnvelope) {
  duplex_pipes p{};
  ASSERT_TRUE(make_duplex_pipes(p));

  term_server_transport server_t{p.server_read, p.server_write, term_discard_passthrough};
  server_t.start(dynamic{});
  auto conn = server_t.accept();
  ASSERT_TRUE(conn != nullptr);

  // Start a 3-chunk envelope (seq=0/total=3) but never send chunks 1 or 2 —
  // then start a fresh single-chunk envelope. Only the fresh one should ever
  // be delivered.
  write_raw(p.client_write, "\x1b]99;0;3;aGk=\x07"); // "hi" base64, claims total=3

  buffer received;
  EXPECT_FALSE(conn->receive(received, std::chrono::milliseconds{200}));

  term_client_transport client_t{p.client_read, p.client_write, term_discard_passthrough};
  client_t.open(dynamic{});
  const buffer frame{'f', 'r', 'e', 's', 'h'};
  client_t.send(frame);

  ASSERT_TRUE(conn->receive(received, std::chrono::milliseconds{2000}));
  EXPECT_EQ(received, frame);
}

TEST(TermTransport, ClientSendStringViewBypassesFraming) {
  duplex_pipes p{};
  ASSERT_TRUE(make_duplex_pipes(p));

  term_client_transport client_t{p.client_read, p.client_write, term_discard_passthrough};
  client_t.open(dynamic{});

  client_t.send(std::string_view{"raw echo"});

  char buf[64]{};
  const auto n = read_raw(p.server_read, buf, sizeof(buf));
  ASSERT_GT(n, 0);
  EXPECT_EQ(std::string(buf, static_cast<size_t>(n)), "raw echo");
}

TEST(TermTransport, ClientToServerWireFormatIsOsc99) {
  // Pins the client->server format documented in FORMAT.md §5.3. Only a
  // term_client_transport is created here (no term_server_transport, whose
  // background reader would otherwise race this test for bytes on
  // p.server_read) -- same pattern as ClientSendStringViewBypassesFraming.
  duplex_pipes p{};
  ASSERT_TRUE(make_duplex_pipes(p));

  term_client_transport client_t{p.client_read, p.client_write, term_discard_passthrough};
  client_t.open(dynamic{});

  client_t.send(buffer{'p', 'i', 'n', 'g'});
  char c2s_buf[256]{};
  const auto c2s_n = read_raw(p.server_read, c2s_buf, sizeof(c2s_buf));
  ASSERT_GT(c2s_n, 0);
  const std::string c2s_wire(c2s_buf, static_cast<size_t>(c2s_n));
  EXPECT_EQ(c2s_wire.rfind("\x1b]99;", 0), 0U);
}

TEST(TermTransport, ServerToClientWireFormatIsMarker) {
  // Pins the server->client format documented in FORMAT.md §5.3: a bare
  // BISON<...> marker with no ESC byte, never OSC-99 (which ConPTY's input
  // pipe would silently swallow -- the root cause this fix addresses). Only
  // a term_server_transport is created here (no term_client_transport,
  // whose background reader would otherwise race this test for bytes on
  // p.client_read).
  duplex_pipes p{};
  ASSERT_TRUE(make_duplex_pipes(p));

  term_server_transport server_t{p.server_read, p.server_write, term_discard_passthrough};
  server_t.start(dynamic{});

  auto conn = server_t.accept();
  ASSERT_TRUE(conn != nullptr);

  conn->send(buffer{'p', 'o', 'n', 'g'});
  char s2c_buf[256]{};
  const auto s2c_n = read_raw(p.client_read, s2c_buf, sizeof(s2c_buf));
  ASSERT_GT(s2c_n, 0);
  const std::string s2c_wire(s2c_buf, static_cast<size_t>(s2c_n));
  EXPECT_EQ(s2c_wire.rfind("BISON<", 0), 0U);
  EXPECT_EQ(s2c_wire.find('\x1b'), std::string::npos);
}

TEST(TermTransport, ShutdownFromReaderThreadOnEofDoesNotDeadlock) {
  // Regression test: on EOF, term_reader::on_read() calls passthrough()
  // synchronously from its own reader loop thread (see term_transport.cpp).
  // If a passthrough callback reacts to the empty "closed" chunk by tearing
  // the transport down (e.g. client::disconnect() -> transport->shutdown()
  // in a real client wired to detect server disconnects), that teardown
  // runs on the very thread it's trying to stop. Before the
  // term_pipe_thread::stop() self-join guard, this made loop_thread.join()
  // a thread joining itself, which throws std::system_error("Resource
  // deadlock avoided") out of the thread function -> std::terminate() ->
  // process abort. Simply reaching the end of this test (instead of the
  // process aborting) is the assertion.
  duplex_pipes p{};
  ASSERT_TRUE(make_duplex_pipes(p));

  std::unique_ptr<term_client_transport> client_t;
  term_passthrough_cb self_shutdown = [&](std::string_view chunk) {
    if (chunk.empty() && client_t)
      client_t->shutdown();
  };

  client_t = std::make_unique<term_client_transport>(p.client_read, p.client_write, self_shutdown);
  client_t->open(dynamic{});

  // Closing the peer's write end delivers EOF to client_t's reader thread.
  close_raw(p.server_write);

  for (int i = 0; i < 200 && client_t->is_connected(); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds{10});

  EXPECT_FALSE(client_t->is_connected());
}

// ═════════════════════════════════════════════════════════════════════════════
// Connect timeout (is_connected(); see FORMAT.md §5.2.3)
// ═════════════════════════════════════════════════════════════════════════════

TEST(TermTransportConnect, IsConnectedStaysTrueBeforeDeadlineWithNoFrame) {
  duplex_pipes p{};
  ASSERT_TRUE(make_duplex_pipes(p));

  term_client_transport client_t{
      p.client_read, p.client_write, term_discard_passthrough, std::chrono::milliseconds{2000}};
  client_t.open(dynamic{});
  EXPECT_TRUE(client_t.is_connected());
}

TEST(TermTransportConnect, IsConnectedBecomesFalseAfterDeadlineWithNoFrame) {
  duplex_pipes p{};
  ASSERT_TRUE(make_duplex_pipes(p));

  term_client_transport client_t{
      p.client_read, p.client_write, term_discard_passthrough, std::chrono::milliseconds{100}};
  client_t.open(dynamic{});

  for (int i = 0; i < 100 && client_t.is_connected(); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds{10});

  EXPECT_FALSE(client_t.is_connected());
}

TEST(TermTransportConnect, IsConnectedStaysTrueAfterDeadlineOnceAFrameArrived) {
  duplex_pipes p{};
  ASSERT_TRUE(make_duplex_pipes(p));

  term_server_transport server_t{p.server_read, p.server_write, term_discard_passthrough};
  server_t.start(dynamic{});
  auto conn = server_t.accept();
  ASSERT_TRUE(conn != nullptr);

  term_client_transport client_t{
      p.client_read, p.client_write, term_discard_passthrough, std::chrono::milliseconds{100}};
  client_t.open(dynamic{});
  conn->send(buffer{'o', 'k'});

  buffer received;
  ASSERT_TRUE(client_t.receive(received, std::chrono::milliseconds{2000}));

  std::this_thread::sleep_for(std::chrono::milliseconds{150}); // past the 100ms deadline
  EXPECT_TRUE(client_t.is_connected());
}
