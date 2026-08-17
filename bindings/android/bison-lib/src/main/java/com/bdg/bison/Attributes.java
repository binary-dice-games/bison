// MIT License © 2025 Binary Dice Games
// Permission is hereby granted, free of charge, to use, copy, modify, and
// distribute this file. See the LICENSE file or
// https://opensource.org/licenses/MIT for details.

package com.bdg.bison;

/**
 * Optional display/documentation metadata for a class, field, or method --
 * mirrors {@code bison_attributes} (see {@code include/bison_c.h}) and the
 * C# binding's {@code Attributes}.
 *
 * <p>Field layout and constructor signature are read directly by
 * {@code bison_jni.cpp} (via cached {@code jfieldID}s and a cached
 * constructor {@code jmethodID}), so both must stay in sync with
 * {@code jni_util.cpp}'s {@code attributes_view}/{@code new_attributes}.
 */
public final class Attributes {
  public final String displayName;
  public final String description;
  public final String category;
  public final boolean obsolete;
  public final String obsoleteMessage;
  public final boolean required;

  public Attributes() {
    this(null, null, null, false, null, false);
  }

  public Attributes(
      String displayName, String description, String category, boolean obsolete, String obsoleteMessage,
      boolean required) {
    this.displayName = displayName;
    this.description = description;
    this.category = category;
    this.obsolete = obsolete;
    this.obsoleteMessage = obsoleteMessage;
    this.required = required;
  }
}
