// MIT License © 2025 Binary Dice Games
// Permission is hereby granted, free of charge, to use, copy, modify, and
// distribute this file. See the LICENSE file or
// https://opensource.org/licenses/MIT for details.

package com.bdg.bison;

import static org.junit.Assert.assertArrayEquals;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;
import static org.junit.Assert.fail;

import androidx.test.ext.junit.runners.AndroidJUnit4;
import com.bdg.bison.rmi.Client;
import com.bdg.bison.rmi.Proxy;
import org.junit.Test;
import org.junit.runner.RunWith;

/**
 * Runs the binding against the real {@code bison_abi}/{@code bison_jni}
 * {@code .so}s on-device -- {@code ./gradlew connectedAndroidTest} (see
 * docs/examples.md) is this Android platform's equivalent of the
 * {@code ctest}/{@code pytest}/{@code dotnet test} suites the other
 * bindings run, and is how this binding's Android build is validated on an
 * emulator/device rather than just compiled.
 */
@RunWith(AndroidJUnit4.class)
public class DynamicInstrumentedTest {

  @Test
  public void scalarFieldsRoundTrip() {
    try (Dynamic obj = new Dynamic("Player")) {
      obj.setInt("hp", 100);
      obj.setString("name", "hero");
      obj.setFloat("speed", 3.5f);
      obj.setBool("alive", true);

      assertEquals(100, obj.getInt("hp"));
      assertEquals("hero", obj.getString("name"));
      assertEquals(3.5f, obj.getFloat("speed"), 0.0f);
      assertTrue(obj.getBool("alive"));
    }
  }

  @Test
  public void serializationRoundTrips() {
    try (Dynamic obj = new Dynamic("Player")) {
      obj.setInt("hp", 42);
      byte[] bytes = obj.serialize();
      assertTrue(bytes.length > 0);
      try (Dynamic restored = Dynamic.deserialize(bytes)) {
        assertEquals(42, restored.getInt("hp"));
      }
      // Field keys are emitted as "#<hash>" via the C ABI (no key-name map
      // is exposed across it -- see bison_c.h's bison_to_json), so this
      // checks structural validity/values rather than matching on field
      // names, matching the Python/C# bindings' equivalent tests.
      assertTrue(obj.toJson().contains("42"));
    }
  }

  @Test
  public void vectorFieldsRoundTrip() {
    try (Dynamic obj = new Dynamic()) {
      obj.setVectorInt("tags", new int[] {1, 2, 3});
      assertArrayEquals(new int[] {1, 2, 3}, obj.getVectorInt("tags"));

      obj.setVectorBytes("payload", new byte[] {9, 8, 7});
      assertArrayEquals(new byte[] {9, 8, 7}, obj.getVectorBytes("payload"));
    }
  }

  @Test
  public void typeMismatchThrowsBisonException() {
    try (Dynamic obj = new Dynamic()) {
      obj.setString("name", "hero");
      try {
        obj.getInt("name");
        fail("expected BisonException");
      } catch (BisonException e) {
        assertEquals(-2 /* BISON_ERR_TYPE */, e.code);
      }
    }
  }

  @Test
  public void addMethodInvokesJavaCallback() {
    try (Dynamic calc = new Dynamic("Calculator")) {
      calc.addMethod("add", (self, params, result) -> result.setInt("value", params.getInt("a") + params.getInt("b")));
      try (Dynamic args = new Dynamic()) {
        args.setInt("a", 3);
        args.setInt("b", 4);
        try (Dynamic result = calc.call("add", args)) {
          assertEquals(7, result.getInt("value"));
        }
      }
    }
  }

  @Test
  public void rmiStandaloneRoundTrip() {
    try (Dynamic proto = new Dynamic("InstrumentedTestCalculator")) {
      proto.setInt("result", 0);
      proto.addMethod("add", (self, params, result) -> {
        int sum = params.getInt("a") + params.getInt("b");
        self.setInt("result", sum);
        result.setInt("value", sum);
      });
      Dynamic.registerClass(proto);
    }

    try (Client client = Client.standalone()) {
      client.connect();
      try (Proxy proxy = client.instantiate("InstrumentedTestCalculator", null)) {
        try (Dynamic args = new Dynamic()) {
          args.setInt("a", 10);
          args.setInt("b", 32);
          try (Dynamic result = proxy.call("add", args)) {
            assertEquals(42, result.getInt("value"));
          }
        }
        try (Dynamic snapshot = proxy.get()) {
          assertEquals(42, snapshot.getInt("result"));
        }
      }
      client.disconnect();
    }
  }

  @Test
  public void indexedFieldAccessRoundTrips() {
    try (Dynamic obj = new Dynamic()) {
      obj.setIntAt(0, 10);
      obj.setIntAt(1, 20);
      obj.setStringAt(2, "hero");
      assertEquals(3, obj.size());
      assertEquals(10, obj.getIntAt(0));
      assertEquals(20, obj.getIntAt(1));
      assertEquals("hero", obj.getStringAt(2));
    }
  }

  @Test
  public void classInstantiationWalksInheritanceChain() {
    try (Dynamic parentProto = new Dynamic("InstrumentedParent")) {
      parentProto.addFieldInt("baseHp", 100, null);
      Dynamic.registerClass(parentProto, null, null, null);
    }
    try (Dynamic childProto = new Dynamic("InstrumentedChild")) {
      childProto.addFieldInt("childOnly", 7, null);
      Dynamic.registerClass(childProto, "InstrumentedParent", null, null);
    }

    // instantiate() walks the registered prototype chain, so a child
    // instance sees fields declared on its parent without redeclaring them.
    try (Dynamic child = Dynamic.instantiate("InstrumentedChild", null)) {
      assertEquals(100, child.getInt("baseHp"));
      assertEquals(7, child.getInt("childOnly"));
    }

    try (Dynamic found = Dynamic.findClass("InstrumentedChild", null)) {
      assertEquals(Key.of("InstrumentedChild"), found.getKey("__class"));
    }
  }

  @Test
  public void fieldAndClassAttributesRoundTrip() {
    try (Dynamic proto = new Dynamic("InstrumentedAttrsClass")) {
      Attributes fieldMeta =
          new Attributes("HP", "Hit points", "stats", false, null, true);
      proto.addFieldInt("hp", 100, fieldMeta);

      Attributes read = proto.fieldAttributes("hp");
      assertEquals("HP", read.displayName);
      assertEquals("Hit points", read.description);
      assertEquals("stats", read.category);
      assertTrue(read.required);
      assertFalse(read.obsolete);

      Attributes classMeta = new Attributes("Attrs Class", null, null, false, null, false);
      Dynamic.registerClass(proto, null, null, classMeta);
    }

    Attributes classRead = Dynamic.classAttributes("InstrumentedAttrsClass", null);
    assertEquals("Attrs Class", classRead.displayName);
  }

  @Test
  public void yamlRoundTrips() {
    try (Dynamic obj = new Dynamic()) {
      obj.setInt("hp", 99);
      String yaml = obj.toYaml();
      assertTrue(yaml.contains("99"));
      try (Dynamic restored = Dynamic.fromYaml(yaml)) {
        // Same "#<hash>" key limitation as toJson() -- see serializationRoundTrips().
        assertTrue(restored.toYaml().contains("99"));
      }
    }
  }
}
