using System.Runtime.InteropServices;
using System.Reflection;
using System.Collections.Generic;

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

    static Native()
    {
        NativeLibrary.SetDllImportResolver(typeof(Native).Assembly, ResolveImport);
    }

    private static IntPtr ResolveImport(
        string libraryName,
        Assembly assembly,
        DllImportSearchPath? searchPath)
    {
        if (!string.Equals(libraryName, LibName, StringComparison.Ordinal))
        {
            return IntPtr.Zero;
        }

        string resolved = ResolveLibPath();

        if (Path.IsPathRooted(resolved) && File.Exists(resolved) &&
            NativeLibrary.TryLoad(resolved, out IntPtr fromPath))
        {
            return fromPath;
        }

        if (NativeLibrary.TryLoad(resolved, assembly, searchPath, out IntPtr fromName))
        {
            return fromName;
        }

        return IntPtr.Zero;
    }

    /// <summary>
    /// Return the resolved native library path based on environment or
    /// canonical build/ directory probing.
    /// </summary>
    internal static string ResolveLibPath()
    {
        var env = Environment.GetEnvironmentVariable("BISON_LIB");
        if (!string.IsNullOrEmpty(env)) return env;

        var probeRoots = new HashSet<string>(StringComparer.OrdinalIgnoreCase);

        AddRootAndParents(probeRoots, Directory.GetCurrentDirectory());
        AddRootAndParents(probeRoots, AppContext.BaseDirectory);
        AddRootAndParents(probeRoots, Path.GetDirectoryName(typeof(Native).Assembly.Location));

        string[] nativeNames = {
            "bison_c.dll",
            "libbison_c.so",
            "libbison_c.dylib",
        };

        foreach (var root in probeRoots)
        {
            foreach (var name in nativeNames)
            {
                string[] candidates = {
                    Path.Combine(root, name),
                    Path.Combine(root, "build", name),
                    Path.Combine(root, "build", "Debug", name),
                    Path.Combine(root, "build", "Release", name),
                };

                foreach (var candidate in candidates)
                {
                    if (File.Exists(candidate)) return Path.GetFullPath(candidate);
                }
            }
        }

        return LibName; // fall back to system search path
    }

    private static void AddRootAndParents(HashSet<string> roots, string? start)
    {
        if (string.IsNullOrWhiteSpace(start)) return;

        var dir = new DirectoryInfo(Path.GetFullPath(start));
        if (!dir.Exists)
        {
            dir = dir.Parent ?? dir;
        }

        while (dir is not null)
        {
            roots.Add(dir.FullName);
            dir = dir.Parent;
        }
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
