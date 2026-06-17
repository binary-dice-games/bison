// MIT License © 2025 Binary Dice Games
// Google Test suite for the pure-C PTY application scaffold API (pty_c.h).

#include "pty_c.h"
#include "rmi_c.h"

#include <gtest/gtest.h>

// ─── Helpers ──────────────────────────────────────────────────────────────────

static int noop_pty_client_on_session(rmi_client_handle, void*) {
  return 0;
}

static void noop_pty_client_on_error(const char*, void*) {}

static void noop_pty_server_register_classes(void*) {}

static void noop_pty_server_on_error(const char*, void*) {}

// ═════════════════════════════════════════════════════════════════════════════
// PTY C API — argument validation
// ═════════════════════════════════════════════════════════════════════════════

TEST(PtyClientTests, NullCallbacksReturnsError) {
  rmi_error err = pty_client_run(0, nullptr, nullptr);
  EXPECT_EQ(err, RMI_ERR_NULL);
}

TEST(PtyClientTests, MissingSessionCallbackReturnsError) {
  pty_client_callbacks callbacks{};
  callbacks.on_error = noop_pty_client_on_error;
  rmi_error err = pty_client_run(0, nullptr, &callbacks);
  EXPECT_EQ(err, RMI_ERR_NULL);
}

TEST(PtyClientTests, NullArgvWithArgcReturnsError) {
  pty_client_callbacks callbacks{};
  callbacks.on_session = noop_pty_client_on_session;
  callbacks.on_error   = noop_pty_client_on_error;
  rmi_error err = pty_client_run(1, nullptr, &callbacks);
  EXPECT_EQ(err, RMI_ERR_NULL);
}

TEST(PtyServerTests, NullCallbacksReturnsError) {
  rmi_error err = pty_server_run(0, nullptr, nullptr);
  EXPECT_EQ(err, RMI_ERR_NULL);
}

TEST(PtyServerTests, MissingRegisterCallbackReturnsError) {
  pty_server_callbacks callbacks{};
  callbacks.on_error = noop_pty_server_on_error;
  rmi_error err = pty_server_run(0, nullptr, &callbacks);
  EXPECT_EQ(err, RMI_ERR_NULL);
}

TEST(PtyServerTests, NullArgvWithArgcReturnsError) {
  pty_server_callbacks callbacks{};
  callbacks.register_classes = noop_pty_server_register_classes;
  callbacks.on_error         = noop_pty_server_on_error;
  rmi_error err = pty_server_run(1, nullptr, &callbacks);
  EXPECT_EQ(err, RMI_ERR_NULL);
}
