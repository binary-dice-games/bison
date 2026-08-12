// MIT License © 2025 Binary Dice Games
// Permission is hereby granted, free of charge, to use, copy, modify, and
// distribute this file. See the LICENSE file or
// https://opensource.org/licenses/MIT for details.

package com.bdg.bison.rmi;

import com.bdg.bison.Dynamic;
import com.bdg.bison.NativeLibrary;

/**
 * Hosts objects for RMI clients to connect to. Wraps {@code
 * rmi_server_handle}. Class prototypes must be registered with {@code
 * bison_add_class} before a client can instantiate them; this binding does
 * not yet expose that call (see {@link Dynamic}'s gaps list), so a server
 * process built purely from this Android binding has nothing to serve --
 * pair it with prototypes registered from native/C++ code, or from another
 * binding, in the same process.
 *
 * <p>The optional {@code auth_handler} callback ({@code rmi_server_listen}'s
 * connection-authentication hook) is not exposed either; {@link #listen}
 * always accepts every incoming connection, matching passing {@code NULL}
 * for it in C.
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
    listen(null);
  }

  public void listen(Dynamic params) {
    nativeListen(handle, params == null ? 0 : params.handle());
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
  private static native void nativeListen(long handle, long paramsHandle);
  private static native void nativeStop(long handle);
  private static native void nativeRelease(long handle);
}
