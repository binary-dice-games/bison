package com.bdg.bison.examples;

import com.bdg.bison.BisonError;
import com.bdg.bison.BisonKey;
import com.bdg.bison.Dynamic;

/**
 * Usage examples for the Bison Java binding.
 *
 * <p>Build {@code bison_c} first, then run from the repository root:
 * <pre>{@code
 * cmake -B build -DPACKAGE_TESTS=ON
 * cmake --build build --config Debug --target bison_c
 * cd bindings/java && mvn compile exec:java -Dexec.mainClass=com.bdg.bison.examples.BisonExamples
 * }</pre>
 *
 * <p>Each method is a self-contained example.  {@link #main(String[])} runs
 * them all in sequence.
 */
public class BisonExamples {

    // ── helpers ───────────────────────────────────────────────────────────────

    private static void section(String title) {
        System.out.println();
        System.out.println("------------------------------------------------------------");
        System.out.println("  " + title);
        System.out.println("------------------------------------------------------------");
    }

    // ═════════════════════════════════════════════════════════════════════════
    // Example 1 — Basic field get/set
    // ═════════════════════════════════════════════════════════════════════════

    static void example1BasicFields() {
        section("Example 1: basic field get/set");

        try (Dynamic obj = new Dynamic()) {
            obj.setInt("hp",     100);
            obj.setFloat("speed", 9.5f);
            obj.setBool("alive", true);
            obj.setString("name", "hero");

            System.out.printf("hp    = %d%n",    obj.getInt("hp"));
            System.out.printf("speed = %.2f%n",  obj.getFloat("speed"));
            System.out.printf("alive = %b%n",    obj.getBool("alive"));
            System.out.printf("name  = %s%n",    obj.getString("name"));
        }
    }

    // ═════════════════════════════════════════════════════════════════════════
    // Example 2 — Numeric (array-like) indices
    // ═════════════════════════════════════════════════════════════════════════

    static void example2ArrayAccess() {
        section("Example 2: array-like indexed fields");

        try (Dynamic arr = new Dynamic()) {
            arr.setStringAt(0, "apple");
            arr.setStringAt(1, "banana");
            arr.setStringAt(2, "cherry");

            System.out.printf("size = %d%n", arr.size());
            for (int i = 0; i < arr.size(); i++) {
                System.out.printf("  [%d] = %s%n", i, arr.getStringAt(i));
            }
        }
    }

    // ═════════════════════════════════════════════════════════════════════════
    // Example 3 — Nested objects
    // ═════════════════════════════════════════════════════════════════════════

    static void example3NestedObjects() {
        section("Example 3: nested objects");

        try (Dynamic player   = new Dynamic();
             Dynamic position = new Dynamic()) {
            player.setString("name", "alice");
            position.setFloat("x", 1.0f);
            position.setFloat("y", 2.5f);
            player.setObject("position", position);
        }

        // Re-create to demonstrate retrieval pattern.
        try (Dynamic player = new Dynamic()) {
            player.setString("name", "alice");

            try (Dynamic pos = new Dynamic()) {
                pos.setFloat("x", 1.0f);
                pos.setFloat("y", 2.5f);
                player.setObject("position", pos);
            }

            try (Dynamic pos = player.getObject("position")) {
                System.out.printf("pos.x = %.1f%n", pos.getFloat("x")); // 1.0
                System.out.printf("pos.y = %.1f%n", pos.getFloat("y")); // 2.5
            }
        }
    }

    // ═════════════════════════════════════════════════════════════════════════
    // Example 4 — JSON import
    // ═════════════════════════════════════════════════════════════════════════

    static void example4JsonImport() {
        section("Example 4: JSON import");

        String jsonText = "{\"player\": {\"name\": \"bob\", \"score\": 250}, \"level\": 3}";
        try (Dynamic root = Dynamic.fromJson(jsonText)) {
            System.out.printf("level = %d%n", root.getInt("level"));

            try (Dynamic player = root.getObject("player")) {
                System.out.printf("player.name  = %s%n", player.getString("name"));
                System.out.printf("player.score = %d%n", player.getInt("score"));
            }
        }
    }

    // ═════════════════════════════════════════════════════════════════════════
    // Example 5 — YAML import
    // ═════════════════════════════════════════════════════════════════════════

    static void example5YamlImport() {
        section("Example 5: YAML import");

        String yamlText =
            "server:\n" +
            "  host: localhost\n" +
            "  port: 8080\n" +
            "debug: true\n" +
            "max_connections: 100\n";

        try (Dynamic cfg = Dynamic.fromYaml(yamlText)) {
            try (Dynamic server = cfg.getObject("server")) {
                System.out.printf("host = %s%n", server.getString("host"));
                System.out.printf("port = %d%n", server.getInt("port"));
            }
            System.out.printf("debug           = %b%n", cfg.getBool("debug"));
            System.out.printf("max_connections = %d%n", cfg.getInt("max_connections"));
        }
    }

    // ═════════════════════════════════════════════════════════════════════════
    // Example 6 — Method registration
    // ═════════════════════════════════════════════════════════════════════════

    static void example6Methods() {
        section("Example 6: method registration and invocation");

        try (Dynamic obj = new Dynamic()) {
            obj.setInt("counter", 5);

            // Register a method that doubles 'counter'.
            obj.addMethod("double", (self, params) -> {
                int current = self.getInt("counter");
                self.setInt("counter", current * 2);
                return new Dynamic();
            });

            try (Dynamic params = new Dynamic();
                 Dynamic result = obj.call("double", params)) {
                // result is unused here
            }

            System.out.printf("counter after double = %d%n", obj.getInt("counter")); // 10
        }
    }

    // ═════════════════════════════════════════════════════════════════════════
    // Example 7 — BisonKey utility
    // ═════════════════════════════════════════════════════════════════════════

    static void example7KeyUtility() {
        section("Example 7: BisonKey utility");

        int k = BisonKey.of("velocity");
        System.out.printf("key('velocity') = 0x%08x%n", k);
        System.out.printf("high bit set    = %b%n", (k & 0x80000000) != 0);
        System.out.printf("deterministic   = %b%n", k == BisonKey.of("velocity"));
    }

    // ═════════════════════════════════════════════════════════════════════════
    // Example 8 — Class hierarchy
    // ═════════════════════════════════════════════════════════════════════════

    static void example8ClassRegistry() {
        section("Example 8: class registry");

        // Register a 'JavaVehicle' prototype (unique name to avoid conflicts
        // with other tests that might run in the same process).
        try (Dynamic proto = new Dynamic("JavaVehicle")) {
            proto.setInt("wheels", 4);
            try {
                proto.addClass("");
            } catch (BisonError e) {
                // already registered – acceptable in repeated runs
            }
        }

        // Create an instance and read back the inherited field.
        try (Dynamic car = new Dynamic("JavaVehicle")) {
            System.out.printf("wheels (inherited) = %d%n", car.getInt("wheels")); // 4

            // Demonstrate addRef: both car and car2 share the same native object.
            try (Dynamic car2 = car.addRef()) {
                car.setInt("wheels", 3);
                System.out.printf("car.wheels  = %d%n", car.getInt("wheels"));   // 3
                System.out.printf("car2.wheels = %d%n", car2.getInt("wheels"));  // 3 – shared!
            }
        }
    }

    // ── Entry point ───────────────────────────────────────────────────────────

    public static void main(String[] args) {
        System.out.println("Bison Java binding -- usage examples");
        example1BasicFields();
        example2ArrayAccess();
        example3NestedObjects();
        example4JsonImport();
        example5YamlImport();
        example6Methods();
        example7KeyUtility();
        example8ClassRegistry();
        System.out.println("\nAll examples completed successfully.");
    }
}
