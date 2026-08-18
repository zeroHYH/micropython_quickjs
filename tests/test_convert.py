# MicroPython QuickJS - Type Conversion & Safety Tests
#
# Tests bidirectional JS <-> MicroPython type conversions:
# - Primitives, containers, and binary types (ArrayBuffer, TypedArray raw bytes)
# - Arbitrary precision BigInt
# - Conversion safety (depth limit, circular reference detection)
# - Exception formatting and stack trace propagation

import quickjs


def expect(cond, msg="Assertion failed"):
    if not cond:
        raise AssertionError(msg)


def expect_raise(fn, exc_type, msg_contains, label=""):
    try:
        fn()
    except exc_type as e:
        expect(msg_contains in str(e), f"{label}: {e!r} lacks {msg_contains!r}")
        return e
    except Exception as e:
        raise AssertionError(f"{label}: expected {exc_type.__name__}, got {e!r}") from e
    raise AssertionError(
        f"{label}: expected exception {exc_type.__name__} containing {msg_contains!r}"
    )


print("=== Running test_convert.py ===")

ctx = quickjs.Context()

# =============================================================
# 1. Primitives & Basic Containers
# =============================================================
# Null / undefined -> None
assert ctx.eval("null") is None
assert ctx.eval("undefined") is None

# Booleans
assert ctx.eval("true") is True
assert ctx.eval("false") is False

# Numbers
assert ctx.eval("42") == 42
assert type(ctx.eval("42")) is int
assert ctx.eval("3.14159") == 3.14159
assert type(ctx.eval("3.14159")) is float

# Strings
assert ctx.eval("'hello world'") == "hello world"
assert ctx.eval("''") == ""
assert (
    ctx.eval("'unicode \u4e2d\u6587 \U0001f600'") == "unicode \u4e2d\u6587 \U0001f600"
)

# Arrays <-> Lists / Tuples
assert ctx.eval("[1, 'two', true, null, [3, 4]]") == [
    1,
    "two",
    True,
    None,
    [3, 4],
]
ctx.set("py_list", [10, "abc", [True, False]])
assert ctx.eval("py_list.length") == 3
assert ctx.eval("py_list[2][0]") is True

ctx.set("py_tuple", (1, 2, 3))
assert ctx.eval("Array.isArray(py_tuple)") is True
assert ctx.eval("py_tuple.length") == 3

# Objects <-> Dicts
d = ctx.eval("({name: 'Alice', age: 30, active: true, scores: [100, 95]})")
assert d == {"name": "Alice", "age": 30, "active": True, "scores": [100, 95]}

ctx.set("py_dict", {"k1": "v1", "k2": 123, "nested": {"a": 1}})
assert ctx.eval("py_dict.k1") == "v1"
assert ctx.eval("py_dict.nested.a") == 1

# Dict key must be string
expect_raise(
    lambda: ctx.set("bad_dict", {123: "val"}),
    RuntimeError,
    "TypeError: JS object keys must be str",
    "int dict key",
)

print("Primitives & Containers OK")

# =============================================================
# 2. Binary Data (ArrayBuffer, TypedArray raw bytes)
# =============================================================
# bytes -> ArrayBuffer -> bytes
data = b"\x00\x01\x02\xfe\xff"
ctx.set("raw_bytes", data)
assert ctx.eval("raw_bytes instanceof ArrayBuffer") is True
assert ctx.eval("new Uint8Array(raw_bytes)[3]") == 0xFE
assert ctx.get("raw_bytes") == data

# bytearray -> Uint8Array -> bytes
ba = bytearray(b"\x10\x20\x30")
ctx.set("ba", ba)
assert ctx.eval("ba instanceof Uint8Array") is True
assert ctx.get("ba") == b"\x10\x20\x30"

# TypedArray raw byte representations (Uint16Array, Int8Array, Float32Array, BigInt64Array)
for expr, expected_len in [
    ("new Uint16Array([1, 2, 3])", 6),
    ("new Int8Array([1, 2, 3])", 3),
    ("new Float32Array([1.5])", 4),
    ("new BigInt64Array([1n])", 8),
]:
    r = ctx.eval(expr)
    assert isinstance(r, bytes)
    assert len(r) == expected_len, f"{expr} len was {len(r)}, expected {expected_len}"

# Uint8Array subarray slices
r = ctx.eval("new Uint8Array([10, 20, 30, 40, 50]).subarray(1, 4)")
assert r == b"\x14\x1e\x28"

print("Binary data conversions OK")

# =============================================================
# 3. BigInt Support
# =============================================================
# Python int64 range BigInt
ctx.set("bi_small", quickjs.bigint(42))
assert ctx.eval("typeof bi_small") == "bigint"
assert ctx.eval("bi_small + 8n") == 50
assert ctx.eval("bi_small == 42n") is True

# Negative BigInt
ctx.set("bi_neg", quickjs.bigint(-999999999999))
assert ctx.eval("bi_neg.toString()") == "-999999999999"

# Arbitrary precision BigInt beyond int64
huge = 1234567890123456789012345678901234567890
ctx.set("bi_huge", quickjs.bigint(huge))
assert ctx.eval("bi_huge.toString()") == str(huge)
assert ctx.eval("(bi_huge + 10n).toString()") == str(huge + 10)

# BigInt arithmetic in JS (within int64)
assert ctx.eval("123n + 456n") == 579
assert ctx.eval("123n * 2n") == 246
assert ctx.eval("9223372036854775807n") == 9223372036854775807
assert ctx.eval("-9223372036854775808n + 1n") == -9223372036854775807

# Large BigInt values beyond int64 can be read via toString()
assert ctx.eval("(10n ** 20n).toString()") == "100000000000000000000"

# BigInt out of range when read back into int
expect_raise(
    lambda: ctx.eval("1n << 100n"),
    TypeError,
    "BigInt out of range",
    "huge bigint to MP",
)

# Type checking on quickjs.bigint()
expect_raise(
    lambda: quickjs.bigint("123"),
    ValueError,
    "bigint() requires an integer",
    "str to bigint",
)
expect_raise(
    lambda: quickjs.bigint(3.14),
    ValueError,
    "bigint() requires an integer",
    "float to bigint",
)

print("BigInt support OK")

# =============================================================
# 4. Conversion Safety (Recursion depth limit & Cycle detection)
# =============================================================
# JS object depth limit
deep_js = "var o = 0; for(var i=0; i<40; i++) o = {next: o}; o;"
expect_raise(
    lambda: ctx.eval(deep_js),
    RuntimeError,
    "maximum conversion depth exceeded",
    "deep JS object",
)

# Python object depth limit
deep_py = 0
for _ in range(40):
    deep_py = [deep_py]
expect_raise(
    lambda: ctx.set("deep_py", deep_py),
    RuntimeError,
    "maximum conversion depth exceeded",
    "deep Python list",
)

# Python list self-cycle
cycle_list = [1, 2]
cycle_list.append(cycle_list)
expect_raise(
    lambda: ctx.set("cycle_list", cycle_list),
    RuntimeError,
    "circular reference detected",
    "py list cycle",
)

# Python dict self-cycle
cycle_dict = {"a": 1}
cycle_dict["self"] = cycle_dict
expect_raise(
    lambda: ctx.set("cycle_dict", cycle_dict),
    RuntimeError,
    "circular reference detected",
    "py dict cycle",
)

# Python mutual two-dict cycle
d1, d2 = {}, {}
d1["ref"] = d2
d2["ref"] = d1
expect_raise(
    lambda: ctx.set("d1", d1),
    RuntimeError,
    "circular reference detected",
    "two-dict cycle",
)

# JS array self-cycle
ctx.eval("var a = [1, 2]; a.push(a); null;")
expect_raise(
    lambda: ctx.get("a"), RuntimeError, "circular reference detected", "js array cycle"
)

# JS object self-cycle
ctx.eval("var obj = {k: 1}; obj.self = obj; null;")
expect_raise(
    lambda: ctx.get("obj"),
    RuntimeError,
    "circular reference detected",
    "js object cycle",
)

# Shared non-circular structures must NOT be misdetected as cycles
shared_leaf = {"data": [1, 2, 3]}
shared_root = {"left": shared_leaf, "right": shared_leaf}
ctx.set("shared", shared_root)
assert ctx.eval("shared.left.data[0] + shared.right.data[0]") == 2

print("Conversion safety (depth & cycles) OK")

# =============================================================
# 5. Error Formatting & Stack Trace Propagation
# =============================================================
# Standard Error object
try:
    ctx.eval("throw new Error('something went wrong');")
    raise AssertionError("should have thrown")
except RuntimeError as e:
    err_str = str(e)
    assert "Error: something went wrong" in err_str
    assert "stack:" in err_str

# Specific Error types
try:
    ctx.eval("throw new TypeError('invalid type argument');")
    raise AssertionError("should have thrown")
except RuntimeError as e:
    assert "TypeError: invalid type argument" in str(e)

try:
    ctx.eval("throw new RangeError('out of bounds');")
    raise AssertionError("should have thrown")
except RuntimeError as e:
    assert "RangeError: out of bounds" in str(e)

# Thrown primitive values
try:
    ctx.eval("throw 'custom string error';")
    raise AssertionError("should have thrown")
except RuntimeError as e:
    assert "custom string error" in str(e)

try:
    ctx.eval("throw 404;")
    raise AssertionError("should have thrown")
except RuntimeError as e:
    assert "404" in str(e)


# Unsupported types raise TypeError
class CustomObj:
    pass


expect_raise(
    lambda: ctx.set("custom", CustomObj()),
    RuntimeError,
    "TypeError: unsupported MicroPython type",
    "custom class",
)

print("Error formatting & stack traces OK")

# =============================================================
# 6. Special Built-in Objects (Symbol, Date, RegExp, Map, Set)
# =============================================================
# Symbol
assert ctx.eval("Symbol('myKey')") == "Symbol(myKey)"
assert ctx.eval("Symbol()") == "Symbol()"

# Date -> ISO 8601 string
date_str = ctx.eval("new Date('2026-08-18T12:00:00Z')")
assert date_str == "2026-08-18T12:00:00.000Z", f"Got {date_str}"

# RegExp -> string pattern representation
re_str = ctx.eval("/^[a-z0-9_]+$/gi")
assert re_str == "/^[a-z0-9_]+$/gi", f"Got {re_str}"

# Map -> Python dict
map_res = ctx.eval("new Map([['name', 'quickjs'], ['ver', 2026]])")
assert isinstance(map_res, dict)
assert map_res == {"name": "quickjs", "ver": 2026}

# Nested Map
nested_map = ctx.eval("new Map([['inner', new Map([['a', 10], ['b', 20]])]])")
assert nested_map == {"inner": {"a": 10, "b": 20}}

# Set -> Python set
set_res = ctx.eval("new Set([1, 2, 3, 2, 1])")
assert isinstance(set_res, set)
assert set_res == {1, 2, 3}

ctx.close()
print("Special built-in objects (Symbol, Date, RegExp, Map, Set) OK")

print("ALL TYPE CONVERSION TESTS PASSED")
