// MIT License © 2025 Binary Dice Games
// Permission is hereby granted, free of charge, to use, copy, modify, and
// distribute this file. See the LICENSE file or
// https://opensource.org/licenses/MIT for details.

package com.bdg.bison;

/**
 * A method callback registered on a {@link Dynamic} with
 * {@link Dynamic#addMethod}. Invoked from native code (possibly from an RMI
 * server worker thread dispatching a remote call) via a JNI upcall.
 *
 * <p>{@code self} and {@code params} are borrowed -- valid only for the
 * duration of the call, backed by handles the native library still owns.
 * {@code result} is likewise a borrowed, pre-allocated output object: write
 * the return value's fields into it. None of the three should be
 * {@linkplain Dynamic#close closed} by the callback.
 */
public interface BisonMethod {
  void invoke(Dynamic self, Dynamic params, Dynamic result);
}
