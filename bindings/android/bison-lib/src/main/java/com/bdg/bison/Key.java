// MIT License © 2025 Binary Dice Games
// Permission is hereby granted, free of charge, to use, copy, modify, and
// distribute this file. See the LICENSE file or
// https://opensource.org/licenses/MIT for details.

package com.bdg.bison;

import java.util.LinkedHashMap;
import java.util.Map;

/**
 * Hashes field/class/method names into the {@code bison_hash} (32-bit
 * FNV-1a) identifiers the wire format and the C ABI key on.
 *
 * <p>C++'s {@code "name"_key} is a {@code constexpr} evaluated at compile
 * time; Java has no equivalent, so every lookup calls across the ABI
 * (bison_key()) the same way the Python and C# bindings do. Results are
 * memoized in a small bounded cache -- field/method names are drawn from a
 * static, schema-defined set reused across many calls, so this turns most
 * lookups into a map hit instead of a JNI round trip, while staying bounded
 * so a caller hashing high-cardinality strings can't grow it forever. Mirrors
 * Python's {@code functools.lru_cache(maxsize=4096)} and C#'s
 * {@code Key.Of()} cache.
 */
public final class Key {
  static {
    NativeLibrary.ensureLoaded();
  }

  private static final int MAX_ENTRIES = 4096;

  // Access-order LinkedHashMap doubling as an LRU cache via removeEldestEntry.
  private static final Map<String, Integer> CACHE =
      new LinkedHashMap<String, Integer>(16, 0.75f, true) {
        @Override
        protected boolean removeEldestEntry(Map.Entry<String, Integer> eldest) {
          return size() > MAX_ENTRIES;
        }
      };

  /** Hashes {@code name}, matching the internal C++ {@code "name"_key} / {@code hash()}. */
  public static synchronized int of(String name) {
    if (name == null || name.isEmpty()) return 0;
    Integer cached = CACHE.get(name);
    if (cached != null) return cached;
    int hash = nativeKey(name);
    CACHE.put(name, hash);
    return hash;
  }

  private static native int nativeKey(String name);

  private Key() {}
}
