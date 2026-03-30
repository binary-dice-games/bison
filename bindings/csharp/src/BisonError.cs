namespace Bdg.Bison;

/// <summary>
/// Signals that a <c>libbison_c</c> API call returned a non-zero error code.
/// </summary>
/// <remarks>
/// The <see cref="Code"/> property contains the raw integer error code.
/// Compare it against the <c>BISON_ERR_*</c> constants exposed by
/// <see cref="BisonError"/> itself.
/// </remarks>
public sealed class BisonError : Exception
{
    // ── Well-known error codes ────────────────────────────────────────────────

    /// <summary>A required handle or pointer was <c>NULL</c>.</summary>
    public const int ErrNull      = Native.BISON_ERR_NULL;
    /// <summary>The field holds a different type than requested.</summary>
    public const int ErrType      = Native.BISON_ERR_TYPE;
    /// <summary>Method or field not found.</summary>
    public const int ErrNotFound  = Native.BISON_ERR_NOT_FOUND;
    /// <summary>Attempted to add a duplicate class or method.</summary>
    public const int ErrDuplicate = Native.BISON_ERR_DUPLICATE;
    /// <summary>An unexpected C++ exception was caught.</summary>
    public const int ErrException = Native.BISON_ERR_EXCEPTION;
    /// <summary>Input string failed to parse (JSON / YAML).</summary>
    public const int ErrParse     = Native.BISON_ERR_PARSE;

    // ── Constructor ───────────────────────────────────────────────────────────

    /// <summary>
    /// Construct a <see cref="BisonError"/> from a raw error code.
    /// </summary>
    /// <param name="code">Raw <c>bison_error</c> integer.</param>
    /// <param name="context">Short description of the operation that failed.</param>
    public BisonError(int code, string? context = null)
        : base(BuildMessage(code, context))
    {
        Code = code;
    }

    /// <summary>The raw <c>bison_error</c> code.</summary>
    public int Code { get; }

    private static string BuildMessage(int code, string? context)
    {
        string description = code switch
        {
            Native.BISON_ERR_NULL      => "Null handle or pointer",
            Native.BISON_ERR_TYPE      => "Field type mismatch",
            Native.BISON_ERR_NOT_FOUND => "Method or field not found",
            Native.BISON_ERR_DUPLICATE => "Duplicate class or method",
            Native.BISON_ERR_EXCEPTION => "Internal C++ exception",
            Native.BISON_ERR_PARSE     => "Parse error (JSON / YAML)",
            _                          => $"Unknown error {code}",
        };

        return string.IsNullOrEmpty(context)
               ? description
               : $"{context}: {description}";
    }
}
