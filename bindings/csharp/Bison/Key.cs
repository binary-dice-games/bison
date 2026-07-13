// MIT License © 2025 Binary Dice Games
/**
 * @file Key.cs
 * @brief Runtime name hashing for the Bison C# binding, with a bounded
 *        memoization cache mirroring `bison/dynamic.py`'s `key()`.
 */

using System.Collections.Concurrent;

namespace Bdg.Bison;

/// <summary>
/// Computes the 32-bit FNV-1a hash of a field/method/class name -- the same
/// value the C++ side gets at compile time from the <c>"name"_key</c>
/// user-defined literal (a <c>constexpr</c> evaluation, so the hash never
/// costs anything at run time there).
///
/// C# has no equivalent of a `constexpr` string literal hash: every
/// <see cref="Dynamic"/> field/method access must therefore hash its name at
/// run time via <c>bison_key()</c>. <see cref="Of"/> memoizes that call in a
/// bounded cache for the same reason <c>bison.dynamic.key()</c> wraps it in
/// `functools.lru_cache(maxsize=4096)`: real callers draw field/method names
/// from a small, static, schema-defined set that gets hashed over and over
/// (every `obj.Field` access, every `proxy.Method()` call), so caching turns
/// most lookups into a dictionary hit instead of a string encode + P/Invoke
/// call. The cache is bounded rather than unbounded so a caller hashing
/// high-cardinality or externally-derived strings (e.g. untrusted input)
/// can't grow it without bound.
/// </summary>
public static class Key
{
    private const int MaxSize = 4096;

    // ConcurrentDictionary gives thread-safe reads/writes without a global
    // lock on the hot path (mirrors CPython's GIL-protected dict backing
    // lru_cache, but explicit here since .NET has no GIL to lean on).
    private static readonly ConcurrentDictionary<string, uint> Cache = new();

    /// <summary>Maximum number of distinct names memoized before eviction
    /// kicks in. Exposed so tests can assert on the bound, mirroring
    /// `dynamic_mod.key.cache_info().maxsize` in the Python test suite.</summary>
    public static int MaxCacheSize => MaxSize;

    /// <summary>Number of distinct names currently memoized.</summary>
    public static int CacheCount => Cache.Count;

    /// <summary>
    /// Returns the FNV-1a hash of <paramref name="name"/> (identical to
    /// <c>"name"_key</c> in C++ and <c>bison.key(name)</c> in Python).
    /// </summary>
    public static uint Of(string name)
    {
        if (Cache.TryGetValue(name, out var cached))
        {
            return cached;
        }

        var hash = Native.bison_key(name);

        // Bounded, not evicting-LRU: once full, stop admitting new entries
        // rather than paying for eviction bookkeeping on every insert. A
        // cache miss just falls back to the (already cheap) native call.
        if (Cache.Count < MaxSize)
        {
            Cache.TryAdd(name, hash);
        }
        return hash;
    }

    /// <summary>Clears every memoized entry. Test-only escape hatch mirroring
    /// <c>dynamic_mod.key.cache_clear()</c>.</summary>
    public static void ClearCache() => Cache.Clear();
}
