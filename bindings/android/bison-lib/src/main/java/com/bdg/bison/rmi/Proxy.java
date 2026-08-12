// MIT License © 2025 Binary Dice Games
// Permission is hereby granted, free of charge, to use, copy, modify, and
// distribute this file. See the LICENSE file or
// https://opensource.org/licenses/MIT for details.

package com.bdg.bison.rmi;

import com.bdg.bison.Dynamic;
import com.bdg.bison.Key;
import com.bdg.bison.NativeLibrary;

/**
 * A handle to an object hosted by an RMI server, returned by
 * {@link Client#instantiate}. Wraps {@code rmi_proxy_handle}.
 *
 * <p>Every operation is synchronous with a timeout, matching {@code
 * rmi_c.h}'s {@code rmi_proxy_*} (non-{@code _async}) functions -- the
 * async/{@code rmi_future_handle} half of the C ABI is not yet exposed by
 * this binding, and neither is {@code rmi_proxy_on_event} (server-pushed
 * events); both are documented gaps for a follow-up.
 */
public final class Proxy implements AutoCloseable {
  static {
    NativeLibrary.ensureLoaded();
  }

  /** Default timeout (`-1` = the internal RMI default), matching {@code rmi_c.h}. */
  public static final long DEFAULT_TIMEOUT_MS = -1;

  private long handle;

  Proxy(long handle) {
    this.handle = handle;
  }

  public void set(Dynamic fields) {
    set(fields, DEFAULT_TIMEOUT_MS);
  }

  public void set(Dynamic fields, long timeoutMs) {
    nativeSet(handle, fields == null ? 0 : fields.handle(), timeoutMs);
  }

  /** Retrieves a full snapshot of the remote object's fields. */
  public Dynamic get() {
    return get(null, DEFAULT_TIMEOUT_MS);
  }

  public Dynamic get(Dynamic projection, long timeoutMs) {
    return Dynamic.wrapOwned(nativeGet(handle, projection == null ? 0 : projection.handle(), timeoutMs));
  }

  public void clear() {
    clear(DEFAULT_TIMEOUT_MS);
  }

  public void clear(long timeoutMs) {
    nativeClear(handle, timeoutMs);
  }

  public Dynamic call(String method, Dynamic args) {
    return call(method, args, DEFAULT_TIMEOUT_MS);
  }

  public Dynamic call(String method, Dynamic args, long timeoutMs) {
    return Dynamic.wrapOwned(
        nativeCall(handle, Key.of(method), args == null ? 0 : args.handle(), timeoutMs));
  }

  @Override
  public void close() {
    if (handle != 0) {
      nativeRelease(handle);
      handle = 0;
    }
  }

  private static native void nativeSet(long handle, long fieldsHandle, long timeoutMs);
  private static native long nativeGet(long handle, long projectionHandle, long timeoutMs);
  private static native void nativeClear(long handle, long timeoutMs);
  private static native long nativeCall(long handle, int methodHash, long paramsHandle, long timeoutMs);
  private static native void nativeRelease(long handle);
}
