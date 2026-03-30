namespace Bdg.Bison;

/// <summary>
/// Utility class for computing FNV-1a hashes of field and class names.
/// </summary>
/// <remarks>
/// The hash function is identical to the C++ <c>"name"_key</c> literal:
/// 32-bit FNV-1a with the high bit always set.
///
/// <para>
/// <see cref="Dynamic"/> accepts plain <see cref="string"/> names in all
/// public methods and calls <see cref="Of"/> internally, so most callers
/// do not need to use this class directly.
/// </para>
/// </remarks>
/// <example>
/// <code>
/// uint k = BisonKey.Of("velocity");
/// Console.WriteLine($"key('velocity') = 0x{k:x8}");
/// // The high bit is always set.
/// Debug.Assert((k &amp; 0x80000000u) != 0);
/// </code>
/// </example>
public static class BisonKey
{
    /// <summary>
    /// Compute the FNV-1a hash of <paramref name="name"/>.
    /// </summary>
    /// <param name="name">Field or class name.</param>
    /// <returns>
    /// 32-bit FNV-1a hash with the high bit set, returned as an unsigned
    /// 32-bit integer.
    /// </returns>
    public static uint Of(string name) => Native.bison_key(name);
}
