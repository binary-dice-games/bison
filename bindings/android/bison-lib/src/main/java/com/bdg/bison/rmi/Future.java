// MIT License © 2025 Binary Dice Games
// Permission is hereby granted, free of charge, to use, copy, modify, and
// distribute this file. See the LICENSE file or
// https://opensource.org/licenses/MIT for details.

package com.bdg.bison.rmi;

import com.bdg.bison.Dynamic;
import com.bdg.bison.NativeLibrary;

/**
 * Wraps an {@code rmi_future_handle} (see {@code include/rmi_c.h}) returned
 * by an {@code *Async} call. Consumed exactly once via {@link #getDynamic}
 * or {@link #getProxy} (matching which async call produced it), or
 * discarded with {@link #close} once {@link #waitFor} confirms completion
 * for a call with no result value (e.g. {@code Proxy#setAsync}/{@code
 * clearAsync}).
 */
public final class Future implements AutoCloseable {
  static {
    NativeLibrary.ensureLoaded();
  }

  /** Default timeout (`-1` = the internal RMI default), matching {@code rmi_c.h}. */
  public static final long DEFAULT_TIMEOUT_MS = -1;

  private long handle;

  Future(long handle) {
    this.handle = handle;
  }

  public void waitFor() {
    waitFor(DEFAULT_TIMEOUT_MS);
  }

  public void waitFor(long timeoutMs) {
    nativeWait(handle, timeoutMs);
  }

  /** Consumes this future, expecting a {@code Dynamic} result (e.g. from {@code call}/{@code get}). */
  public Dynamic getDynamic() {
    long result = nativeGetDynamic(handle);
    handle = 0;
    return Dynamic.wrapOwned(result);
  }

  /** Consumes this future, expecting a {@link Proxy} result (e.g. from {@code instantiateAsync}). */
  public Proxy getProxy() {
    long result = nativeGetProxy(handle);
    handle = 0;
    return result == 0 ? null : new Proxy(result);
  }

  @Override
  public void close() {
    if (handle != 0) {
      nativeRelease(handle);
      handle = 0;
    }
  }

  private static native void nativeWait(long handle, long timeoutMs);
  private static native long nativeGetDynamic(long handle);
  private static native long nativeGetProxy(long handle);
  private static native void nativeRelease(long handle);
}
