// MIT License © 2025 Binary Dice Games
// Permission is hereby granted, free of charge, to use, copy, modify, and
// distribute this file. See the LICENSE file or
// https://opensource.org/licenses/MIT for details.

package com.bdg.bison.rmi;

import com.bdg.bison.Dynamic;

/**
 * Connection-authentication callback for {@link Server#listen}, matching
 * {@code rmi_c.h}'s {@code rmi_auth_fn}. Invoked once per incoming
 * connection with the client's {@code OP_CONNECT} payload.
 *
 * <p>{@code payload} is borrowed -- valid only for the duration of the call
 * -- and must not be {@linkplain Dynamic#close closed} by the handler.
 */
public interface AuthHandler {
  /**
   * @return an identity string (possibly empty) to accept the connection,
   *     or {@code null} to reject it -- {@code rmi_auth_fn}'s {@code
   *     identity_buf} output and {@code bool} return folded into one value.
   */
  String authenticate(Dynamic payload);
}
