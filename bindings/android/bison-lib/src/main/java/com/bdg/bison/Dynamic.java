// MIT License © 2025 Binary Dice Games
// Permission is hereby granted, free of charge, to use, copy, modify, and
// distribute this file. See the LICENSE file or
// https://opensource.org/licenses/MIT for details.

package com.bdg.bison;

/**
 * A self-describing, heterogeneous object -- the Android/Kotlin binding's
 * wrapper around {@code bison_handle} (see {@code include/bison_c.h}).
 *
 * <p>Every {@code Dynamic} obtained from a constructor, {@link #fromJson},
 * {@link #deserialize}, {@link #copy}, or {@link #call} owns a reference and
 * must be {@link #close}d (or used in a try-with-resources block); C++'s
 * RAII destructor becomes Java's {@link AutoCloseable}, the same choice the
 * C# binding makes with {@code IDisposable}. Instances handed to a
 * {@link BisonMethod} callback are <em>borrowed</em> and must not be closed
 * -- see that interface's docs.
 *
 * <p>Covers named-field and indexed (numeric) scalar/vector access,
 * serialization, JSON/YAML text interop, class registration/inheritance and
 * cross-namespace lookup, field/class/method attribute metadata, and
 * invoking existing methods -- the same surface the Python/C# bindings
 * expose over {@code bison_c.h}.
 */
public final class Dynamic implements AutoCloseable {
  static {
    NativeLibrary.ensureLoaded();
  }

  private long handle;
  private final boolean owned;

  public Dynamic() {
    this(0);
  }

  /** Creates a new object tagged with the given class name (see {@code __class}). */
  public Dynamic(String className) {
    this(Key.of(className));
  }

  private Dynamic(int classNameHash) {
    this.handle = nativeCreate(classNameHash);
    this.owned = true;
  }

  private Dynamic(long handle, boolean owned) {
    this.handle = handle;
    this.owned = owned;
  }

  /**
   * Wraps a new, owned {@code bison_handle} returned by a native call; {@code 0} becomes
   * {@code null}. Binding-internal: used by {@code com.bdg.bison.rmi} (a separate Java package,
   * so this can't be package-private) and by {@code bison_jni.cpp}'s JNI upcall for
   * {@link BisonMethod}; not meant for application code.
   */
  public static Dynamic wrapOwned(long handle) {
    return handle == 0 ? null : new Dynamic(handle, true);
  }

  /** Binding-internal, see {@link #wrapOwned}. Wraps a borrowed handle (a {@link BisonMethod}
   *  callback's {@code self}/{@code params}/{@code result}); must not be closed by the caller. */
  public static Dynamic wrapBorrowed(long handle) {
    return handle == 0 ? null : new Dynamic(handle, false);
  }

  /** Binding-internal, see {@link #wrapOwned}. The raw {@code bison_handle}, as a pointer-width integer. */
  public long handle() {
    return handle;
  }

  public static Dynamic fromJson(String json) {
    return wrapOwned(nativeFromJson(json));
  }

  public static Dynamic deserialize(byte[] data) {
    return wrapOwned(nativeDeserialize(data));
  }

  /** Deep-copies this object (`bison_clone`). */
  public Dynamic copy() {
    return wrapOwned(nativeClone(handle));
  }

  @Override
  public void close() {
    if (handle != 0 && owned) {
      nativeRelease(handle);
    }
    handle = 0;
  }

  // ─── Scalar fields ────────────────────────────────────────────────────

  public void setInt(String field, int value) {
    nativeSetInt(handle, Key.of(field), value);
  }

  public void setFloat(String field, float value) {
    nativeSetFloat(handle, Key.of(field), value);
  }

  public void setBool(String field, boolean value) {
    nativeSetBool(handle, Key.of(field), value);
  }

  public void setString(String field, String value) {
    nativeSetString(handle, Key.of(field), value);
  }

  /** Sets a field whose value is itself a hashed key (distinct from a plain int32 field). */
  public void setKey(String field, int valueHash) {
    nativeSetKey(handle, Key.of(field), valueHash);
  }

  public void setObject(String field, Dynamic value) {
    nativeSetObject(handle, Key.of(field), value == null ? 0 : value.handle);
  }

  public int getInt(String field) {
    return nativeGetInt(handle, Key.of(field));
  }

  public float getFloat(String field) {
    return nativeGetFloat(handle, Key.of(field));
  }

  public boolean getBool(String field) {
    return nativeGetBool(handle, Key.of(field));
  }

  public String getString(String field) {
    return nativeGetString(handle, Key.of(field));
  }

  public int getKey(String field) {
    return nativeGetKey(handle, Key.of(field));
  }

  public Dynamic getObject(String field) {
    return wrapOwned(nativeGetObject(handle, Key.of(field)));
  }

  // ─── Indexed (numeric) field access ─────────────────────────────────────

  public void setIntAt(long index, int value) {
    nativeSetIntAt(handle, index, value);
  }

  public void setFloatAt(long index, float value) {
    nativeSetFloatAt(handle, index, value);
  }

  public void setBoolAt(long index, boolean value) {
    nativeSetBoolAt(handle, index, value);
  }

  public void setStringAt(long index, String value) {
    nativeSetStringAt(handle, index, value);
  }

  /** Sets an indexed element whose value is itself a hashed key. */
  public void setKeyAt(long index, int valueHash) {
    nativeSetKeyAt(handle, index, valueHash);
  }

  public void setObjectAt(long index, Dynamic value) {
    nativeSetObjectAt(handle, index, value == null ? 0 : value.handle);
  }

  public int getIntAt(long index) {
    return nativeGetIntAt(handle, index);
  }

  public float getFloatAt(long index) {
    return nativeGetFloatAt(handle, index);
  }

  public boolean getBoolAt(long index) {
    return nativeGetBoolAt(handle, index);
  }

  public String getStringAt(long index) {
    return nativeGetStringAt(handle, index);
  }

  public int getKeyAt(long index) {
    return nativeGetKeyAt(handle, index);
  }

  public Dynamic getObjectAt(long index) {
    return wrapOwned(nativeGetObjectAt(handle, index));
  }

  // ─── Vector fields ────────────────────────────────────────────────────

  public void setVectorBool(String field, boolean[] values) {
    nativeSetVectorBool(handle, Key.of(field), values);
  }

  public void setVectorInt(String field, int[] values) {
    nativeSetVectorInt(handle, Key.of(field), values);
  }

  public void setVectorFloat(String field, float[] values) {
    nativeSetVectorFloat(handle, Key.of(field), values);
  }

  public void setVectorBytes(String field, byte[] values) {
    nativeSetVectorBytes(handle, Key.of(field), values);
  }

  public boolean[] getVectorBool(String field) {
    return nativeGetVectorBool(handle, Key.of(field));
  }

  public int[] getVectorInt(String field) {
    return nativeGetVectorInt(handle, Key.of(field));
  }

  public float[] getVectorFloat(String field) {
    return nativeGetVectorFloat(handle, Key.of(field));
  }

  public byte[] getVectorBytes(String field) {
    return nativeGetVectorBytes(handle, Key.of(field));
  }

  // ─── Methods ──────────────────────────────────────────────────────────

  /**
   * Registers {@code callback} as method {@code name} on this object. The
   * registration is not undone by {@link #close} -- it lives as long as the
   * underlying native object does, matching {@code bison_add_method}'s
   * lifetime contract.
   */
  public void addMethod(String name, BisonMethod callback) {
    nativeAddMethod(handle, Key.of(name), callback);
  }

  /** Invokes method {@code name} (local, or a class method inherited/registered elsewhere). */
  public Dynamic call(String name, Dynamic args) {
    return wrapOwned(nativeCall(handle, Key.of(name), args == null ? 0 : args.handle));
  }

  /** Reads the attributes ({@code bison_get_method_attributes}) of method {@code name} on this object. */
  public Attributes methodAttributes(String name) {
    return nativeGetMethodAttributes(handle, Key.of(name));
  }

  // ─── Class registry ───────────────────────────────────────────────────

  /**
   * Registers {@code prototype} as a root (parentless) class prototype in
   * the global namespace, so an RMI server can host it and clients can
   * {@code instantiate()} it by name -- {@code bison_add_class(0, ..., 0,
   * NULL)}. The class name is {@code prototype}'s own (set by the {@link
   * Dynamic#Dynamic(String)} constructor it was created with), matching
   * how the internal C++ API's {@code addClass(0U, base)} takes no separate
   * name argument either. The registry keeps its own reference; {@code
   * prototype} may still be {@link #close}d afterwards.
   */
  public static void registerClass(Dynamic prototype) {
    registerClass(prototype, null, null, null);
  }

  /**
   * Registers {@code prototype} as a class prototype, optionally under a
   * parent class ({@code parentName}) and/or in a specific namespace
   * ({@code nsName}) rather than the global one -- the full
   * {@code bison_add_class(ns_name, klass, parent_name, meta)}.
   */
  public static void registerClass(Dynamic prototype, String parentName, String nsName, Attributes meta) {
    nativeAddClass(Key.of(nsName), prototype.handle, Key.of(parentName), meta);
  }

  /**
   * Looks up a registered class prototype ({@code bison_find_class}). The
   * returned {@link Dynamic}, if any, is a <em>borrowed</em> view into the
   * registry -- do not {@link #close} it.
   */
  public static Dynamic findClass(String className, String nsName) {
    return wrapBorrowed(nativeFindClass(Key.of(nsName), Key.of(className)));
  }

  /**
   * Creates a new instance of {@code className} ({@code bison_instantiate}),
   * walking the class's registered inheritance chain. Unlike the plain
   * {@link Dynamic#Dynamic(String)} constructor, this always succeeds --
   * falling back to an anonymous object if {@code className} names no
   * registered class, matching {@code dynamic::instantiate()}'s contract.
   */
  public static Dynamic instantiate(String className, String nsName) {
    return wrapOwned(nativeInstantiate(Key.of(nsName), Key.of(className)));
  }

  /** Removes every registered class ({@code bison_clear_registry}). */
  public static void clearRegistry() {
    nativeClearRegistry();
  }

  /** Reads the attributes ({@code bison_get_class_attributes}) of a registered class. */
  public static Attributes classAttributes(String className, String nsName) {
    return nativeGetClassAttributes(Key.of(nsName), Key.of(className));
  }

  // ─── Field registration with optional attribute metadata ────────────────
  // Unlike setInt()/setString()/etc. above (which silently auto-vivify a
  // field on first write, matching dynamic::operator[]), these fail with
  // BISON_ERR_DUPLICATE if the field already exists -- the same
  // "declare once, with metadata" contract bison_add_field_*() documents.

  public void addFieldInt(String name, int value, Attributes meta) {
    nativeAddFieldInt(handle, Key.of(name), value, meta);
  }

  public void addFieldFloat(String name, float value, Attributes meta) {
    nativeAddFieldFloat(handle, Key.of(name), value, meta);
  }

  public void addFieldBool(String name, boolean value, Attributes meta) {
    nativeAddFieldBool(handle, Key.of(name), value, meta);
  }

  public void addFieldString(String name, String value, Attributes meta) {
    nativeAddFieldString(handle, Key.of(name), value, meta);
  }

  /** Declares a new {@code bison::key_t}-valued field -- the {@link #addFieldInt} counterpart to {@link #setKey}. */
  public void addFieldKey(String name, int valueHash, Attributes meta) {
    nativeAddFieldKey(handle, Key.of(name), valueHash, meta);
  }

  public void addFieldVectorBool(String name, boolean[] values, Attributes meta) {
    nativeAddFieldVectorBool(handle, Key.of(name), values, meta);
  }

  public void addFieldVectorInt(String name, int[] values, Attributes meta) {
    nativeAddFieldVectorInt(handle, Key.of(name), values, meta);
  }

  public void addFieldVectorFloat(String name, float[] values, Attributes meta) {
    nativeAddFieldVectorFloat(handle, Key.of(name), values, meta);
  }

  public void addFieldVectorBytes(String name, byte[] values, Attributes meta) {
    nativeAddFieldVectorBytes(handle, Key.of(name), values, meta);
  }

  /** Reads the attributes ({@code bison_get_field_attributes}) of field {@code name} on this object. */
  public Attributes fieldAttributes(String name) {
    return nativeGetFieldAttributes(handle, Key.of(name));
  }

  // ─── Serialization ────────────────────────────────────────────────────

  public int size() {
    return (int) nativeSize(handle);
  }

  /** Compact binary wire format (see {@code FORMAT.md}); round-trips with {@link #deserialize}. */
  public byte[] serialize() {
    return nativeSerialize(handle);
  }

  public String toJson() {
    return toJson(-1);
  }

  /** @param indent Pretty-print indent width, or {@code -1} for compact output. */
  public String toJson(int indent) {
    return nativeToJson(handle, indent);
  }

  public static Dynamic fromYaml(String yaml) {
    return wrapOwned(nativeFromYaml(yaml));
  }

  public String toYaml() {
    return nativeToYaml(handle);
  }

  // ─── Native methods (implemented in bindings/android/jni/bison_jni.cpp) ─

  private static native long nativeCreate(int classNameHash);
  private static native long nativeFromJson(String json);
  private static native long nativeFromYaml(String yaml);
  private static native long nativeDeserialize(byte[] data);
  private static native long nativeClone(long handle);
  private static native void nativeRelease(long handle);

  private static native void nativeSetInt(long handle, int nameHash, int value);
  private static native void nativeSetFloat(long handle, int nameHash, float value);
  private static native void nativeSetBool(long handle, int nameHash, boolean value);
  private static native void nativeSetString(long handle, int nameHash, String value);
  private static native void nativeSetKey(long handle, int nameHash, int valueHash);
  private static native void nativeSetObject(long handle, int nameHash, long valueHandle);

  private static native int nativeGetInt(long handle, int nameHash);
  private static native float nativeGetFloat(long handle, int nameHash);
  private static native boolean nativeGetBool(long handle, int nameHash);
  private static native String nativeGetString(long handle, int nameHash);
  private static native int nativeGetKey(long handle, int nameHash);
  private static native long nativeGetObject(long handle, int nameHash);

  private static native void nativeSetIntAt(long handle, long index, int value);
  private static native void nativeSetFloatAt(long handle, long index, float value);
  private static native void nativeSetBoolAt(long handle, long index, boolean value);
  private static native void nativeSetStringAt(long handle, long index, String value);
  private static native void nativeSetKeyAt(long handle, long index, int valueHash);
  private static native void nativeSetObjectAt(long handle, long index, long valueHandle);

  private static native int nativeGetIntAt(long handle, long index);
  private static native float nativeGetFloatAt(long handle, long index);
  private static native boolean nativeGetBoolAt(long handle, long index);
  private static native String nativeGetStringAt(long handle, long index);
  private static native int nativeGetKeyAt(long handle, long index);
  private static native long nativeGetObjectAt(long handle, long index);

  private static native void nativeSetVectorBool(long handle, int nameHash, boolean[] values);
  private static native void nativeSetVectorInt(long handle, int nameHash, int[] values);
  private static native void nativeSetVectorFloat(long handle, int nameHash, float[] values);
  private static native void nativeSetVectorBytes(long handle, int nameHash, byte[] values);

  private static native boolean[] nativeGetVectorBool(long handle, int nameHash);
  private static native int[] nativeGetVectorInt(long handle, int nameHash);
  private static native float[] nativeGetVectorFloat(long handle, int nameHash);
  private static native byte[] nativeGetVectorBytes(long handle, int nameHash);

  private static native void nativeAddClass(int nsHash, long protoHandle, int parentHash, Attributes meta);
  private static native long nativeFindClass(int nsHash, int klassHash);
  private static native long nativeInstantiate(int nsHash, int klassHash);
  private static native void nativeClearRegistry();
  private static native Attributes nativeGetClassAttributes(int nsHash, int klassHash);
  private static native Attributes nativeGetFieldAttributes(long handle, int fieldHash);
  private static native Attributes nativeGetMethodAttributes(long handle, int methodHash);

  private static native void nativeAddFieldInt(long handle, int keyHash, int value, Attributes meta);
  private static native void nativeAddFieldFloat(long handle, int keyHash, float value, Attributes meta);
  private static native void nativeAddFieldBool(long handle, int keyHash, boolean value, Attributes meta);
  private static native void nativeAddFieldString(long handle, int keyHash, String value, Attributes meta);
  private static native void nativeAddFieldKey(long handle, int keyHash, int valueHash, Attributes meta);
  private static native void nativeAddFieldVectorBool(long handle, int keyHash, boolean[] values, Attributes meta);
  private static native void nativeAddFieldVectorInt(long handle, int keyHash, int[] values, Attributes meta);
  private static native void nativeAddFieldVectorFloat(long handle, int keyHash, float[] values, Attributes meta);
  private static native void nativeAddFieldVectorBytes(long handle, int keyHash, byte[] values, Attributes meta);

  private static native void nativeAddMethod(long handle, int nameHash, BisonMethod callback);
  private static native long nativeCall(long handle, int nameHash, long paramsHandle);

  private static native long nativeSize(long handle);
  private static native byte[] nativeSerialize(long handle);
  private static native String nativeToJson(long handle, int indent);
  private static native String nativeToYaml(long handle);
}
