# MicroPython QuickJS - Tooling, Memory Stats & BJSON Tests
#
# Tests memory_stats() detailed breakdown and bjson binary serialization in JS and Python.

import quickjs


def expect(cond, msg="Assertion failed"):
    if not cond:
        raise AssertionError(msg)


print("=== Running test_tooling.py ===")

# ---------------------------------------------------------------------------
# Section 1: memory_stats() Breakdown
# ---------------------------------------------------------------------------
ctx = quickjs.Context()

ctx.eval("const arr = [1, 2, 3, 'hello'];")
ctx.eval("function compute(a, b) { return a * b; }")

stats = ctx.memory_stats()
expect(isinstance(stats, dict))
expect(stats["memory_used_size"] > 0)
expect(stats["malloc_size"] > 0)
expect(stats["atoms_count"] > 0)
expect(stats["objects_count"] > 0)
expect(stats["js_func_count"] > 0)

# Singleton memory_stats
mod_stats = quickjs.memory_stats()
expect(isinstance(mod_stats, dict))
expect(mod_stats["memory_used_size"] > 0)

print("memory_stats() detailed breakdown OK")

# ---------------------------------------------------------------------------
# Section 2: bjson in JavaScript
# ---------------------------------------------------------------------------
# bjson.write & bjson.read
res = ctx.eval("""
const data = { name: "sensor", value: 123.45, active: true, list: [1, 2, 3] };
const encoded = bjson.write(data);
const decoded = bjson.read(encoded);
decoded;
""")
expect(res == {"name": "sensor", "value": 123.45, "active": True, "list": [1, 2, 3]})

# bjson constants
expect(ctx.eval("bjson.READ_OBJ_BYTECODE") == 1)
expect(ctx.eval("bjson.WRITE_OBJ_BYTECODE") == 1)

print("JS bjson.write / bjson.read OK")

# ---------------------------------------------------------------------------
# Section 3: bjson in Python (ctx.bjson_encode / decode & quickjs.bjson_*)
# ---------------------------------------------------------------------------
py_data = {"id": 1001, "tags": ["embedded", "quickjs"], "status": True}
bin_data = ctx.bjson_encode(py_data)
expect(isinstance(bin_data, bytes))
expect(len(bin_data) > 0)

restored = ctx.bjson_decode(bin_data)
expect(restored == py_data)

# Singleton bjson
s_bin = quickjs.bjson_encode([10, 20, 30])
s_res = quickjs.bjson_decode(s_bin)
expect(s_res == [10, 20, 30])

ctx.close()
print("Python bjson_encode / bjson_decode OK")

print("ALL TOOLING & BJSON TESTS PASSED")
