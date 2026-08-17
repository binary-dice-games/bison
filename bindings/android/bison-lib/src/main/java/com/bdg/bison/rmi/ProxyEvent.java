// MIT License © 2025 Binary Dice Games
// Permission is hereby granted, free of charge, to use, copy, modify, and
// distribute this file. See the LICENSE file or
// https://opensource.org/licenses/MIT for details.

package com.bdg.bison.rmi;

import com.bdg.bison.Dynamic;

/**
 * Handler for a server-pushed event subscribed to with
 * {@link Proxy#onEvent}. Invoked from native code (an RMI client worker
 * thread) via a JNI upcall.
 *
 * <p>{@code params} is borrowed -- valid only for the duration of the call
 * -- and must not be {@linkplain Dynamic#close closed} by the handler.
 */
public interface ProxyEvent {
  void onEvent(Dynamic params);
}
