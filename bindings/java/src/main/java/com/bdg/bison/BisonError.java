package com.bdg.bison;

/**
 * Signals that a {@code libbison_c} API call returned a non-zero error code.
 *
 * <p>The {@link #getCode()} method returns the raw integer error code; the
 * message contains a human-readable description.
 *
 * <p>Error codes map to the {@code BISON_ERR_*} constants in
 * {@link BisonLibrary}.
 */
public class BisonError extends RuntimeException {

    private final int code;

    /**
     * Construct a {@code BisonError} from a raw error code.
     *
     * @param code    the raw {@code bison_error} integer value.
     * @param context a short description of the operation that failed (may be
     *                empty or {@code null}).
     */
    public BisonError(int code, String context) {
        super(buildMessage(code, context));
        this.code = code;
    }

    /** Convenience constructor with no context string. */
    public BisonError(int code) {
        this(code, null);
    }

    /**
     * Return the raw {@code bison_error} code.
     *
     * @return negative error code; compare against {@code BisonLibrary.BISON_ERR_*}.
     */
    public int getCode() {
        return code;
    }

    private static String buildMessage(int code, String context) {
        String description = switch (code) {
            case BisonLibrary.BISON_ERR_NULL      -> "Null handle or pointer";
            case BisonLibrary.BISON_ERR_TYPE      -> "Field type mismatch";
            case BisonLibrary.BISON_ERR_NOT_FOUND -> "Method or field not found";
            case BisonLibrary.BISON_ERR_DUPLICATE -> "Duplicate class or method";
            case BisonLibrary.BISON_ERR_EXCEPTION -> "Internal C++ exception";
            case BisonLibrary.BISON_ERR_PARSE     -> "Parse error (JSON / YAML)";
            default                               -> "Unknown error " + code;
        };
        return (context != null && !context.isEmpty())
               ? context + ": " + description
               : description;
    }
}
