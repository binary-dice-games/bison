"""
examples.py — Python usage examples for the Bison library ctypes binding.

Build ``bison_c`` first, then run this script from the repository root::

    cmake -B build -DPACKAGE_TESTS=ON
    cmake --build build --config Debug --target bison_c
    python bindings/python/examples.py

Each example is a self-contained function; the ``main()`` function at the
bottom runs all of them in sequence.
"""

import os
import sys

# Make sure the bindings/python directory is importable regardless of where the script
# is invoked from.
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

import python.bison as bison


# ─── Helpers ─────────────────────────────────────────────────────────────────

def section(title: str) -> None:
    print(f"\n{'─' * 60}")
    print(f"  {title}")
    print('─' * 60)


# ═════════════════════════════════════════════════════════════════════════════
# Example 1 — Basic field get/set
# ═════════════════════════════════════════════════════════════════════════════

def example_basic_fields() -> None:
    """Demonstrates creating a Dynamic object and reading/writing fields."""
    section("Example 1: basic field get/set")

    # Create an anonymous dynamic object.
    with bison.Dynamic() as obj:
        obj["hp"]    = 100
        obj["speed"] = 9.5
        obj["alive"] = True
        obj["name"]  = "hero"

        print(f"hp    = {obj['hp']}")       # 100
        print(f"speed = {obj['speed']:.2f}")  # 9.50
        print(f"alive = {obj['alive']}")    # True
        print(f"name  = {obj['name']}")     # hero


# ═════════════════════════════════════════════════════════════════════════════
# Example 2 — Numeric (array-like) indices
# ═════════════════════════════════════════════════════════════════════════════

def example_array_access() -> None:
    """Store a sequence of values using integer indices."""
    section("Example 2: array-like indexed fields")

    with bison.Dynamic() as arr:
        arr[0] = "apple"
        arr[1] = "banana"
        arr[2] = "cherry"

        print(f"size = {arr.size()}")    # 3
        for i in range(arr.size()):
            print(f"  [{i}] = {arr[i]}")


# ═════════════════════════════════════════════════════════════════════════════
# Example 3 — Nested objects
# ═════════════════════════════════════════════════════════════════════════════

def example_nested_objects() -> None:
    """Nest one Dynamic inside another as a field value."""
    section("Example 3: nested objects")

    with bison.Dynamic() as player:
        player["name"] = "alice"

        position = bison.Dynamic()
        position["x"] = 1.0
        position["y"] = 2.5

        player["position"] = position
        position.release()

        # Read back the nested object.
        pos = player["position"]           # returns a new Dynamic (refcount +1)
        print(f"pos.x = {pos['x']:.1f}")   # 1.0
        print(f"pos.y = {pos['y']:.1f}")   # 2.5
        pos.release()


# ═════════════════════════════════════════════════════════════════════════════
# Example 4 — JSON import
# ═════════════════════════════════════════════════════════════════════════════

def example_json_import() -> None:
    """Parse a JSON string directly into a Dynamic object."""
    section("Example 4: JSON import")

    json_text = '{"player": {"name": "bob", "score": 250}, "level": 3}'
    with bison.from_json(json_text) as root:
        print(f"level = {root['level']}")

        player = root["player"]
        print(f"player.name  = {player['name']}")
        print(f"player.score = {player['score']}")
        player.release()


# ═════════════════════════════════════════════════════════════════════════════
# Example 5 — YAML import
# ═════════════════════════════════════════════════════════════════════════════

def example_yaml_import() -> None:
    """Parse a YAML string directly into a Dynamic object."""
    section("Example 5: YAML import")

    yaml_text = """\
server:
  host: localhost
  port: 8080
debug: true
max_connections: 100
"""
    with bison.from_yaml(yaml_text) as cfg:
        server = cfg["server"]
        print(f"host = {server['host']}")
        print(f"port = {server['port']}")
        server.release()
        print(f"debug           = {cfg['debug']}")
        print(f"max_connections = {cfg['max_connections']}")


# ═════════════════════════════════════════════════════════════════════════════
# Example 7 — Method registration and invocation
# ═════════════════════════════════════════════════════════════════════════════

def example_methods() -> None:
    """Register a Python function as a named method and call it."""
    section("Example 7: method registration and invocation")

    # Method that doubles the 'counter' field on self and returns the new value.
    def double_counter(self: bison.Dynamic, params: bison.Dynamic) -> bison.Dynamic:
        current = self["counter"]
        self["counter"] = current * 2
        # The C binding inspects indexed fields of the returned Dynamic.
        # We return an empty object here; named results must be set on result_h
        # inside add_method's c_callback (see bison.py).
        return bison.Dynamic()

    with bison.Dynamic() as obj:
        obj["counter"] = 5
        obj.add_method("double", double_counter)

        result = obj.call("double", bison.Dynamic())
        result.release()

        # After the call, self["counter"] should be 10.
        print(f"counter after double = {obj['counter']}")   # 10


# ═════════════════════════════════════════════════════════════════════════════
# Example 8 — bison_key utility
# ═════════════════════════════════════════════════════════════════════════════

def example_key_utility() -> None:
    """Show that bison.key() produces the same hash as the C++ '_key' literal."""
    section("Example 8: bison_key utility")

    # The high bit is always set for named keys.
    k = bison.key("velocity")
    print(f"key('velocity') = 0x{k:08x}")
    print(f"high bit set    = {bool(k & 0x80000000)}")

    k2 = bison.key("velocity")
    print(f"deterministic   = {k == k2}")


# ═════════════════════════════════════════════════════════════════════════════
# Example 9 — Class hierarchy and add_ref
# ═════════════════════════════════════════════════════════════════════════════

def example_class_registry() -> None:
    """Register a class prototype and create an instance that inherits its fields."""
    section("Example 9: class registry and add_ref")

    # Register a 'Vehicle' prototype with 4 wheels.
    proto = bison.Dynamic("Vehicle")
    proto["wheels"] = 4
    try:
        bison.add_class("", proto)
    except bison.BisonError:
        pass   # already registered from a previous run
    proto.release()

    # Create an instance.
    car = bison.Dynamic("Vehicle")
    print(f"wheels (inherited) = {car['wheels']}")   # 4

    # Demonstrate add_ref: both car and car2 point to the same object.
    car2 = car.add_ref()
    car["wheels"] = 3
    print(f"car['wheels']  = {car['wheels']}")    # 3
    print(f"car2['wheels'] = {car2['wheels']}")   # 3 — shared!

    car.release()
    car2.release()


# ─── Entry point ─────────────────────────────────────────────────────────────

def main() -> None:
    print("Bison Python binding — usage examples")
    example_basic_fields()
    example_array_access()
    example_nested_objects()
    example_json_import()
    example_yaml_import()
    example_methods()
    example_key_utility()
    example_class_registry()
    print("\nAll examples completed successfully.")


if __name__ == "__main__":
    main()
