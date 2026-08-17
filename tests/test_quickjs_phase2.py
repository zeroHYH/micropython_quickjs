# Phase 2 tests: conversion safety (depth/cycles), binary types, BigInt,
# error info enhancement. Run with: micropython tests/test_quickjs_phase2.py

import quickjs
import gc

ctx = quickjs.Context()

# =============================================================
# 1. recursion depth limit
# =============================================================
# JS side: nested arrays 40 deep (> QUICKJS_MAX_CONVERSION_DEPTH 32)
try:
    ctx.eval("(function d(n){ if(n==0) return 0; return [d(n-1)]; })(40)")
    raise AssertionError("expected depth error")
except Exception as e:
    assert "maximum conversion depth exceeded" in str(e), str(e)
    print("JS depth limit OK:", str(e)[:50])

# Python side: nested lists 40 deep
deep = []
cur = deep
for _ in range(40):
    nxt = []
    cur.append(nxt)
    cur = nxt
try:
    ctx.set("deep", deep)
    raise AssertionError("expected depth error")
except Exception as e:
    assert "maximum conversion depth exceeded" in str(e), str(e)
    print("Py depth limit OK:", str(e)[:50])

# deep but within limit must work
ok = []
cur = ok
for _ in range(20):
    nxt = []
    cur.append(nxt)
    cur = nxt
ctx.set("ok", ok)
r = ctx.get("ok")
assert len(r) == 1
assert isinstance(r[0], list)
print("depth 20 OK")

# =============================================================
# 2. Python list circular reference
# =============================================================
a = []
a.append(a)
try:
    ctx.set("circ", a)
    raise AssertionError("expected circular")
except Exception as e:
    assert "circular reference" in str(e), str(e)
print("Py list cycle OK")

# =============================================================
# 3. Python dict circular reference
# =============================================================
d = {}
d["self"] = d
try:
    ctx.set("circd", d)
    raise AssertionError("expected circular")
except Exception as e:
    assert "circular reference" in str(e), str(e)
print("Py dict cycle OK")

# =============================================================
# 4. Python multi-object cycle (two dicts)
# =============================================================
a = {}
b = {}
a["b"] = b
b["a"] = a
try:
    ctx.set("circ2", {"x": a})
    raise AssertionError("expected circular")
except Exception as e:
    assert "circular reference" in str(e), str(e)
print("Py two-dict cycle OK")

# also via call arg
ctx.eval("function takes(o){ return o; }")
try:
    ctx.call("takes", a)
    raise AssertionError("expected circular")
except Exception as e:
    assert "circular reference" in str(e), str(e)
print("Py cycle via call OK")

# =============================================================
# 5. JS Array circular reference
# =============================================================
try:
    ctx.eval("var jc = []; jc.push(jc); jc;")
    raise AssertionError("expected circular")
except Exception as e:
    assert "circular reference" in str(e), str(e)
print("JS array cycle OK")

# =============================================================
# 6. JS Object circular reference
# =============================================================
try:
    ctx.eval("var jo = {}; jo.self = jo; jo;")
    raise AssertionError("expected circular")
except Exception as e:
    assert "circular reference" in str(e), str(e)
print("JS object cycle OK")

# =============================================================
# 7. JS two-object cycle
# =============================================================
try:
    ctx.eval("var A={}; var B={}; A.b=B; B.a=A; A;")
    raise AssertionError("expected circular")
except Exception as e:
    assert "circular reference" in str(e), str(e)
print("JS two-object cycle OK")

# =============================================================
# 8. shared reference but NOT circular
# =============================================================
x = [1, 2]
ctx.set("shared", [x, x])
assert ctx.eval("shared[0][0]") == 1
assert ctx.eval("shared[1][1]") == 2
print("shared non-circular OK")

# JS shared (same object twice in an array)
ctx.eval("var s1 = {v: 5}; var arr = [s1, s1];")
assert ctx.eval("arr[0].v") == 5
assert ctx.eval("arr[1].v") == 5
print("JS shared non-circular OK")

# =============================================================
# 9. bytes -> ArrayBuffer
# =============================================================
data = b"\x00\x01\x02\x03"
ctx.set("data", data)
assert ctx.eval("data.byteLength") == 4
assert ctx.eval("new Uint8Array(data)[0]") == 0
assert ctx.eval("Array.from(new Uint8Array(data))") == [0, 1, 2, 3]
print("bytes -> ArrayBuffer OK")

# =============================================================
# 10. ArrayBuffer -> bytes
# =============================================================
r = ctx.eval("new Uint8Array([0, 1, 2, 3]).buffer")
assert r == b"\x00\x01\x02\x03", r
r = ctx.eval("new ArrayBuffer(3)")
assert r == b"\x00\x00\x00"
print("ArrayBuffer -> bytes OK")

# =============================================================
# 11. bytearray -> Uint8Array
# =============================================================
ba = bytearray([1, 2, 3])
ctx.set("bdata", ba)
assert ctx.eval("bdata.length") == 3
assert ctx.eval("bdata[0]") == 1
assert ctx.eval("bdata[2]") == 3
print("bytearray -> Uint8Array OK")

# =============================================================
# 12. Uint8Array -> bytes
# =============================================================
r = ctx.eval("new Uint8Array([10, 20, 30])")
assert r == b"\x0a\x14\x1e", r
print("Uint8Array -> bytes OK")

# =============================================================
# 13. unsupported TypedArray
# =============================================================
for expr in (
    "new Uint16Array([1,2,3])",
    "new Int8Array([1,2,3])",
    "new Float32Array([1.5])",
    "new BigInt64Array([1n])",
):
    try:
        ctx.eval(expr)
        raise AssertionError("expected TypeError for %s" % expr)
    except TypeError as e:
        assert "unsupported typed array" in str(e), str(e)
print("unsupported TypedArray OK")

# =============================================================
# 14. BigInt
# =============================================================
assert ctx.eval("0n") == 0
assert ctx.eval("123n") == 123
assert ctx.eval("-123n") == -123
assert ctx.eval("9223372036854775807n") == 9223372036854775807
assert ctx.eval("-9223372036854775808n") == -9223372036854775808
print("BigInt OK")

# =============================================================
# 15. BigInt int64 upper/lower boundary
# =============================================================
assert ctx.eval("123n + 456n") == 579
assert ctx.eval("123n * 2n") == 246
assert ctx.eval("-9223372036854775808n + 1n") == -9223372036854775807
print("BigInt arithmetic OK")

# =============================================================
# 16. BigInt overflow (must not silently truncate / convert to float)
# =============================================================
for expr in (
    "9223372036854775808n",
    "-9223372036854775809n",
    "999999999999999999999999999999999n",
    "-999999999999999999999999999999999n",
):
    try:
        ctx.eval(expr)
        raise AssertionError("expected overflow for %s" % expr)
    except TypeError as e:
        assert "BigInt out of range" in str(e), str(e)
print("BigInt overflow OK")

# =============================================================
# 17. BigInt mixed with number -> JS TypeError mapped cleanly
# =============================================================
try:
    ctx.eval("123n + 1")
    raise AssertionError("expected TypeError")
except Exception as e:
    assert "TypeError" in str(e), str(e)
    print("BigInt+number error OK:", str(e).splitlines()[0])

# =============================================================
# 18. Symbol unsupported (no crash)
# =============================================================
try:
    ctx.eval("Symbol('x')")
    raise AssertionError("expected TypeError")
except TypeError as e:
    assert "unsupported" in str(e), str(e)
print("Symbol unsupported OK")

# =============================================================
# 19. Function unsupported in conversion (phase 3 will add wrapper)
# =============================================================
ctx.eval("function someFunction(){ return 1; }")
try:
    ctx.get("someFunction")
    raise AssertionError("expected TypeError")
except TypeError as e:
    assert "unsupported" in str(e), str(e)
try:
    ctx.eval("(function(){ return 1; })")
    raise AssertionError("expected TypeError")
except TypeError as e:
    assert "unsupported" in str(e), str(e)
print("Function unsupported OK")

# =============================================================
# 20. JS Error name/message
# =============================================================
try:
    ctx.eval('throw new Error("hello")')
    raise AssertionError("expected")
except Exception as e:
    msg = str(e)
    assert "Error" in msg.splitlines()[0], msg
    assert "hello" in msg, msg
    print("Error name/message OK:", msg.splitlines()[0])

try:
    ctx.eval('throw new TypeError("bad type")')
    raise AssertionError("expected")
except Exception as e:
    assert "TypeError" in str(e).splitlines()[0]
    assert "bad type" in str(e)
    print("TypeError name/message OK:", str(e).splitlines()[0])

# thrown non-Error value
try:
    ctx.eval('throw "plain string"')
    raise AssertionError("expected")
except Exception as e:
    assert "plain string" in str(e), str(e)
    print("thrown string OK:", str(e).splitlines()[0])

try:
    ctx.eval('throw 42')
    raise AssertionError("expected")
except Exception as e:
    assert "42" in str(e), str(e)
    print("thrown number OK:", str(e).splitlines()[0])

# =============================================================
# 21. JS Error stack (if available)
# =============================================================
try:
    ctx.eval('function inner(){ throw new Error("withstack"); } inner();')
    raise AssertionError("expected")
except Exception as e:
    msg = str(e)
    assert "withstack" in msg
    assert "stack" in msg, "stack should be included"
    print("Error stack included; head:", msg.splitlines()[0])
    print("  stack:", msg.split("stack:")[1].strip().splitlines()[0])

# =============================================================
# 22. GC + binary copy semantics
# =============================================================
tmp = b"\x11\x22\x33\x44"
ctx.set("gdata", tmp)
del tmp
gc.collect()
assert ctx.eval("gdata.byteLength") == 4
assert ctx.eval("gdata instanceof ArrayBuffer")
r = ctx.eval("new Uint8Array([5,6,7]).buffer")
gc.collect()
assert r == b"\x05\x06\x07"
print("GC binary copy semantics OK")

# =============================================================
# 23. default singleton regression with new conversions
# =============================================================
quickjs.init()
assert quickjs.eval("123n") == 123
assert quickjs.eval("1 + 2") == 3
assert quickjs.eval("new Uint8Array([1,2,3])") == b"\x01\x02\x03"
assert quickjs.eval("new Uint8Array([10,20,30]).buffer") == b"\x0a\x14\x1e"
try:
    quickjs.eval("var qc = []; qc.push(qc); qc;")
    raise AssertionError("expected circular")
except Exception as e:
    assert "circular reference" in str(e)
print("default singleton new-conversions OK")

# =============================================================
# 24. stress tests
# =============================================================
for _ in range(1000):
    ctx.eval("({a:1,b:[1,2,3]})")
for _ in range(1000):
    ctx.eval("new Uint8Array([1,2,3])")
for _ in range(1000):
    ctx.eval("123n")
for _ in range(1000):
    try:
        ctx.eval('throw new Error("x")')
    except Exception:
        pass
for _ in range(100):
    try:
        ctx.eval("var s=[]; s.push(s); s;")
    except Exception:
        pass
    try:
        ctx.eval("var o={}; o.self=o; o;")
    except Exception:
        pass
    try:
        c = []; c.append(c); ctx.set("cy", c)
    except Exception:
        pass
print("stress loops OK")

ctx.close()
print("ALL QUICKJS PHASE2 TESTS PASSED")