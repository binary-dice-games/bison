// MIT License © 2025 Binary Dice Games
// Tests for bdg::bison::rmi::transport::term_transport (OSC-99-chunked
// framing over a pair of raw fds). See FORMAT.md §5.3 for the wire framing
// contract.

#include "src/rmi/transport/term_transport.hpp"

#include <gtest/gtest.h>

#include <mutex>
#include <string>
#include <thread>

#include <unistd.h>

using namespace bdg::bison;
using namespace bdg::bison::rmi::transport;

namespace {

bool make_pipe(int fds[2]) {
  return pipe(fds) == 0;
}

void write_raw(int fd, const std::string& data) {
  ASSERT_EQ(write(fd, data.data(), data.size()), static_cast<ssize_t>(data.size()));
}

long read_raw(int fd, char* buf, size_t len) {
  return read(fd, buf, len);
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

  term_server_transport server_t{p.server_read, p.server_write, stdio_discard_passthrough};
  server_t.start(dynamic{});
  auto conn = server_t.accept();
  ASSERT_TRUE(conn != nullptr);

  term_client_transport client_t{p.client_read, p.client_write, stdio_discard_passthrough};
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

  term_server_transport server_t{p.server_read, p.server_write, stdio_discard_passthrough};
  server_t.start(dynamic{});
  auto conn = server_t.accept();
  ASSERT_TRUE(conn != nullptr);

  term_client_transport client_t{p.client_read, p.client_write, stdio_discard_passthrough};
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

  term_server_transport server_t{p.server_read, p.server_write, stdio_discard_passthrough};
  server_t.start(dynamic{});
  auto conn = server_t.accept();
  ASSERT_TRUE(conn != nullptr);

  term_client_transport client_t{p.client_read, p.client_write, stdio_discard_passthrough};
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

  term_server_transport server_t{p.server_read, p.server_write, stdio_discard_passthrough};
  server_t.start(dynamic{});
  auto conn = server_t.accept();
  ASSERT_TRUE(conn != nullptr);

  term_client_transport client_t{p.client_read, p.client_write, stdio_discard_passthrough};
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

TEST(TermTransport, IsClosedAfterClose) {
  duplex_pipes p{};
  ASSERT_TRUE(make_duplex_pipes(p));

  term_server_transport server_t{p.server_read, p.server_write, stdio_discard_passthrough};
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

  write_raw(p.server_write, "BISON/1.0 OK\r\n");

  term_client_transport client_t{p.client_read, p.client_write, stdio_discard_passthrough};
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
  stdio_passthrough_cb collect = [&](std::string_view chunk) {
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
  stdio_passthrough_cb collect = [&](std::string_view chunk) {
    std::lock_guard<std::mutex> lock(m);
    collected.append(chunk);
  };

  term_server_transport server_t{p.server_read, p.server_write, collect};
  server_t.start(dynamic{});
  auto conn = server_t.accept();
  ASSERT_TRUE(conn != nullptr);

  write_raw(p.client_write, "shell output\n");

  term_client_transport client_t{p.client_read, p.client_write, stdio_discard_passthrough};
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

  term_server_transport server_t{p.server_read, p.server_write, stdio_discard_passthrough};
  server_t.start(dynamic{});
  auto conn = server_t.accept();
  ASSERT_TRUE(conn != nullptr);

  // Well-formed OSC-99 shape but garbage seq/total fields — should be logged
  // and dropped, not delivered as (or corrupt) a frame.
  write_raw(p.client_write, "\x1b]99;bad;bad;bad\x07");

  buffer received;
  EXPECT_FALSE(conn->receive(received, std::chrono::milliseconds{200}));

  // The connection must still work normally afterwards.
  term_client_transport client_t{p.client_read, p.client_write, stdio_discard_passthrough};
  client_t.open(dynamic{});
  const buffer frame{'o', 'k'};
  client_t.send(frame);
  ASSERT_TRUE(conn->receive(received, std::chrono::milliseconds{2000}));
  EXPECT_EQ(received, frame);
}

TEST(TermTransport, StalledReassemblyDiscardedWithoutCorruptingNextEnvelope) {
  duplex_pipes p{};
  ASSERT_TRUE(make_duplex_pipes(p));

  term_server_transport server_t{p.server_read, p.server_write, stdio_discard_passthrough};
  server_t.start(dynamic{});
  auto conn = server_t.accept();
  ASSERT_TRUE(conn != nullptr);

  // Start a 3-chunk envelope (seq=0/total=3) but never send chunks 1 or 2 —
  // then start a fresh single-chunk envelope. Only the fresh one should ever
  // be delivered.
  write_raw(p.client_write, "\x1b]99;0;3;aGk=\x07"); // "hi" base64, claims total=3

  buffer received;
  EXPECT_FALSE(conn->receive(received, std::chrono::milliseconds{200}));

  term_client_transport client_t{p.client_read, p.client_write, stdio_discard_passthrough};
  client_t.open(dynamic{});
  const buffer frame{'f', 'r', 'e', 's', 'h'};
  client_t.send(frame);

  ASSERT_TRUE(conn->receive(received, std::chrono::milliseconds{2000}));
  EXPECT_EQ(received, frame);
}

TEST(TermTransport, ClientSendStringViewBypassesFraming) {
  duplex_pipes p{};
  ASSERT_TRUE(make_duplex_pipes(p));

  write_raw(p.server_write, "BISON/1.0 OK\r\n");

  term_client_transport client_t{p.client_read, p.client_write, stdio_discard_passthrough};
  client_t.open(dynamic{});

  // Consume the handshake's "START BISON/1.0\r\n" before checking raw echo.
  char buf[64]{};
  const auto n = read_raw(p.server_read, buf, sizeof(buf));
  ASSERT_GT(n, 0);

  client_t.send(std::string_view{"raw echo"});

  char buf2[64]{};
  const auto n2 = read_raw(p.server_read, buf2, sizeof(buf2));
  ASSERT_GT(n2, 0);
  EXPECT_EQ(std::string(buf2, static_cast<size_t>(n2)), "raw echo");
}

// ═════════════════════════════════════════════════════════════════════════════
// Connect-time handshake (reused verbatim from stdio_transport — see FORMAT.md)
// ═════════════════════════════════════════════════════════════════════════════

TEST(TermTransportHandshake, OpenSendsStartAndSucceedsWhenPeerReplies) {
  duplex_pipes p{};
  ASSERT_TRUE(make_duplex_pipes(p));

  write_raw(p.server_write, "BISON/1.0 OK\r\n");

  term_client_transport client_t{p.client_read, p.client_write, stdio_discard_passthrough};
  EXPECT_NO_THROW(client_t.open(dynamic{}));

  char buf[64]{};
  const auto n = read_raw(p.server_read, buf, sizeof(buf));
  ASSERT_GT(n, 0);
  EXPECT_EQ(std::string(buf, static_cast<size_t>(n)), "START BISON/1.0\r\n");
}

TEST(TermTransportHandshake, OpenThrowsWhenNoPeerResponds) {
  duplex_pipes p{};
  ASSERT_TRUE(make_duplex_pipes(p));

  term_client_transport client_t{p.client_read, p.client_write, stdio_discard_passthrough,
                                  std::chrono::milliseconds{100}};
  EXPECT_THROW(client_t.open(dynamic{}), std::runtime_error);
}

TEST(TermTransportHandshake, ServerRepliesOkWhenClientOpens) {
  duplex_pipes p{};
  ASSERT_TRUE(make_duplex_pipes(p));

  term_server_transport server_t{p.server_read, p.server_write, stdio_discard_passthrough};
  server_t.start(dynamic{});
  auto conn = server_t.accept();
  ASSERT_TRUE(conn != nullptr);

  term_client_transport client_t{p.client_read, p.client_write, stdio_discard_passthrough};
  EXPECT_NO_THROW(client_t.open(dynamic{}));
}

TEST(TermTransportHandshake, ShutdownSendsStop) {
  duplex_pipes p{};
  ASSERT_TRUE(make_duplex_pipes(p));

  write_raw(p.server_write, "BISON/1.0 OK\r\n");

  term_client_transport client_t{p.client_read, p.client_write, stdio_discard_passthrough};
  ASSERT_NO_THROW(client_t.open(dynamic{})); // consumes "START BISON/1.0\r\n" off p.server_read
  client_t.shutdown();

  std::string received;
  char buf[64]{};
  while (received.find("STOP BISON/1.0\r\n") == std::string::npos) {
    const auto n = read_raw(p.server_read, buf, sizeof(buf));
    ASSERT_GT(n, 0);
    received.append(buf, static_cast<size_t>(n));
  }
  EXPECT_NE(received.find("STOP BISON/1.0\r\n"), std::string::npos);
}
