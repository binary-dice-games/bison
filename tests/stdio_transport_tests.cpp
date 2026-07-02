// MIT License © 2025 Binary Dice Games
// Tests for bdg::bison::rmi::transport::stdio_transport (BISON: line framing
// over a pair of raw fds). See FORMAT.md for the wire framing contract.

#include "src/rmi/transport/stdio_transport.hpp"

#include <gtest/gtest.h>

#include <chrono>
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
  return _pipe(fds, 65536, _O_BINARY) == 0;
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

TEST(StdioTransport, AcceptReturnsSingleConnection) {
  duplex_pipes p{};
  ASSERT_TRUE(make_duplex_pipes(p));

  stdio_server_transport server_t{p.server_read, p.server_write, stdio_discard_passthrough};
  server_t.start(dynamic{});

  auto first = server_t.accept();
  EXPECT_TRUE(first != nullptr);

  auto second = server_t.accept();
  EXPECT_EQ(second, nullptr);
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
  write_raw(p.client_write, "\nBISON:!!!\n");

  for (int i = 0; i < 100 && collected.find("BISON:!!!\n") == std::string::npos; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds{10});

  std::lock_guard<std::mutex> lock(m);
  EXPECT_NE(collected.find("BISON:!!!\n"), std::string::npos);
}
