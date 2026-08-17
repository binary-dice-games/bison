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
 * <p>Every {@code rmi_c.h} {@code rmi_proxy_*} operation has both a
 * synchronous form (blocking with a timeout) and an {@code *Async} form
 * returning a {@link Future}. {@link #onEvent} subscribes to server-pushed
 * events ({@code rmi_proxy_on_event}).
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

  /** Async counterpart to {@link #set}; wait on the returned {@link Future}, then {@link Future#close} it. */
  public Future setAsync(Dynamic fields) {
    return new Future(nativeSetAsync(handle, fields == null ? 0 : fields.handle()));
  }

  /** Retrieves a full snapshot of the remote object's fields. */
  public Dynamic get() {
    return get(null, DEFAULT_TIMEOUT_MS);
  }

  public Dynamic get(Dynamic projection, long timeoutMs) {
    return Dynamic.wrapOwned(nativeGet(handle, projection == null ? 0 : projection.handle(), timeoutMs));
  }

  /** Async counterpart to {@link #get}; consume with {@link Future#getDynamic}. */
  public Future getAsync(Dynamic projection) {
    return new Future(nativeGetAsync(handle, projection == null ? 0 : projection.handle()));
  }

  public void clear() {
    clear(DEFAULT_TIMEOUT_MS);
  }

  public void clear(long timeoutMs) {
    nativeClear(handle, timeoutMs);
  }

  /** Async counterpart to {@link #clear}; wait on the returned {@link Future}, then {@link Future#close} it. */
  public Future clearAsync() {
    return new Future(nativeClearAsync(handle));
  }

  public Dynamic call(String method, Dynamic args) {
    return call(method, args, DEFAULT_TIMEOUT_MS);
  }

  public Dynamic call(String method, Dynamic args, long timeoutMs) {
    return Dynamic.wrapOwned(
        nativeCall(handle, Key.of(method), args == null ? 0 : args.handle(), timeoutMs));
  }

  /** Async counterpart to {@link #call}; consume with {@link Future#getDynamic}. */
  public Future callAsync(String method, Dynamic args) {
    return new Future(nativeCallAsync(handle, Key.of(method), args == null ? 0 : args.handle()));
  }

  /**
   * Subscribes to a server-initiated event named {@code name}. The
   * subscription is not undone by {@link #close} -- it lives as long as the
   * underlying proxy handle does, matching {@code rmi_proxy_on_event}'s
   * lifetime contract (fixed once registered, same as a server's auth
   * handler -- see {@link Server#listen}).
   */
  public void onEvent(String name, ProxyEvent handler) {
    nativeOnEvent(handle, Key.of(name), handler);
  }

  @Override
  public void close() {
    if (handle != 0) {
      nativeRelease(handle);
      handle = 0;
    }
  }

  private static native void nativeSet(long handle, long fieldsHandle, long timeoutMs);
  private static native long nativeSetAsync(long handle, long fieldsHandle);
  private static native long nativeGet(long handle, long projectionHandle, long timeoutMs);
  private static native long nativeGetAsync(long handle, long projectionHandle);
  private static native void nativeClear(long handle, long timeoutMs);
  private static native long nativeClearAsync(long handle);
  private static native long nativeCall(long handle, int methodHash, long paramsHandle, long timeoutMs);
  private static native long nativeCallAsync(long handle, int methodHash, long paramsHandle);
  private static native void nativeOnEvent(long handle, int eventHash, ProxyEvent handler);
  private static native void nativeRelease(long handle);
}
