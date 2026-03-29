package com.bdg.bison;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.AfterEach;
import org.junit.jupiter.api.BeforeEach;

import static org.junit.jupiter.api.Assertions.*;

/**
 * JUnit 5 tests for the Bison Java binding.
 *
 * <p>Build {@code bison_c} first, then run from the {@code java/} directory:
 * <pre>{@code
 * cmake -B ../build -DPACKAGE_TESTS=ON
 * cmake --build ../build --config Debug --target bison_c
 * mvn test
 * }</pre>
 */
class BisonTest {

    // ═════════════════════════════════════════════════════════════════════════
    // 1. Lifecycle
    // ═════════════════════════════════════════════════════════════════════════

    @Test
    void testCreateSucceeds() {
        Dynamic obj = new Dynamic();
        assertNotNull(obj);
        obj.close();
    }

    @Test
    void testTryWithResourcesReleasesHandle() {
        // If close() does not throw the context manager works.
        try (Dynamic obj = new Dynamic()) {
            obj.setInt("x", 1);
        }
    }

    @Test
    void testCloseIsIdempotent() {
        Dynamic obj = new Dynamic();
        obj.close();
        obj.close(); // must not throw
    }

    @Test
    void testAddRefReturnsNewObject() {
        try (Dynamic obj = new Dynamic()) {
            obj.setInt("v", 99);
            try (Dynamic ref = obj.addRef()) {
                assertEquals(99, ref.getInt("v"));
            }
        }
    }

    @Test
    void testAddRefSharedMutation() {
        try (Dynamic obj = new Dynamic()) {
            obj.setInt("n", 10);
            try (Dynamic ref = obj.addRef()) {
                obj.setInt("n", 20);
                // Both handles share the same underlying object.
                assertEquals(20, ref.getInt("n"));
            }
        }
    }

    // ═════════════════════════════════════════════════════════════════════════
    // 2. Named field setters / getters
    // ═════════════════════════════════════════════════════════════════════════

    private Dynamic obj;

    @BeforeEach
    void setUp() {
        obj = new Dynamic();
    }

    @AfterEach
    void tearDown() {
        obj.close();
    }

    @Test
    void testIntRoundTrip() {
        obj.setInt("score", 42);
        assertEquals(42, obj.getInt("score"));
    }

    @Test
    void testFloatRoundTrip() {
        obj.setFloat("ratio", 3.14f);
        assertEquals(3.14f, obj.getFloat("ratio"), 1e-4f);
    }

    @Test
    void testBoolRoundTripTrue() {
        obj.setBool("flag", true);
        assertTrue(obj.getBool("flag"));
    }

    @Test
    void testBoolRoundTripFalse() {
        obj.setBool("flag", false);
        assertFalse(obj.getBool("flag"));
    }

    @Test
    void testStringRoundTrip() {
        obj.setString("name", "alice");
        assertEquals("alice", obj.getString("name"));
    }

    @Test
    void testNestedObjectRoundTrip() {
        try (Dynamic child = new Dynamic()) {
            child.setInt("x", 7);
            obj.setObject("inner", child);
        }
        try (Dynamic inner = obj.getObject("inner")) {
            assertEquals(7, inner.getInt("x"));
        }
    }

    @Test
    void testTypeMismatchThrows() {
        obj.setInt("n", 1);
        // Attempting to re-set as float should throw BISON_ERR_TYPE.
        BisonError e = assertThrows(BisonError.class,
                () -> obj.setFloat("n", 3.14f));
        assertEquals(BisonLibrary.BISON_ERR_TYPE, e.getCode());
    }

    // ═════════════════════════════════════════════════════════════════════════
    // 3. Indexed (array-like) fields
    // ═════════════════════════════════════════════════════════════════════════

    @Test
    void testIntAt() {
        Dynamic arr = new Dynamic();
        arr.setIntAt(0, 10);
        arr.setIntAt(1, 20);
        arr.setIntAt(2, 30);
        assertEquals(10, arr.getIntAt(0));
        assertEquals(20, arr.getIntAt(1));
        assertEquals(30, arr.getIntAt(2));
        arr.close();
    }

    @Test
    void testFloatAt() {
        Dynamic arr = new Dynamic();
        arr.setFloatAt(0, 1.5f);
        assertEquals(1.5f, arr.getFloatAt(0), 1e-4f);
        arr.close();
    }

    @Test
    void testStringAt() {
        Dynamic arr = new Dynamic();
        arr.setStringAt(0, "hello");
        assertEquals("hello", arr.getStringAt(0));
        arr.close();
    }

    @Test
    void testSize() {
        Dynamic arr = new Dynamic();
        assertEquals(0L, arr.size());
        arr.setIntAt(0, 1);
        arr.setIntAt(1, 2);
        assertEquals(2L, arr.size());
        arr.close();
    }

    // ═════════════════════════════════════════════════════════════════════════
    // 4. Import helpers
    // ═════════════════════════════════════════════════════════════════════════

    @Test
    void testFromJsonFlat() {
        try (Dynamic d = Dynamic.fromJson("{\"x\": 1, \"y\": 2}")) {
            assertEquals(1, d.getInt("x"));
            assertEquals(2, d.getInt("y"));
        }
    }

    @Test
    void testFromJsonNested() {
        try (Dynamic d = Dynamic.fromJson("{\"a\": {\"b\": 3}}")) {
            try (Dynamic inner = d.getObject("a")) {
                assertEquals(3, inner.getInt("b"));
            }
        }
    }

    @Test
    void testFromJsonInvalidThrows() {
        assertThrows(BisonError.class, () -> Dynamic.fromJson("{broken"));
    }

    @Test
    void testFromYamlFlat() {
        try (Dynamic d = Dynamic.fromYaml("x: 10\nname: test\n")) {
            assertEquals(10, d.getInt("x"));
            assertEquals("test", d.getString("name"));
        }
    }

    @Test
    void testFromYamlBool() {
        try (Dynamic d = Dynamic.fromYaml("flag: true\n")) {
            assertTrue(d.getBool("flag"));
        }
    }

    @Test
    void testFromYamlSequence() {
        try (Dynamic d = Dynamic.fromYaml("- 1\n- 2\n- 3\n")) {
            assertEquals(3L, d.size());
            assertEquals(1, d.getIntAt(0));
        }
    }

    @Test
    void testFromYamlInvalidThrows() {
        assertThrows(BisonError.class, () -> Dynamic.fromYaml("{broken"));
    }

    // ═════════════════════════════════════════════════════════════════════════
    // 5. BisonKey utility
    // ═════════════════════════════════════════════════════════════════════════

    @Test
    void testKeyHighBitAlwaysSet() {
        assertTrue((BisonKey.of("hello") & 0x80000000) != 0);
    }

    @Test
    void testKeyDeterministic() {
        assertEquals(BisonKey.of("foo"), BisonKey.of("foo"));
    }

    @Test
    void testKeyDifferentStringsDiffer() {
        assertNotEquals(BisonKey.of("alpha"), BisonKey.of("beta"));
    }

    // ═════════════════════════════════════════════════════════════════════════
    // 6. Class registry
    // ═════════════════════════════════════════════════════════════════════════

    @Test
    void testAddClassDoesNotCrash() {
        // Use a unique name per test run to avoid global registry conflicts.
        String className = "JavaTestShape_" + System.identityHashCode(this);
        try (Dynamic proto = new Dynamic(className)) {
            proto.addClass(""); // root class
        } catch (BisonError e) {
            // BISON_ERR_DUPLICATE is acceptable when the test is re-run in the
            // same process (global registry persists).
            assertEquals(BisonLibrary.BISON_ERR_DUPLICATE, e.getCode());
        }
    }

    // ═════════════════════════════════════════════════════════════════════════
    // 7. Methods
    // ═════════════════════════════════════════════════════════════════════════

    @Test
    void testAddAndCallMethod() {
        try (Dynamic d = new Dynamic()) {
            d.setInt("n", 0);
            d.addMethod("inc", (self, params) -> {
                int n = self.getInt("n");
                self.setInt("n", n + 1);
                return new Dynamic();
            });

            try (Dynamic p = new Dynamic()) {
                try (Dynamic r = d.call("inc", p)) { /* unused */ }
                try (Dynamic r = d.call("inc", p)) { /* unused */ }
            }

            assertEquals(2, d.getInt("n"));
        }
    }

    @Test
    void testCallMissingMethodThrows() {
        try (Dynamic d = new Dynamic()) {
            try (Dynamic p = new Dynamic()) {
                BisonError e = assertThrows(BisonError.class,
                        () -> d.call("no_such_method", p));
                assertEquals(BisonLibrary.BISON_ERR_NOT_FOUND, e.getCode());
            }
        }
    }

    @Test
    void testAddDuplicateMethodThrows() {
        try (Dynamic d = new Dynamic()) {
            d.addMethod("fn", (self, params) -> new Dynamic());
            BisonError e = assertThrows(BisonError.class,
                    () -> d.addMethod("fn", (self, params) -> new Dynamic()));
            assertEquals(BisonLibrary.BISON_ERR_DUPLICATE, e.getCode());
        }
    }
}
