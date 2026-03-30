package com.bdg.bison;

/**
 * Utility class for computing FNV-1a hashes of field and class names.
 *
 * <p>The hash function is identical to the C++ {@code "name"_key} literal:
 * 32-bit FNV-1a with the high bit always set.  Use {@link #of(String)} to
 * obtain the hash of a name at runtime and pass it to {@link BisonLibrary}
 * functions that accept pre-hashed names.
 *
 * <p>Note that {@link Dynamic} accepts plain {@code String} names in all its
 * public methods and calls {@link #of(String)} internally, so most users do
 * not need to use this class directly.
 *
 * <h2>Example</h2>
 * <pre>{@code
 * int k = BisonKey.of("velocity");
 * System.out.printf("key('velocity') = 0x%08x%n", k);
 * // The high bit is always set.
 * assert (k & 0x80000000) != 0;
 * }</pre>
 */
public final class BisonKey {

    private BisonKey() {}

    /**
     * Compute the FNV-1a hash of {@code name}.
     *
     * <p>The result is the same value produced by the C++ {@code "name"_key}
     * literal and {@code bison_key()} in the C API.
     *
     * @param name field or class name (must not be {@code null}).
     * @return 32-bit FNV-1a hash with the high bit set, returned as a signed
     *         Java {@code int} (all bits are meaningful; compare with
     *         {@code ==} or use bitwise operations as needed).
     */
    public static int of(String name) {
        return BisonLibrary.getInstance().bison_key(name);
    }
}
