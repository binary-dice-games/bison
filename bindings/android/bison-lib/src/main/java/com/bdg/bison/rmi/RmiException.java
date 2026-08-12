// MIT License © 2025 Binary Dice Games
// Permission is hereby granted, free of charge, to use, copy, modify, and
// distribute this file. See the LICENSE file or
// https://opensource.org/licenses/MIT for details.

package com.bdg.bison.rmi;

/**
 * Thrown when an {@code rmi_c.h} call fails. {@link #code} is the raw
 * {@code rmi_error} value (see {@code include/rmi_c.h}), e.g.
 * {@code RMI_ERR_TIMEOUT} (-3) or {@code RMI_ERR_TRANSPORT} (-5).
 */
public final class RmiException extends RuntimeException {
  public final int code;

  public RmiException(int code, String message) {
    super(message);
    this.code = code;
  }
}
