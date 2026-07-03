// MIT License © 2025 Binary Dice Games
// Tests for bdg::bison::rmi::transport::stdio_transport (BISON<...> framing
// over a pair of raw fds). See FORMAT.md for the wire framing contract.

#include "src/rmi/transport/stdio_transport.hpp"

#include <gtest/gtest.h>

#include <chrono>
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

/** @brief Blocking read of whatever's currently available; -1 on error. */
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
// StdioTransport
// ═════════════════════════════════════════════════════════════════════════════

TEST(StdioTransport, ClientSendServerReceive) {
  duplex_pipes p{};
  ASSERT_TRUE(make_duplex_pipes(p));

  stdio_server_transport server_t{p.server_read, p.server_write, stdio_discard_passthrough};
  server_t.start(dynamic{});
  auto conn = server_t.accept();
  ASSERT_TRUE(conn != nullptr);

  stdio_client_transport client_t{p.client_read, p.client_write, stdio_discard_passthrough};
  client_t.open(dynamic{});

  const buffer frame{'H', 'e', 'l', 'l', 'o'};
  client_t.send(frame);

  buffer received;
  ASSERT_TRUE(conn->receive(received, std::chrono::milliseconds{2000}));
  EXPECT_EQ(received, frame);
}

TEST(StdioTransport, ServerSendClientReceive) {
  duplex_pipes p{};
  ASSERT_TRUE(make_duplex_pipes(p));

  stdio_server_transport server_t{p.server_read, p.server_write, stdio_discard_passthrough};
  server_t.start(dynamic{});
  auto conn = server_t.accept();
  ASSERT_TRUE(conn != nullptr);

  stdio_client_transport client_t{p.client_read, p.client_write, stdio_discard_passthrough};
  client_t.open(dynamic{});

  const buffer reply{'O', 'K'};
  conn->send(reply);

  buffer got;
  ASSERT_TRUE(client_t.receive(got, std::chrono::milliseconds{2000}));
  EXPECT_EQ(got, reply);
}

TEST(StdioTransport, EmptyFrameRoundTrip) {
  duplex_pipes p{};
  ASSERT_TRUE(make_duplex_pipes(p));

  stdio_server_transport server_t{p.server_read, p.server_write, stdio_discard_passthrough};
  server_t.start(dynamic{});
  auto conn = server_t.accept();
  ASSERT_TRUE(conn != nullptr);

  stdio_client_transport client_t{p.client_read, p.client_write, stdio_discard_passthrough};
  client_t.open(dynamic{});

  client_t.send(buffer{});

  buffer received;
  ASSERT_TRUE(conn->receive(received, std::chrono::milliseconds{2000}));
  EXPECT_TRUE(received.empty());
}

TEST(StdioTransport, AcceptReturnsNullptrWhileAConnectionIsCheckedOut) {
  duplex_pipes p{};
  ASSERT_TRUE(make_duplex_pipes(p));

  stdio_server_transport server_t{p.server_read, p.server_write, stdio_discard_passthrough};
  server_t.start(dynamic{});

  auto first = server_t.accept();
  EXPECT_TRUE(first != nullptr);

  // Short timeout: accept() now blocks for real (see stdio_transport.cpp's
  // accept()), so this only needs to prove it times out, not wait 5s for it.
  auto second = server_t.accept(std::chrono::milliseconds{50});
  EXPECT_EQ(second, nullptr);
}

TEST(StdioTransport, AcceptSucceedsAgainAfterConnectionCloses) {
  duplex_pipes p{};
  ASSERT_TRUE(make_duplex_pipes(p));

  stdio_server_transport server_t{p.server_read, p.server_write, stdio_discard_passthrough};
  server_t.start(dynamic{});

  auto first = server_t.accept();
  ASSERT_TRUE(first != nullptr);
  first->close();

  auto second = server_t.accept();
  EXPECT_TRUE(second != nullptr);

  // The underlying reader/writer survive across the reconnect: a frame sent
  // after the second connection is accepted is still received correctly,
  // which wouldn't be true if closing the first connection had torn down
  // the shared fds (the pty-session-survives-disconnect scenario this
  // supports — see src/pty/DESIGN.md).
  stdio_client_transport client_t{p.client_read, p.client_write, stdio_discard_passthrough};
  client_t.open(dynamic{});
  const buffer frame{'a', 'g', 'a', 'i', 'n'};
  client_t.send(frame);

  buffer received;
  ASSERT_TRUE(second->receive(received, std::chrono::milliseconds{2000}));
  EXPECT_EQ(received, frame);
}

TEST(StdioTransport, AcceptWakesPromptlyWhenCheckedOutConnectionCloses) {
  // Regression test: accept() must block on a condition variable instead of
  // returning nullptr immediately (which would leave callers like
  // server::accept_loop() spinning at 100% CPU re-polling accept() with no
  // sleep of its own).
  duplex_pipes p{};
  ASSERT_TRUE(make_duplex_pipes(p));

  stdio_server_transport server_t{p.server_read, p.server_write, stdio_discard_passthrough};
  server_t.start(dynamic{});

  auto first = server_t.accept();
  ASSERT_TRUE(first != nullptr);

  std::unique_ptr<server_connection_iface> second;
  std::thread waiter([&] { second = server_t.accept(std::chrono::milliseconds{5000}); });

  std::this_thread::sleep_for(std::chrono::milliseconds{100});
  const auto start = std::chrono::steady_clock::now();
  first->close();
  waiter.join();
  const auto elapsed = std::chrono::steady_clock::now() - start;

  EXPECT_TRUE(second != nullptr);
  EXPECT_LT(elapsed, std::chrono::milliseconds{1000});
}

TEST(StdioTransport, IsClosedAfterClose) {
  duplex_pipes p{};
  ASSERT_TRUE(make_duplex_pipes(p));

  stdio_server_transport server_t{p.server_read, p.server_write, stdio_discard_passthrough};
  server_t.start(dynamic{});
  auto conn = server_t.accept();
  ASSERT_TRUE(conn != nullptr);

  EXPECT_FALSE(conn->is_closed());
  conn->close();
  EXPECT_TRUE(conn->is_closed());
}

TEST(StdioTransport, ShutdownPreventsClientReceive) {
  duplex_pipes p{};
  ASSERT_TRUE(make_duplex_pipes(p));

  // open() now performs a connect-time handshake (see FORMAT.md and
  // stdio_client_transport::open()'s doc comment) and blocks until it sees
  // "BISON/1.0 OK\n" from the peer. There's no stdio_server_transport here
  // to answer it automatically, so fake a minimal peer response directly.
  write_raw(p.server_write, "BISON/1.0 OK\r\n");

  stdio_client_transport client_t{p.client_read, p.client_write, stdio_discard_passthrough};
  client_t.open(dynamic{});
  client_t.shutdown();

  buffer frame;
  EXPECT_FALSE(client_t.receive(frame, std::chrono::milliseconds{50}));
}

TEST(StdioTransport, PassthroughReceivesNonFrameBytes) {
  duplex_pipes p{};
  ASSERT_TRUE(make_duplex_pipes(p));

  std::mutex m;
  std::string collected;
  stdio_passthrough_cb collect = [&](std::string_view chunk) {
    std::lock_guard<std::mutex> lock(m);
    collected.append(chunk);
  };

  stdio_server_transport server_t{p.server_read, p.server_write, collect};
  server_t.start(dynamic{});
  auto conn = server_t.accept();
  ASSERT_TRUE(conn != nullptr);

  write_raw(p.client_write, "hello prompt: ");

  // Poll briefly: the scanner is byte-level so this should show up quickly,
  // well before any line terminator arrives.
  for (int i = 0; i < 100 && collected.find("hello prompt: ") == std::string::npos; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds{10});

  std::lock_guard<std::mutex> lock(m);
  EXPECT_NE(collected.find("hello prompt: "), std::string::npos);
}

TEST(StdioTransport, PassthroughAndFrameCoexist) {
  duplex_pipes p{};
  ASSERT_TRUE(make_duplex_pipes(p));

  std::mutex m;
  std::string collected;
  stdio_passthrough_cb collect = [&](std::string_view chunk) {
    std::lock_guard<std::mutex> lock(m);
    collected.append(chunk);
  };

  stdio_server_transport server_t{p.server_read, p.server_write, collect};
  server_t.start(dynamic{});
  auto conn = server_t.accept();
  ASSERT_TRUE(conn != nullptr);

  write_raw(p.client_write, "shell output\n");

  stdio_client_transport client_t{p.client_read, p.client_write, stdio_discard_passthrough};
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

TEST(StdioTransport, MalformedBase64FrameIsPassedThroughAsText) {
  duplex_pipes p{};
  ASSERT_TRUE(make_duplex_pipes(p));

  std::mutex m;
  std::string collected;
  stdio_passthrough_cb collect = [&](std::string_view chunk) {
    std::lock_guard<std::mutex> lock(m);
    collected.append(chunk);
  };

  stdio_server_transport server_t{p.server_read, p.server_write, collect};
  server_t.start(dynamic{});
  auto conn = server_t.accept();
  ASSERT_TRUE(conn != nullptr);

  // "!!!" is not valid base64 (invalid characters), so the scanner should
  // treat the completed frame as malformed and hand it to the passthrough
  // callback instead of enqueuing a decoded frame.
  write_raw(p.client_write, "BISON<!!!>");

  for (int i = 0; i < 100 && collected.find("BISON<!!!>") == std::string::npos; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds{10});

  std::lock_guard<std::mutex> lock(m);
  EXPECT_NE(collected.find("BISON<!!!>"), std::string::npos);
}

// ═════════════════════════════════════════════════════════════════════════════
// Connect-time handshake (START BISON/1.0 / BISON/1.0 OK / STOP BISON/1.0)
// See FORMAT.md and stdio_client_transport::open()'s doc comment.
// ═════════════════════════════════════════════════════════════════════════════

TEST(StdioTransportHandshake, OpenSendsStartAndSucceedsWhenPeerReplies) {
  duplex_pipes p{};
  ASSERT_TRUE(make_duplex_pipes(p));

  write_raw(p.server_write, "BISON/1.0 OK\r\n");

  stdio_client_transport client_t{p.client_read, p.client_write, stdio_discard_passthrough};
  EXPECT_NO_THROW(client_t.open(dynamic{}));

  char buf[64]{};
  const auto n = read_raw(p.server_read, buf, sizeof(buf));
  ASSERT_GT(n, 0);
  EXPECT_EQ(std::string(buf, static_cast<size_t>(n)), "START BISON/1.0\r\n");
}

TEST(StdioTransportHandshake, OpenThrowsWhenNoPeerResponds) {
  duplex_pipes p{};
  ASSERT_TRUE(make_duplex_pipes(p));

  // The handshake timeout is a constructor parameter specifically so tests
  // don't have to wait out the real (5s) production default.
  stdio_client_transport client_t{p.client_read, p.client_write, stdio_discard_passthrough,
                                   std::chrono::milliseconds{100}};
  EXPECT_THROW(client_t.open(dynamic{}), std::runtime_error);
}

TEST(StdioTransportHandshake, HandshakeOkLineIsNotForwardedToPassthrough) {
  duplex_pipes p{};
  ASSERT_TRUE(make_duplex_pipes(p));

  std::mutex m;
  std::string collected;
  stdio_passthrough_cb collect = [&](std::string_view chunk) {
    std::lock_guard<std::mutex> lock(m);
    collected.append(chunk);
  };

  write_raw(p.server_write, "BISON/1.0 OK\r\nafter\n");

  stdio_client_transport client_t{p.client_read, p.client_write, collect};
  ASSERT_NO_THROW(client_t.open(dynamic{}));

  for (int i = 0; i < 100; ++i) {
    {
      std::lock_guard<std::mutex> lock(m);
      if (collected.find("after") != std::string::npos)
        break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
  }

  std::lock_guard<std::mutex> lock(m);
  EXPECT_EQ(collected.find("BISON/1.0 OK"), std::string::npos)
      << "the handshake ack line must be withheld from the caller's passthrough, "
         "e.g. so it can't be mistaken for a typed REPL command";
  EXPECT_NE(collected.find("after"), std::string::npos) << "bytes following the ack line must still be delivered";
}

TEST(StdioTransportHandshake, ServerRepliesOkWhenClientOpens) {
  duplex_pipes p{};
  ASSERT_TRUE(make_duplex_pipes(p));

  stdio_server_transport server_t{p.server_read, p.server_write, stdio_discard_passthrough};
  server_t.start(dynamic{});
  auto conn = server_t.accept();
  ASSERT_TRUE(conn != nullptr);

  stdio_client_transport client_t{p.client_read, p.client_write, stdio_discard_passthrough};
  EXPECT_NO_THROW(client_t.open(dynamic{}));
}

TEST(StdioTransportHandshake, ServerReplyDoesNotArriveAsADecodedFrame) {
  duplex_pipes p{};
  ASSERT_TRUE(make_duplex_pipes(p));

  stdio_server_transport server_t{p.server_read, p.server_write, stdio_discard_passthrough};
  server_t.start(dynamic{});
  auto conn = server_t.accept();
  ASSERT_TRUE(conn != nullptr);

  stdio_client_transport client_t{p.client_read, p.client_write, stdio_discard_passthrough};
  ASSERT_NO_THROW(client_t.open(dynamic{}));

  // A real frame sent right after the handshake must still be the *first*
  // thing the server decodes — the handshake exchange must not leak into
  // (or get confused with) normal frame decoding on either side.
  const buffer frame{'p', 'i', 'n', 'g'};
  client_t.send(frame);
  buffer received;
  ASSERT_TRUE(conn->receive(received, std::chrono::milliseconds{2000}));
  EXPECT_EQ(received, frame);
}

TEST(StdioTransportHandshake, ShutdownSendsStop) {
  duplex_pipes p{};
  ASSERT_TRUE(make_duplex_pipes(p));

  write_raw(p.server_write, "BISON/1.0 OK\r\n");

  stdio_client_transport client_t{p.client_read, p.client_write, stdio_discard_passthrough};
  ASSERT_NO_THROW(client_t.open(dynamic{})); // consumes "START BISON/1.0\r\n" off p.server_read
  client_t.shutdown();

  // shutdown() enqueues "STOP BISON/1.0\r\n" asynchronously before stopping the
  // writer, so read (blocking, possibly more than once) rather than assume
  // one read call is enough to have caught up with it.
  std::string received;
  char buf[64]{};
  while (received.find("STOP BISON/1.0\r\n") == std::string::npos) {
    const auto n = read_raw(p.server_read, buf, sizeof(buf));
    ASSERT_GT(n, 0);
    received.append(buf, static_cast<size_t>(n));
  }
  EXPECT_NE(received.find("STOP BISON/1.0\r\n"), std::string::npos);
}
