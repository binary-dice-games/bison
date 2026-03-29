using System.Runtime.InteropServices;

namespace Bdg.Bison;

/// <summary>
/// P/Invoke declarations for every exported function in <c>libbison_c</c>.
/// </summary>
/// <remarks>
/// This class is an implementation detail.  End-users should use
/// <see cref="Dynamic"/> instead.  The native library is located using the
/// following precedence:
/// <list type="number">
///   <item>The <c>BISON_LIB</c> environment variable (full path).</item>
///   <item>A <c>build/</c> sibling of the repository root.</item>
///   <item>The OS loader's default search path (e.g. <c>LD_LIBRARY_PATH</c>).</item>
/// </list>
/// </remarks>
internal static class Native
{
    // ── Error codes ──────────────────────────────────────────────────────────

    internal const int BISON_OK             =  0;
    internal const int BISON_ERR_NULL       = -1;
    internal const int BISON_ERR_TYPE       = -2;
    internal const int BISON_ERR_NOT_FOUND  = -3;
    internal const int BISON_ERR_DUPLICATE  = -4;
    internal const int BISON_ERR_EXCEPTION  = -5;
    internal const int BISON_ERR_PARSE      = -6;

    // ── Method callback delegate ─────────────────────────────────────────────

    /// <summary>Delegate type for methods registered on dynamic objects.</summary>
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    internal delegate void MethodDelegate(
        IntPtr self, IntPtr @params, IntPtr result, IntPtr user);

    // ── Library name resolution ──────────────────────────────────────────────

    private const string LibName = "bison_c";

    /// <summary>
    /// Return the resolved native library path based on environment or
    /// canonical build/ directory probing.
    /// </summary>
    internal static string ResolveLibPath()
    {
        var env = Environment.GetEnvironmentVariable("BISON_LIB");
        if (!string.IsNullOrEmpty(env)) return env;

        // Try the canonical build/ sub-directories relative to cwd.
        string[] candidates = {
            Path.Combine("..", "build", "libbison_c.so"),    // Linux
            Path.Combine("..", "build", "libbison_c.dylib"), // macOS
            Path.Combine("..", "build", "Release", "bison_c.dll"),
            Path.Combine("..", "build", "Debug",   "bison_c.dll"),
        };
        foreach (var candidate in candidates)
        {
            if (File.Exists(candidate)) return Path.GetFullPath(candidate);
        }

        return LibName; // fall back to system search path
    }

    // ── Lifecycle ────────────────────────────────────────────────────────────

    [DllImport(LibName, CallingConvention = CallingConvention.Cdecl,
               EntryPoint = "bison_create")]
    internal static extern IntPtr bison_create(uint klassName);

    [DllImport(LibName, CallingConvention = CallingConvention.Cdecl,
               EntryPoint = "bison_instantiate")]
    internal static extern IntPtr bison_instantiate(uint klassName);

    [DllImport(LibName, CallingConvention = CallingConvention.Cdecl,
               EntryPoint = "bison_add_ref")]
    internal static extern IntPtr bison_add_ref(IntPtr h);

    [DllImport(LibName, CallingConvention = CallingConvention.Cdecl,
               EntryPoint = "bison_release")]
    internal static extern void bison_release(IntPtr h);

    // ── Import helpers ───────────────────────────────────────────────────────

    [DllImport(LibName, CallingConvention = CallingConvention.Cdecl,
               EntryPoint = "bison_from_json", CharSet = CharSet.Ansi)]
    internal static extern IntPtr bison_from_json([MarshalAs(UnmanagedType.LPStr)] string json);

    [DllImport(LibName, CallingConvention = CallingConvention.Cdecl,
               EntryPoint = "bison_from_yaml", CharSet = CharSet.Ansi)]
    internal static extern IntPtr bison_from_yaml([MarshalAs(UnmanagedType.LPStr)] string yaml);

    // ── Class registry ───────────────────────────────────────────────────────

    [DllImport(LibName, CallingConvention = CallingConvention.Cdecl,
               EntryPoint = "bison_add_class")]
    internal static extern int bison_add_class(uint parentName, IntPtr klass);

    [DllImport(LibName, CallingConvention = CallingConvention.Cdecl,
               EntryPoint = "bison_find_class")]
    internal static extern IntPtr bison_find_class(IntPtr h, uint name);

    // ── Named setters ─────────────────────────────────────────────────────────

    [DllImport(LibName, CallingConvention = CallingConvention.Cdecl,
               EntryPoint = "bison_set_int")]
    internal static extern int bison_set_int(IntPtr h, uint name, int value);

    [DllImport(LibName, CallingConvention = CallingConvention.Cdecl,
               EntryPoint = "bison_set_float")]
    internal static extern int bison_set_float(IntPtr h, uint name, float value);

    [DllImport(LibName, CallingConvention = CallingConvention.Cdecl,
               EntryPoint = "bison_set_bool")]
    internal static extern int bison_set_bool(IntPtr h, uint name, int value);

    [DllImport(LibName, CallingConvention = CallingConvention.Cdecl,
               EntryPoint = "bison_set_string", CharSet = CharSet.Ansi)]
    internal static extern int bison_set_string(IntPtr h, uint name,
        [MarshalAs(UnmanagedType.LPStr)] string value);

    [DllImport(LibName, CallingConvention = CallingConvention.Cdecl,
               EntryPoint = "bison_set_object")]
    internal static extern int bison_set_object(IntPtr h, uint name, IntPtr value);

    // ── Indexed setters ───────────────────────────────────────────────────────

    [DllImport(LibName, CallingConvention = CallingConvention.Cdecl,
               EntryPoint = "bison_set_int_at")]
    internal static extern int bison_set_int_at(IntPtr h, UIntPtr index, int value);

    [DllImport(LibName, CallingConvention = CallingConvention.Cdecl,
               EntryPoint = "bison_set_float_at")]
    internal static extern int bison_set_float_at(IntPtr h, UIntPtr index, float value);

    [DllImport(LibName, CallingConvention = CallingConvention.Cdecl,
               EntryPoint = "bison_set_string_at", CharSet = CharSet.Ansi)]
    internal static extern int bison_set_string_at(IntPtr h, UIntPtr index,
        [MarshalAs(UnmanagedType.LPStr)] string value);

    // ── Named getters ─────────────────────────────────────────────────────────

    [DllImport(LibName, CallingConvention = CallingConvention.Cdecl,
               EntryPoint = "bison_get_int")]
    internal static extern int bison_get_int(IntPtr h, uint name, out int outVal);

    [DllImport(LibName, CallingConvention = CallingConvention.Cdecl,
               EntryPoint = "bison_get_float")]
    internal static extern int bison_get_float(IntPtr h, uint name, out float outVal);

    [DllImport(LibName, CallingConvention = CallingConvention.Cdecl,
               EntryPoint = "bison_get_bool")]
    internal static extern int bison_get_bool(IntPtr h, uint name, out int outVal);

    [DllImport(LibName, CallingConvention = CallingConvention.Cdecl,
               EntryPoint = "bison_get_string")]
    internal static extern int bison_get_string(
        IntPtr h, uint name, IntPtr buf, UIntPtr bufLen, out UIntPtr lenOut);

    [DllImport(LibName, CallingConvention = CallingConvention.Cdecl,
               EntryPoint = "bison_get_object")]
    internal static extern int bison_get_object(IntPtr h, uint name, out IntPtr outHandle);

    // ── Indexed getters ───────────────────────────────────────────────────────

    [DllImport(LibName, CallingConvention = CallingConvention.Cdecl,
               EntryPoint = "bison_get_int_at")]
    internal static extern int bison_get_int_at(IntPtr h, UIntPtr index, out int outVal);

    [DllImport(LibName, CallingConvention = CallingConvention.Cdecl,
               EntryPoint = "bison_get_float_at")]
    internal static extern int bison_get_float_at(IntPtr h, UIntPtr index, out float outVal);

    [DllImport(LibName, CallingConvention = CallingConvention.Cdecl,
               EntryPoint = "bison_get_string_at")]
    internal static extern int bison_get_string_at(
        IntPtr h, UIntPtr index, IntPtr buf, UIntPtr bufLen, out UIntPtr lenOut);

    [DllImport(LibName, CallingConvention = CallingConvention.Cdecl,
               EntryPoint = "bison_size")]
    internal static extern UIntPtr bison_size(IntPtr h);

    // ── Methods ───────────────────────────────────────────────────────────────

    [DllImport(LibName, CallingConvention = CallingConvention.Cdecl,
               EntryPoint = "bison_add_method")]
    internal static extern int bison_add_method(IntPtr h, uint name,
        MethodDelegate fn, IntPtr user);

    [DllImport(LibName, CallingConvention = CallingConvention.Cdecl,
               EntryPoint = "bison_call")]
    internal static extern int bison_call(IntPtr h, uint name, IntPtr @params,
        out IntPtr result);

    // ── Utility ──────────────────────────────────────────────────────────────

    [DllImport(LibName, CallingConvention = CallingConvention.Cdecl,
               EntryPoint = "bison_key", CharSet = CharSet.Ansi)]
    internal static extern uint bison_key([MarshalAs(UnmanagedType.LPStr)] string name);
}
