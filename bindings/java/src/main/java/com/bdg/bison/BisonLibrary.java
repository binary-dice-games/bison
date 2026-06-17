package com.bdg.bison;

import com.sun.jna.Callback;
import com.sun.jna.Library;
import com.sun.jna.Native;
import com.sun.jna.Pointer;
import com.sun.jna.ptr.FloatByReference;
import com.sun.jna.ptr.IntByReference;
import com.sun.jna.ptr.PointerByReference;

import java.util.HashMap;
import java.util.Map;

/**
 * JNA interface that maps every exported function in {@code libbison_c}.
 *
 * <p>
 * This interface is loaded once (singleton) and the loaded instance is
 * shared across all {@link Dynamic} objects on the same JVM. Users should
 * never interact with {@code BisonLibrary} directly; use {@link Dynamic}
 * instead.
 *
 * <p>
 * To locate the shared library the loader checks (in order):
 * <ol>
 * <li>The {@code BISON_LIB} environment variable.</li>
 * <li>A {@code build/} sibling directory of the repository root.</li>
 * <li>The JNA system-library search path.</li>
 * </ol>
 */
public interface BisonLibrary extends Library {

    // ── Error codes ──────────────────────────────────────────────────────────

    /** Success. */
    int BISON_OK = 0;
    /** A required handle or pointer argument was NULL. */
    int BISON_ERR_NULL = -1;
    /** The field holds a different type than requested. */
    int BISON_ERR_TYPE = -2;
    /** Method or field not found. */
    int BISON_ERR_NOT_FOUND = -3;
    /** Attempted to add a duplicate class or method. */
    int BISON_ERR_DUPLICATE = -4;
    /** An unexpected C++ exception was caught. */
    int BISON_ERR_EXCEPTION = -5;
    /** Input string failed to parse (JSON / YAML). */
    int BISON_ERR_PARSE = -6;

    // ── Callback type ────────────────────────────────────────────────────────

    /**
     * Callback signature for methods registered on {@code dynamic} objects.
     *
     * <p>
     * The callback receives the object on which the method is called
     * ({@code self}), a separate object containing call arguments
     * ({@code params}), and an output handle for the return value
     * ({@code result}). The {@code user} pointer is an arbitrary context
     * value supplied to {@link #bison_add_method}.
     */
    interface MethodCallback extends Callback {
        void invoke(Pointer self, Pointer params, Pointer result, Pointer user);
    }

    // ── Lifecycle ────────────────────────────────────────────────────────────

    /**
     * Create a new, empty dynamic object.
     *
     * @param klassName hashed class name; pass {@code 0} for anonymous.
     * @return new handle (ref-count 1), or {@code null} on failure.
     */
    Pointer bison_create(int klassName);

    /**
     * Create a new dynamic object by class key using {@code dynamic::instantiate}.
     *
     * @param klassName hashed class name.
     * @return new handle (ref-count 1), or {@code null} on failure.
     */
    Pointer bison_instantiate(int klassName);

    /**
     * Increment the reference count of {@code h} and return a new handle.
     *
     * @param h a valid non-null handle.
     * @return new handle sharing ownership.
     */
    Pointer bison_add_ref(Pointer h);

    /**
     * Release a handle and decrement the reference count.
     *
     * @param h handle to release; {@code null} is silently ignored.
     */
    void bison_release(Pointer h);

    // ── Import helpers ───────────────────────────────────────────────────────

    /**
     * Parse a JSON string and return the root object as a handle.
     *
     * @param json null-terminated UTF-8 JSON string.
     * @return new handle (ref-count 1), or {@code null} on parse error.
     */
    Pointer bison_from_json(String json);

    /**
     * Parse a YAML string and return the root object as a handle.
     *
     * @param yaml null-terminated UTF-8 YAML string.
     * @return new handle (ref-count 1), or {@code null} on parse error.
     */
    Pointer bison_from_yaml(String yaml);

    // ── Class registry ───────────────────────────────────────────────────────

    /**
     * Register {@code klass} as a class prototype in the global registry.
     *
     * @param parentName hash of the parent class name; pass {@code 0} for root.
     * @param klass      handle whose {@code __class} field has already been set.
     * @return {@code BISON_OK} on success, negative error code on failure.
     */
    int bison_add_class(int nsName, Pointer klass, int parentName, Pointer meta);

    /**
     * Look up a class in the global namespace.
     *
     * @param name hash of the class name to look up.
     * @return non-owning handle for the found prototype, or {@code null}.
     */
    Pointer bison_find_class(int name);

    /**
     * Look up a class in a specific namespace.
     *
     * @param ns_name hash of the namespace name.
     * @param name    hash of the class name to look up.
     * @return non-owning handle for the found prototype, or {@code null}.
     */
    Pointer bison_find_class_ns(int ns_name, int name);

    // ── Scalar setters ───────────────────────────────────────────────────────

    int bison_set_int(Pointer h, int name, int value);

    int bison_set_float(Pointer h, int name, float value);

    int bison_set_bool(Pointer h, int name, int value);

    int bison_set_string(Pointer h, int name, String value);

    int bison_set_object(Pointer h, int name, Pointer value);

    // ── Indexed setters ──────────────────────────────────────────────────────

    int bison_set_int_at(Pointer h, long index, int value);

    int bison_set_float_at(Pointer h, long index, float value);

    int bison_set_string_at(Pointer h, long index, String value);

    // ── Scalar getters ───────────────────────────────────────────────────────

    int bison_get_int(Pointer h, int name, IntByReference out);

    int bison_get_float(Pointer h, int name, FloatByReference out);

    int bison_get_bool(Pointer h, int name, IntByReference out);

    int bison_get_string(Pointer h, int name, byte[] buf, long bufLen, PointerByReference lenOut);

    int bison_get_object(Pointer h, int name, PointerByReference out);

    // ── Indexed getters ──────────────────────────────────────────────────────

    int bison_get_int_at(Pointer h, long index, IntByReference out);

    int bison_get_float_at(Pointer h, long index, FloatByReference out);

    int bison_get_string_at(Pointer h, long index, byte[] buf, long bufLen, PointerByReference lenOut);

    /** Return the number of array-like (numeric-key) elements. */
    long bison_size(Pointer h);

    // ── Methods ──────────────────────────────────────────────────────────────

    /**
     * Register a C callback as a named method on {@code h}.
     *
     * @param h       target object handle.
     * @param name    method name hash.
     * @param fn      callback implementing the method.
     * @param user    arbitrary user context (may be {@code null}).
     * @return {@code BISON_OK} or an error code.
     */
    int bison_add_method(Pointer h, int name, MethodCallback fn, Pointer user);

    /**
     * Invoke a named method on {@code h}.
     *
     * @param h      target object handle.
     * @param name   method name hash.
     * @param params handle containing call arguments.
     * @param result set to a new handle (ref-count 1) holding the return value.
     * @return {@code BISON_OK} or an error code.
     */
    int bison_call(Pointer h, int name, Pointer params, PointerByReference result);

    // ── Utility ──────────────────────────────────────────────────────────────

    /**
     * Compute the FNV-1a hash of a null-terminated string.
     *
     * @param name null-terminated string.
     * @return 32-bit FNV-1a hash with the high bit set.
     */
    int bison_key(String name);

    // ── Singleton loader ─────────────────────────────────────────────────────

    /**
     * Returns the singleton loaded instance of {@link BisonLibrary}.
     *
     * <p>
     * The library is loaded once; subsequent calls return the same instance.
     *
     * @return loaded library instance.
     * @throws UnsatisfiedLinkError if the native library cannot be found.
     */
    static BisonLibrary getInstance() {
        return Holder.INSTANCE;
    }

    /** Lazy singleton holder (thread-safe by JVM class-loading semantics). */
    final class Holder {
        private static final BisonLibrary INSTANCE = load();

        private static BisonLibrary load() {
            String envPath = System.getenv("BISON_LIB");
            if (envPath != null && !envPath.isEmpty()) {
                Map<String, Object> opts = new HashMap<>();
                return Native.load(envPath, BisonLibrary.class, opts);
            }

            // Probe for a library built in the canonical build/ directory.
            String[] candidates = {
                    "build/libbison_c.so", // Linux (when cwd is repo root)
                    "build/libbison_c.dylib", // macOS (when cwd is repo root)
                    "build/Release/bison_c.dll",
                    "build/Debug/bison_c.dll",
                    "../../build/libbison_c.so", // Linux (when cwd is bindings/java)
                    "../../build/libbison_c.dylib", // macOS (when cwd is bindings/java)
                    "../../build/Release/bison_c.dll",
                    "../../build/Debug/bison_c.dll",
            };
            for (String candidate : candidates) {
                java.io.File f = new java.io.File(
                        System.getProperty("user.dir"), candidate);
                if (f.exists()) {
                    return Native.load(f.getAbsolutePath(), BisonLibrary.class);
                }
            }

            // Fall back to the JNA/system library search path.
            return Native.load("bison_c", BisonLibrary.class);
        }
    }
}
