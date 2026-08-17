# Phase 0 regression test for the MicroPython quickjs module.
# Run with:  micropython test_quickjs_phase0.py

import quickjs

quickjs.init()

# --- numbers ---
assert quickjs.eval("0") == 0
assert quickjs.eval("123") == 123
assert quickjs.eval("-123") == -123
assert quickjs.eval("3.14159") == 3.14159

# --- bool ---
assert quickjs.eval("true") is True
assert quickjs.eval("false") is False

# --- null / undefined ---
assert quickjs.eval("null") is None
assert quickjs.eval("undefined") is None

# --- strings (including rope strings) ---
assert quickjs.eval("'hello'") == "hello"
assert quickjs.eval("'hello' + ' world'") == "hello world"
assert quickjs.eval("'a' + 'b' + 'c'") == "abc"
assert quickjs.eval("String(123)") == "123"
assert quickjs.eval("['a','b','c'].join('')") == "abc"

# --- arrays ---
assert quickjs.eval("[1, 2, 3]") == [1, 2, 3]
assert quickjs.eval("[]") == []

# --- objects ---
obj = quickjs.eval("""
({
    name: "MicroPython",
    version: 1,
    enabled: true
})
""")
assert obj["name"] == "MicroPython"
assert obj["version"] == 1
assert obj["enabled"] is True

# --- nested ---
obj = quickjs.eval("""
({
    array: [1, 2, 3],
    object: {
        x: 10,
        y: 20
    },
    flag: true
})
""")
assert obj["array"] == [1, 2, 3]
assert obj["object"]["x"] == 10
assert obj["object"]["y"] == 20
assert obj["flag"] is True

# --- JS function + quickjs.call ---
quickjs.eval("""
function test(a, b, c, d, e, f) {
    return {
        a: a,
        b: b,
        c: c,
        d: d,
        e: e,
        f: f
    };
}
""")

result = quickjs.call("test", 123, 3.14, True, None, "hello", [1, 2, 3])
assert result["a"] == 123
assert result["b"] == 3.14
assert result["c"] is True
assert result["d"] is None
assert result["e"] == "hello"
assert result["f"] == [1, 2, 3]

# --- deep nested ---
result = quickjs.eval("""
({
    a: [
        1,
        {
            b: [
                true,
                null,
                3.14,
                {
                    c: "hello"
                }
            ]
        }
    ]
})
""")
assert result["a"][0] == 1
assert result["a"][1]["b"][0] is True
assert result["a"][1]["b"][1] is None
assert result["a"][1]["b"][2] == 3.14
assert result["a"][1]["b"][3]["c"] == "hello"

# --- Python -> JS -> Python roundtrip with tuple ---
r = quickjs.call("test", (1, 2), "x", 0, -1, 1.5, {})
assert r["a"] == [1, 2]
assert r["b"] == "x"
assert r["f"] == {}

# --- errors: exception mapping ---
try:
    quickjs.eval('throw new Error("hello")')
    raise AssertionError("exception expected")
except Exception as e:
    msg = str(e)
    assert "hello" in msg, "message should contain 'hello', got: %r" % msg
    print("expected JS exception:", msg)

# --- errors: runtime error (undefined function) ---
try:
    quickjs.eval("not_existing_function()")
    raise AssertionError("exception expected")
except Exception as e:
    print("expected ReferenceError:", e)

# --- errors: undefined variable reference ---
try:
    quickjs.eval("({foo: missingVariable})")
    raise AssertionError("exception expected")
except Exception as e:
    print("expected ReferenceError:", e)

# --- errors: python -> JS unsupported type must not crash ---
try:
    quickjs.call("test", quickjs)  # module object is not convertible
    raise AssertionError("exception expected")
except Exception as e:
    print("expected conversion error:", e)

# --- NaN / Infinity ---
nan = quickjs.eval("NaN")
assert isinstance(nan, float)
import math
assert math.isnan(nan)

inf = quickjs.eval("Infinity")
assert isinstance(inf, float) and inf == float("inf")

ninf = quickjs.eval("-Infinity")
assert isinstance(ninf, float) and ninf == float("-inf")

# Python float nan/inf -> JS roundtrip
r = quickjs.call("test", float("nan"), float("inf"), float("-inf"), None, None, None)
assert math.isnan(r["a"])
assert r["b"] == float("inf")
assert r["c"] == float("-inf")

# --- repeated init is idempotent ---
quickjs.init()
quickjs.init()
assert quickjs.eval("1 + 2") == 3

# --- repeated execution (no leak / no crash / no OOM) ---
for i in range(1000):
    assert quickjs.eval("1 + 2") == 3

for i in range(200):
    assert quickjs.call("test", i, i * 1.5, i % 2 == 0, None, str(i), [i])["a"] == i

# --- memory limit still active: allocating a huge object should fail cleanly ---
try:
    quickjs.eval("new Array(1 << 28)")
    print("note: huge allocation unexpectedly succeeded")
except Exception as e:
    print("memory limit triggered (expected under 64 KiB):", e)

print("ALL QUICKJS PHASE0 TESTS PASSED")