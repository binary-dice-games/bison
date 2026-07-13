// MIT License © 2025 Binary Dice Games
/**
 * @file ParamsMarshal.cs
 * @brief Converts the loosely-typed "params" argument accepted throughout
 *        the RMI API (a `Dynamic`, an `IDictionary`, or `null`) into a raw
 *        `bison_handle` for the duration of one native call.
 */

namespace Bdg.Bison;

/// <summary>
/// Yields a raw <c>bison_handle</c> (or <see cref="nint.Zero"/>) for a loosely
/// typed params argument, disposing any scratch <see cref="Dynamic"/> it had
/// to allocate once the scope ends. Mirrors <c>bison.rmi._as_params()</c>.
/// </summary>
internal readonly struct ParamsScope : IDisposable
{
    public nint Handle { get; }
    private readonly Dynamic? _scratch;

    public ParamsScope(nint handle, Dynamic? scratch)
    {
        Handle = handle;
        _scratch = scratch;
    }

    public void Dispose() => _scratch?.Release();
}

internal static class ParamsMarshal
{
    /// <summary>
    /// Accepts a <see cref="Dynamic"/> (passed through untouched -- its
    /// lifetime remains the caller's responsibility), an
    /// <see cref="IDictionary{TKey,TValue}"/> of <c>string</c> to <c>object?</c>
    /// (converted into a scratch <see cref="Dynamic"/> released when the
    /// scope is disposed), or <c>null</c>.
    /// </summary>
    public static ParamsScope From(object? parameters)
    {
        switch (parameters)
        {
            case null:
                return new ParamsScope(nint.Zero, null);
            case Dynamic dyn:
                return new ParamsScope(dyn.Handle, null);
            case IDictionary<string, object?> dict:
            {
                var scratch = new Dynamic();
                foreach (var (k, v) in dict)
                {
                    scratch[k] = v;
                }
                return new ParamsScope(scratch.Handle, scratch);
            }
            default:
                throw new ArgumentException($"params must be a Dynamic, IDictionary<string, object?>, or null, not {parameters.GetType()}");
        }
    }
}
