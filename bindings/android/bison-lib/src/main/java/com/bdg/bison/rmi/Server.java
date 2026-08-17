// MIT License © 2025 Binary Dice Games
// Permission is hereby granted, free of charge, to use, copy, modify, and
// distribute this file. See the LICENSE file or
// https://opensource.org/licenses/MIT for details.

package com.bdg.bison.rmi;

import com.bdg.bison.Dynamic;
import com.bdg.bison.NativeLibrary;

/**
 * Hosts objects for RMI clients to connect to. Wraps {@code
 * rmi_server_handle}. Class prototypes must be registered with
 * {@link Dynamic#registerClass} before a client can instantiate them.
 *
 * <p>The optional {@code auth_handler} callback ({@code rmi_server_listen}'s
 * connection-authentication hook) can be supplied to {@link #listen}; it is
 * evaluated once per incoming connection for as long as the server keeps
 * listening -- it can only be set here, not changed afterward. Omit it (or
 * pass {@code null}) to accept every connection unconditionally, matching
 * passing {@code NULL} for it in C.
 */
public final class Server implements AutoCloseable {
  static {
    NativeLibrary.ensureLoaded();
  }

  private long handle;

  private Server(long handle) {
    this.handle = handle;
  }

  public static Server tcp(String host, int port) {
    return new Server(nativeTcpCreate(host, port));
  }

  public void listen() {
    listen(null, null);
  }

  public void listen(Dynamic params) {
    listen(params, null);
  }

  public void listen(Dynamic params, AuthHandler authHandler) {
    nativeListen(handle, params == null ? 0 : params.handle(), authHandler);
  }

  public void stop() {
    nativeStop(handle);
  }

  @Override
  public void close() {
    if (handle != 0) {
      nativeRelease(handle);
      handle = 0;
    }
  }

  private static native long nativeTcpCreate(String host, int port);
  private static native void nativeListen(long handle, long paramsHandle, AuthHandler authHandler);
  private static native void nativeStop(long handle);
  private static native void nativeRelease(long handle);
}
