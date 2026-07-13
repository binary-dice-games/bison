// MIT License © 2025 Binary Dice Games
/**
 * @file Attributes.cs
 * @brief Optional display/documentation metadata for a class, field, or
 *        method -- mirrors `bison_attributes` / `bison.Attributes`.
 */

using System.Runtime.InteropServices;

namespace Bdg.Bison;

/// <summary>Optional display/documentation metadata for a class, field, or method.</summary>
public sealed record Attributes
{
    public string? DisplayName { get; init; }
    public string? Description { get; init; }
    public string? Category { get; init; }
    public bool Obsolete { get; init; }
    public string? ObsoleteMessage { get; init; }
    public bool Required { get; init; }

    /// <summary>Pins the UTF-8-encoded strings for the lifetime of the
    /// returned scope and produces the native struct pointing at them.
    /// Dispose the scope only after the native call that consumed the
    /// pointer has returned.</summary>
    internal NativeAttributesScope ToNative() => new(this);

    internal static Attributes FromNative(in NativeAttributes native) => new()
    {
        DisplayName = MarshalUtf8(native.DisplayName),
        Description = MarshalUtf8(native.Description),
        Category = MarshalUtf8(native.Category),
        Obsolete = native.Obsolete != 0,
        ObsoleteMessage = MarshalUtf8(native.ObsoleteMessage),
        Required = native.Required != 0,
    };

    private static string? MarshalUtf8(nint ptr) => ptr == nint.Zero ? null : Marshal.PtrToStringUTF8(ptr);
}

/// <summary>
/// Owns the UTF-8 buffers backing a <see cref="NativeAttributes"/> struct so
/// they outlive the P/Invoke call that reads them. Mirrors how the Python
/// binding's <c>Attributes._to_c()</c> leans on ctypes' own string-to-bytes
/// keep-alive semantics, made explicit here since C# has no such implicit
/// lifetime extension for manually marshalled pointers.
/// </summary>
internal sealed class NativeAttributesScope : IDisposable
{
    public NativeAttributes Native;
    private readonly List<nint> _owned = new();

    public NativeAttributesScope(Attributes attrs)
    {
        Native = new NativeAttributes
        {
            DisplayName = Alloc(attrs.DisplayName),
            Description = Alloc(attrs.Description),
            Category = Alloc(attrs.Category),
            Obsolete = attrs.Obsolete ? 1 : 0,
            ObsoleteMessage = Alloc(attrs.ObsoleteMessage),
            Required = attrs.Required ? 1 : 0,
        };
    }

    private nint Alloc(string? s)
    {
        if (s is null)
        {
            return nint.Zero;
        }
        var ptr = Marshal.StringToCoTaskMemUTF8(s);
        _owned.Add(ptr);
        return ptr;
    }

    public void Dispose()
    {
        foreach (var ptr in _owned)
        {
            Marshal.FreeCoTaskMem(ptr);
        }
        _owned.Clear();
    }
}
