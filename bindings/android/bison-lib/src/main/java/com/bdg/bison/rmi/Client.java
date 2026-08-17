// MIT License © 2025 Binary Dice Games
// Permission is hereby granted, free of charge, to use, copy, modify, and
// distribute this file. See the LICENSE file or
// https://opensource.org/licenses/MIT for details.

package com.bdg.bison.rmi;

import com.bdg.bison.Dynamic;
import com.bdg.bison.Key;
import com.bdg.bison.NativeLibrary;

/**
 * Connects to an RMI server (or, for {@link #standalone}, an in-process one)
 * and instantiates remote objects. Wraps {@code rmi_client_handle} (see
 * {@code include/rmi_c.h}).
 *
 * <p>Only the {@code standalone} and TCP socket transports are exposed by
 * this binding for now -- the ones meaningful on Android, where the app
 * process (unlike a desktop CLI) has no inherited stdio/pty to hop over and
 * no access to filesystem-path Unix-domain sockets shared with another
 * process. Named-pipe and terminal (`--transport=term`) transports are
 * intentionally not bound.
 */
public final class Client implements AutoCloseable {
  static {
    NativeLibrary.ensureLoaded();
  }

  private long handle;

  private Client(long handle) {
    this.handle = handle;
  }

  /** An in-process client+server pair with no serialization overhead (`rmi::standalone`). */
  public static Client standalone() {
    return new Client(nativeStandaloneCreate());
  }

  public static Client tcp(String host, int port) {
    return new Client(nativeTcpCreate(host, port));
  }

  public void connect() {
    connect(null);
  }

  public void connect(Dynamic params) {
    nativeConnect(handle, params == null ? 0 : params.handle());
  }

  /** Instantiates a remote object of class {@code className} in the global namespace. */
  public Proxy instantiate(String className, Dynamic params) {
    return instantiate(0, className, params);
  }

  public Proxy instantiate(int namespaceHash, String className, Dynamic params) {
    long proxyHandle = nativeInstantiate(
        handle, namespaceHash, Key.of(className), params == null ? 0 : params.handle());
    return new Proxy(proxyHandle);
  }

  /** Async counterpart to {@link #instantiate(String, Dynamic)}; consume with {@link Future#getProxy}. */
  public Future instantiateAsync(String className, Dynamic params) {
    return instantiateAsync(0, className, params);
  }

  public Future instantiateAsync(int namespaceHash, String className, Dynamic params) {
    long futureHandle = nativeInstantiateAsync(
        handle, namespaceHash, Key.of(className), params == null ? 0 : params.handle());
    return new Future(futureHandle);
  }

  public void disconnect() {
    nativeDisconnect(handle);
  }

  @Override
  public void close() {
    if (handle != 0) {
      nativeRelease(handle);
      handle = 0;
    }
  }

  private static native long nativeStandaloneCreate();
  private static native long nativeTcpCreate(String host, int port);
  private static native void nativeConnect(long handle, long paramsHandle);
  private static native long nativeInstantiate(long handle, int nsHash, int classHash, long paramsHandle);
  private static native long nativeInstantiateAsync(long handle, int nsHash, int classHash, long paramsHandle);
  private static native void nativeDisconnect(long handle);
  private static native void nativeRelease(long handle);
}
