// MIT License © 2025 Binary Dice Games
// Permission is hereby granted, free of charge, to use, copy, modify, and
// distribute this file. See the LICENSE file or
// https://opensource.org/licenses/MIT for details.

package com.bdg.bison;

/**
 * Loads the native libraries backing this binding.
 *
 * <p>Unlike the Python ({@code ctypes.CDLL}) and C# ({@code [LibraryImport]})
 * bindings, which locate a precompiled {@code bison_abi} at run time via
 * {@code BISON_LIB}/the OS search path, an Android app ships its native
 * libraries inside the APK's {@code jniLibs/<abi>/} directory and loads them
 * by name with {@link System#loadLibrary}. {@code bison_jni} (this binding's
 * own JNI glue, built from {@code bindings/android/jni/}) is linked against
 * {@code bison_abi}, so both must be loaded, {@code bison_abi} first.
 */
public final class NativeLibrary {
  private static boolean loaded;

  /** Package-visibility escape hatch for {@code com.bdg.bison.rmi}, which needs this too. */
  public static synchronized void ensureLoaded() {
    if (loaded) return;
    System.loadLibrary("bison_abi");
    System.loadLibrary("bison_jni");
    loaded = true;
  }

  private NativeLibrary() {}
}
