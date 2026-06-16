package com.bdg.bison;

import com.sun.jna.Pointer;
import com.sun.jna.ptr.FloatByReference;
import com.sun.jna.ptr.IntByReference;
import com.sun.jna.ptr.PointerByReference;

import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.function.BiFunction;

/**
 * A reference-counted dynamic object backed by {@code libbison_c}.
 *
 * <p>{@code Dynamic} wraps an opaque {@code bison_handle} and provides a
 * Java-idiomatic interface for reading and writing fields, calling methods,
 * and managing object lifetime.
 *
 * <p>Every {@code Dynamic} instance holds an owning reference to the
 * underlying native object.  You must call {@link #close()} (or use it in a
 * try-with-resources block) when the object is no longer needed.
 *
 * <h2>Basic usage</h2>
 * <pre>{@code
 * try (Dynamic obj = new Dynamic()) {
 *     obj.setInt("score", 42);
 *     obj.setFloat("speed", 9.5f);
 *     obj.setBool("alive", true);
 *     obj.setString("name", "hero");
 *
 *     System.out.println(obj.getInt("score"));   // 42
 *     System.out.println(obj.getFloat("speed")); // 9.5
 *     System.out.println(obj.getBool("alive"));  // true
 *     System.out.println(obj.getString("name")); // hero
 * }
 * }</pre>
 *
 * <h2>Nested objects</h2>
 * <pre>{@code
 * try (Dynamic player = new Dynamic();
 *      Dynamic position = new Dynamic()) {
 *     position.setFloat("x", 1.0f);
 *     position.setFloat("y", 2.5f);
 *     player.setObject("position", position);
 *
 *     try (Dynamic pos = player.getObject("position")) {
 *         System.out.println(pos.getFloat("x")); // 1.0
 *     }
 * }
 * }</pre>
 *
 * <h2>Method registration</h2>
 * <pre>{@code
 * try (Dynamic obj = new Dynamic()) {
 *     obj.setInt("counter", 5);
 *     obj.addMethod("double", (self, params) -> {
 *         int current = self.getInt("counter");
 *         self.setInt("counter", current * 2);
 *         return new Dynamic();
 *     });
 *     try (Dynamic result = obj.call("double", new Dynamic())) {
 *         // counter is now 10
 *     }
 *     System.out.println(obj.getInt("counter")); // 10
 * }
 * }</pre>
 */
public class Dynamic implements AutoCloseable {

    // ── Internal state ───────────────────────────────────────────────────────

    /** The underlying native handle. May be {@code null} after {@link #close()}. */
    private Pointer handle;

    /** Whether this instance holds an owning reference. */
    private final boolean owned;

    /** JNA callbacks must be kept alive while the method is registered. */
    private final List<BisonLibrary.MethodCallback> callbacks = new ArrayList<>();

    /** Shared library singleton. */
    private static final BisonLibrary LIB = BisonLibrary.getInstance();

    // ── Constructors ─────────────────────────────────────────────────────────

    /**
     * Create a new anonymous dynamic object.
     */
    public Dynamic() {
        this.handle = LIB.bison_create(0);
        if (this.handle == null) {
            throw new OutOfMemoryError("bison_create failed");
        }
        this.owned = true;
    }

    /**
     * Create a new dynamic object belonging to the named class.
     *
     * @param className class name string; the FNV-1a hash is computed
     *                  automatically.
     */
    public Dynamic(String className) {
        this.handle = LIB.bison_create(BisonKey.of(className));
        if (this.handle == null) {
            throw new OutOfMemoryError("bison_create failed");
        }
        this.owned = true;
    }

    /**
     * Internal constructor: adopt an already-allocated handle.
     *
     * @param handle raw native handle (must be non-null for owning instances).
     * @param owned  {@code true} if this instance owns the reference.
     */
    Dynamic(Pointer handle, boolean owned) {
        this.handle = handle;
        this.owned  = owned;
    }

    // ── AutoCloseable / lifecycle ─────────────────────────────────────────────

    /**
     * Release the native handle.
     *
     * <p>Safe to call multiple times; subsequent calls are no-ops.
     * Non-owning instances (created inside method callbacks) are silently
     * skipped.
     */
    @Override
    public void close() {
        if (owned && handle != null) {
            LIB.bison_release(handle);
            handle = null;
        }
    }

    /**
     * Increment the reference count and return a new {@code Dynamic} sharing
     * ownership of the same object.
     *
     * <p>The caller is responsible for closing the returned instance.
     *
     * @return a new {@code Dynamic} backed by the same native object.
     */
    public Dynamic addRef() {
        Pointer h = LIB.bison_add_ref(handle);
        if (h == null) {
            throw new RuntimeException("bison_add_ref failed");
        }
        return new Dynamic(h, true);
    }

    // ── Helpers ───────────────────────────────────────────────────────────────

    private void check(int rc, String context) {
        if (rc != BisonLibrary.BISON_OK) {
            throw new BisonError(rc, context);
        }
    }

    private int key(String name) {
        return BisonKey.of(name);
    }

    Pointer handle() {
        return handle;
    }

    // ── Named field setters ───────────────────────────────────────────────────

    /**
     * Set an {@code int32_t} field by name.
     *
     * @param name  field name.
     * @param value integer value.
     */
    public void setInt(String name, int value) {
        check(LIB.bison_set_int(handle, key(name), value), "setInt(" + name + ")");
    }

    /**
     * Set a {@code float} field by name.
     *
     * @param name  field name.
     * @param value float value.
     */
    public void setFloat(String name, float value) {
        check(LIB.bison_set_float(handle, key(name), value), "setFloat(" + name + ")");
    }

    /**
     * Set a {@code bool} field by name.
     *
     * @param name  field name.
     * @param value boolean value.
     */
    public void setBool(String name, boolean value) {
        check(LIB.bison_set_bool(handle, key(name), value ? 1 : 0), "setBool(" + name + ")");
    }

    /**
     * Set a {@code std::string} field by name.
     *
     * @param name  field name.
     * @param value string value.
     */
    public void setString(String name, String value) {
        check(LIB.bison_set_string(handle, key(name), value), "setString(" + name + ")");
    }

    /**
     * Set a nested {@link Dynamic} field by name.
     *
     * <p>The native reference count of {@code value} is incremented; both
     * this object and the caller retain ownership.
     *
     * @param name  field name.
     * @param value nested object (may be {@code null} for a null ref).
     */
    public void setObject(String name, Dynamic value) {
        Pointer vPtr = (value != null) ? value.handle : null;
        check(LIB.bison_set_object(handle, key(name), vPtr), "setObject(" + name + ")");
    }

    // ── Indexed field setters ─────────────────────────────────────────────────

    /**
     * Set an {@code int32_t} field by zero-based numeric index.
     *
     * @param index zero-based index.
     * @param value integer value.
     */
    public void setIntAt(int index, int value) {
        check(LIB.bison_set_int_at(handle, index, value), "setIntAt[" + index + "]");
    }

    /**
     * Set a {@code float} field by zero-based numeric index.
     *
     * @param index zero-based index.
     * @param value float value.
     */
    public void setFloatAt(int index, float value) {
        check(LIB.bison_set_float_at(handle, index, value), "setFloatAt[" + index + "]");
    }

    /**
     * Set a string field by zero-based numeric index.
     *
     * @param index zero-based index.
     * @param value string value.
     */
    public void setStringAt(int index, String value) {
        check(LIB.bison_set_string_at(handle, index, value), "setStringAt[" + index + "]");
    }

    // ── Named field getters ───────────────────────────────────────────────────

    /**
     * Read an {@code int32_t} field by name.
     *
     * @param name field name.
     * @return integer value.
     * @throws BisonError if the field does not exist or has the wrong type.
     */
    public int getInt(String name) {
        IntByReference out = new IntByReference();
        check(LIB.bison_get_int(handle, key(name), out), "getInt(" + name + ")");
        return out.getValue();
    }

    /**
     * Read a {@code float} field by name.
     *
     * @param name field name.
     * @return float value.
     * @throws BisonError if the field does not exist or has the wrong type.
     */
    public float getFloat(String name) {
        FloatByReference out = new FloatByReference();
        check(LIB.bison_get_float(handle, key(name), out), "getFloat(" + name + ")");
        return out.getValue();
    }

    /**
     * Read a {@code bool} field by name.
     *
     * @param name field name.
     * @return boolean value.
     * @throws BisonError if the field does not exist or has the wrong type.
     */
    public boolean getBool(String name) {
        IntByReference out = new IntByReference();
        check(LIB.bison_get_bool(handle, key(name), out), "getBool(" + name + ")");
        return out.getValue() != 0;
    }

    /**
     * Read a {@code std::string} field by name.
     *
     * @param name field name.
     * @return string value.
     * @throws BisonError if the field does not exist or has the wrong type.
     */
    public String getString(String name) {
        PointerByReference lenOut = new PointerByReference();
        int rc = LIB.bison_get_string(handle, key(name), null, 0, lenOut);
        check(rc, "getString(" + name + ") length query");

        long len = Pointer.nativeValue(lenOut.getValue());
        if (len == 0) return "";

        byte[] buf = new byte[(int) len + 1];
        check(LIB.bison_get_string(handle, key(name), buf, len + 1, null),
              "getString(" + name + ")");
        return new String(buf, 0, (int) len, StandardCharsets.UTF_8);
    }

    /**
     * Read a nested {@link Dynamic} object field by name.
     *
     * <p>Returns a new {@code Dynamic} (ref-count 1) that must be closed by
     * the caller.
     *
     * @param name field name.
     * @return nested object.
     * @throws BisonError if the field does not exist or has the wrong type.
     */
    public Dynamic getObject(String name) {
        PointerByReference out = new PointerByReference();
        check(LIB.bison_get_object(handle, key(name), out), "getObject(" + name + ")");
        Pointer childHandle = out.getValue();
        if (childHandle == null) return null;
        return new Dynamic(childHandle, true);
    }

    // ── Indexed field getters ─────────────────────────────────────────────────

    /**
     * Read an {@code int32_t} field at a zero-based numeric index.
     *
     * @param index zero-based index.
     * @return integer value.
     */
    public int getIntAt(int index) {
        IntByReference out = new IntByReference();
        check(LIB.bison_get_int_at(handle, index, out), "getIntAt[" + index + "]");
        return out.getValue();
    }

    /**
     * Read a {@code float} field at a zero-based numeric index.
     *
     * @param index zero-based index.
     * @return float value.
     */
    public float getFloatAt(int index) {
        FloatByReference out = new FloatByReference();
        check(LIB.bison_get_float_at(handle, index, out), "getFloatAt[" + index + "]");
        return out.getValue();
    }

    /**
     * Read a string field at a zero-based numeric index.
     *
     * @param index zero-based index.
     * @return string value.
     */
    public String getStringAt(int index) {
        PointerByReference lenOut = new PointerByReference();
        int rc = LIB.bison_get_string_at(handle, index, null, 0, lenOut);
        check(rc, "getStringAt[" + index + "] length query");

        long len = Pointer.nativeValue(lenOut.getValue());
        if (len == 0) return "";

        byte[] buf = new byte[(int) len + 1];
        check(LIB.bison_get_string_at(handle, index, buf, len + 1, null),
              "getStringAt[" + index + "]");
        return new String(buf, 0, (int) len, StandardCharsets.UTF_8);
    }

    /**
     * Return the number of array-like (numeric-key) elements.
     *
     * @return element count.
     */
    public long size() {
        return LIB.bison_size(handle);
    }

    // ── Methods ───────────────────────────────────────────────────────────────

    /**
     * Register a Java lambda as a named method on this object.
     *
     * <p>The lambda must have the signature
     * {@code (Dynamic self, Dynamic params) -> Dynamic result}.  The returned
     * {@code Dynamic} is the method's return value; the Java method is
     * responsible for closing it <em>only if</em> the result is non-null
     * and will not be used further.  The callback is kept alive for the
     * lifetime of this object.
     *
     * @param name method name.
     * @param fn   lambda implementing the method.
     * @throws BisonError if the name is already registered.
     */
    public void addMethod(String name,
            BiFunction<Dynamic, Dynamic, Dynamic> fn) {
        BisonLibrary.MethodCallback cb = (selfPtr, paramsPtr, resultPtr, user) -> {
            Dynamic selfDyn   = new Dynamic(selfPtr,   false);
            Dynamic paramsDyn = new Dynamic(paramsPtr, false);
            Dynamic resultDyn = new Dynamic(resultPtr, false);
            try {
                Dynamic ret = fn.apply(selfDyn, paramsDyn);
                if (ret != null) {
                    // Copy indexed fields from ret into result.
                    long sz = ret.size();
                    for (long i = 0; i < sz; i++) {
                        // Best-effort: try int, float, string.
                        try {
                            resultDyn.setIntAt((int) i, ret.getIntAt((int) i));
                        } catch (BisonError e1) {
                            try {
                                resultDyn.setFloatAt((int) i, ret.getFloatAt((int) i));
                            } catch (BisonError e2) {
                                try {
                                    resultDyn.setStringAt((int) i, ret.getStringAt((int) i));
                                } catch (BisonError ignored) {}
                            }
                        }
                    }
                    ret.close();
                }
            } catch (Exception ignored) {
                // Swallow Java exceptions – cannot propagate across C ABI.
            }
        };
        callbacks.add(cb);  // keep alive
        check(LIB.bison_add_method(handle, key(name), cb, null, null),
              "addMethod(" + name + ")");
    }

    /**
     * Invoke a named method on this object.
     *
     * <p>The returned {@code Dynamic} (ref-count 1) must be closed by the
     * caller.
     *
     * @param name   method name.
     * @param params argument object (may be an empty {@code Dynamic}).
     * @return return value of the method.
     * @throws BisonError if the method is not found.
     */
    public Dynamic call(String name, Dynamic params) {
        PointerByReference resultRef = new PointerByReference();
        check(LIB.bison_call(handle, key(name), params.handle, resultRef),
              "call(" + name + ")");
        return new Dynamic(resultRef.getValue(), true);
    }

    // ── Class registry helpers ────────────────────────────────────────────────

    /**
     * Register this object as a named class in the global registry.
     *
     * @param parentName parent class name (empty string for root classes).
     * @throws BisonError if a class with the same name is already registered.
     */
    public void addClass(String parentName) {
        int parentKey = (parentName == null || parentName.isEmpty())
                        ? 0 : BisonKey.of(parentName);
        check(LIB.bison_add_class(parentKey, handle), "addClass");
    }

    /**
     * Create a new instance of this object's class and return it.
     *
     * <p>The caller is responsible for closing the returned instance.
     *
     * @return new instance of the same class.
     */
    public Dynamic instantiate() {
        int classKey = getInt("__class");
        Pointer h = LIB.bison_instantiate(classKey);
        if (h == null) throw new OutOfMemoryError("bison_instantiate failed");
        return new Dynamic(h, true);
    }

    // ── Factory methods ───────────────────────────────────────────────────────

    /**
     * Parse a JSON string and return the root object as a {@code Dynamic}.
     *
     * <p>The caller is responsible for closing the returned instance.
     *
     * @param json valid JSON string.
     * @return decoded root object.
     * @throws BisonError if the JSON is invalid.
     */
    public static Dynamic fromJson(String json) {
        Pointer h = LIB.bison_from_json(json);
        if (h == null) throw new BisonError(BisonLibrary.BISON_ERR_PARSE, "fromJson");
        return new Dynamic(h, true);
    }

    /**
     * Parse a YAML string and return the root object as a {@code Dynamic}.
     *
     * <p>The caller is responsible for closing the returned instance.
     *
     * @param yaml valid YAML string (UTF-8).
     * @return decoded root object.
     * @throws BisonError if the YAML is invalid.
     */
    public static Dynamic fromYaml(String yaml) {
        Pointer h = LIB.bison_from_yaml(yaml);
        if (h == null) throw new BisonError(BisonLibrary.BISON_ERR_PARSE, "fromYaml");
        return new Dynamic(h, true);
    }

    // ── toString ─────────────────────────────────────────────────────────────

    @Override
    public String toString() {
        if (handle == null) return "Dynamic(released)";
        return "Dynamic(handle=" + handle + ", size=" + size() + ")";
    }
}
