// MIT License © 2025 Binary Dice Games
/**
 * @file BisonException.cs
 * @brief Exception type raised when a `bison_*` C API call returns a
 *        non-zero error code.
 */

namespace Bdg.Bison;

/// <summary>Raised when a <c>bison_*</c> C API call returns a non-zero error code.</summary>
public sealed class BisonException : Exception
{
    private static readonly Dictionary<BisonErrorCode, string> Messages = new()
    {
        [BisonErrorCode.Null] = "Null handle or pointer",
        [BisonErrorCode.Type] = "Field type mismatch",
        [BisonErrorCode.NotFound] = "Method or field not found",
        [BisonErrorCode.Duplicate] = "Duplicate class or method",
        [BisonErrorCode.Exception] = "Internal C++ exception",
        [BisonErrorCode.Parse] = "Parse error (JSON / YAML)",
    };

    public BisonErrorCode Code { get; }

    public BisonException(BisonErrorCode code, string context = "")
        : base(Format(code, context))
    {
        Code = code;
    }

    private static string Format(BisonErrorCode code, string context)
    {
        var msg = Messages.TryGetValue(code, out var m) ? m : $"Unknown error {(int)code}";
        return context.Length > 0 ? $"{context}: {msg}" : msg;
    }

    internal static void Check(int rc, string context = "")
    {
        if (rc != (int)BisonErrorCode.Ok)
        {
            throw new BisonException((BisonErrorCode)rc, context);
        }
    }
}
