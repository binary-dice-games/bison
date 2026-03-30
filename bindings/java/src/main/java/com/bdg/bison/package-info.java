/**
 * Java JNA binding for the Bison dynamic-object library.
 *
 * <p>This package wraps the {@code libbison_c} shared library using
 * <a href="https://github.com/java-native-access/jna">JNA</a> (Java Native
 * Access), mirroring the same approach as the Python {@code ctypes} binding.
 *
 * <p>Before using these classes, build the native shared library:
 * <pre>{@code
 * cmake -B build -DPACKAGE_TESTS=ON
 * cmake --build build --config Debug --target bison_c
 * }</pre>
 *
 * <p>If the shared library is not in the default {@code build/} directory,
 * set the {@code BISON_LIB} environment variable to the full path of
 * {@code bison_c.dll}, {@code libbison_c.so}, or {@code libbison_c.dylib}
 * before running.
 *
 * <h2>Quick start</h2>
 * <pre>{@code
 * try (Dynamic obj = new Dynamic()) {
 *     obj.setInt("hp", 100);
 *     obj.setFloat("speed", 9.5f);
 *     obj.setBool("alive", true);
 *     obj.setString("name", "hero");
 *
 *     System.out.println("hp    = " + obj.getInt("hp"));
 *     System.out.println("speed = " + obj.getFloat("speed"));
 *     System.out.println("alive = " + obj.getBool("alive"));
 *     System.out.println("name  = " + obj.getString("name"));
 * }
 * }</pre>
 */
package com.bdg.bison;
