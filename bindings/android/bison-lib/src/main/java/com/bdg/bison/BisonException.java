// MIT License © 2025 Binary Dice Games
// Permission is hereby granted, free of charge, to use, copy, modify, and
// distribute this file. See the LICENSE file or
// https://opensource.org/licenses/MIT for details.

package com.bdg.bison;

/**
 * Thrown when a {@code bison_c.h} call fails. {@link #code} is the raw
 * {@code bison_error} value (see {@code include/bison_c.h}), preserved for
 * callers that want to branch on the failure kind rather than parse the
 * message -- the same reasoning as the C++ binding's {@code bison_exception}
 * and the C# binding's {@code BisonException}.
 */
public final class BisonException extends RuntimeException {
  public final int code;

  public BisonException(int code, String message) {
    super(message);
    this.code = code;
  }
}
