# Phase 3 tests: JS Function <-> MicroPython callable + execution timeout.
# Run with: micropython tests/test_quickjs_phase3.py

import quickjs
import gc
import time

ctx = quickjs.Context()

# =============================================================
# A. JS Function -> callable
# =============================================================
ctx.eval("""
function add(a, b) { return a + b; }
""")
f = ctx.get("add")
assert callable(f)
assert f(1, 2) == 3
assert f(10, 20) == 30
print("A. JS function wrapper OK")

# =============================================================
# B. multiple argument kinds
# =============================================================
ctx.eval("""
function test(a, b, c, d, e, fl) {
    return {a: a, b: b, c: c, d: d, e: e, f: fl};
}
""")
t = ctx.get("test")
r = t(123, 3.14, True, None, "hello", [1, 2, 3])
assert r == {"a": 123, "b": 3.14, "c": True, "d": None, "e": "hello", "f": [1, 2, 3]}, r
print("B. multiple args OK")

# =============================================================
# C. nested arguments
# =============================================================
r = t({"items": [10, {"value": 20}], "config": {"debug": True, "mode": "test"}},
      1, 2, 3, 4, 5)
assert r["a"]["items"][1]["value"] == 20
assert r["a"]["config"]["debug"] is True
assert r["a"]["config"]["mode"] == "test"
print("C. nested args OK")

# =============================================================
# D. JS function returns complex objects
# =============================================================
ctx.eval("""
function mk() {
    return {
        d: {x: 1, y: [2, 3]},
        l: [1, [2, {z: 3}]],
        buf: new Uint8Array([9, 8, 7]),
        n: null,
        b: true,
        fl: 2.5,
        big: 9223372036854775807n,
        arr: [4, 5]
    };
}
""")
m = ctx.get("mk")
out = m()
assert out["d"] == {"x": 1, "y": [2, 3]}
assert out["l"] == [1, [2, {"z": 3}]]
assert out["buf"] == b"\x09\x08\x07"
assert out["n"] is None
assert out["b"] is True
assert out["fl"] == 2.5
assert out["big"] == 9223372036854775807
assert out["arr"] == [4, 5]
print("D. complex return OK")

# =============================================================
# E. Python callable -> JS
# =============================================================
def add2(a, b):
    return a + b
ctx.add_callable("add2", add2)
assert ctx.eval("add2(1, 2)") == 3
assert ctx.eval("add2(10, 20)") == 30
assert ctx.call("add2", 5, 6) == 11
assert ctx.eval("(function(){ return add2(7, 8); })()") == 15
print("E. python callable -> JS OK")

# =============================================================
# F. callback survives GC of the Python name
# =============================================================
def survive():
    return 999
ctx.add_callable("survive", survive)
del survive
gc.collect()
assert ctx.eval("survive()") == 999
print("F. callback GC OK")

# =============================================================
# G. callback returns complex object
# =============================================================
def test2(x):
    return {"x": x, "array": [1, 2, 3], "enabled": True}
ctx.add_callable("test2", test2)
assert ctx.eval("JSON.stringify(test2(10))") == '{"x":10,"array":[1,2,3],"enabled":true}'
print("G. callback complex return OK")

# =============================================================
# H. callback exception
# =============================================================
def fail():
    raise ValueError("callback failure")
ctx.add_callable("fail", fail)
try:
    ctx.eval("fail()")
    raise AssertionError("expected exception")
except Exception as e:
    assert "callback failure" in str(e), str(e)
    print("H. callback exception OK:", str(e).splitlines()[0])

# JS can catch the callback error
def maybe_fail(x):
    if x < 0:
        raise RuntimeError("negative not allowed")
    return x * 2
ctx.add_callable("maybe_fail", maybe_fail)
r = ctx.eval('''
try { maybe_fail(-1); "no-catch"; }
catch (e) { "caught:" + e.message; }
''')
assert "negative not allowed" in r, r
print("H2. JS catches callback error OK:", r)

# =============================================================
# I. callback called many times
# =============================================================
counter = {"n": 0}
def inc():
    counter["n"] += 1
    return counter["n"]
ctx.add_callable("inc", inc)
ctx.eval("for (let i = 0; i < 1000; i++) { inc(); }")
assert counter["n"] == 1000
print("I. callback 1000x OK")

# =============================================================
# J. callback replacement (last registration wins)
# =============================================================
ctx.add_callable("rfoo", lambda: 1)
assert ctx.eval("rfoo()") == 1
ctx.add_callable("rfoo", lambda: 2)
assert ctx.eval("rfoo()") == 2
print("J. callback replace OK")

# =============================================================
# K. function wrapper after context close
# =============================================================
ctx.eval("function early(){ return 1; }")
g = ctx.get("early")
ctx.close()
try:
    g()
    raise AssertionError("expected context closed")
except RuntimeError as e:
    assert "context closed" in str(e), str(e)
del g
gc.collect()
print("K. wrapper after close OK")

# =============================================================
# L. wrapper keeps Context alive across Python GC
# =============================================================
ctxL = quickjs.Context()
ctxL.eval("function keep(){ return 42; }")
w = ctxL.get("keep")
del ctxL
gc.collect()
assert w() == 42
# close explicitly -> now it must raise
ctxL = None  # can't, already deleted; use wrapper's context via new ctx
print("L. wrapper keeps context alive OK")

# =============================================================
# M. timeout on eval
# =============================================================
ctxT = quickjs.Context()
ctxT.set_time_limit(100)
t0 = time.ticks_ms()
try:
    ctxT.eval("while (true) {}")
    raise AssertionError("expected timeout")
except RuntimeError as e:
    dt = time.ticks_diff(time.ticks_ms(), t0)
    assert "timeout" in str(e).lower(), str(e)
    assert dt < 2000, "took too long: %d ms" % dt
print("M. eval timeout OK (%d ms)" % dt)

# =============================================================
# N. context usable after timeout
# =============================================================
ctxT.set_time_limit(100)
try:
    ctxT.eval("while (true) {}")
except RuntimeError:
    pass
ctxT.set_time_limit(0)
assert ctxT.eval("1 + 2") == 3
print("N. context reusable after timeout OK")

# =============================================================
# O. timeout on call
# =============================================================
ctxT.eval("function loop() { while (true) {} }")
ctxT.set_time_limit(100)
try:
    ctxT.call("loop")
    raise AssertionError("expected timeout")
except RuntimeError as e:
    assert "timeout" in str(e).lower(), str(e)
print("O. call timeout OK")

# timeout via function wrapper
ctxT.eval("function also_loop(){ while (true) {} }")
loop_w = ctxT.get("also_loop")
ctxT.set_time_limit(100)
try:
    loop_w()
    raise AssertionError("expected timeout")
except RuntimeError as e:
    assert "timeout" in str(e).lower(), str(e)
print("O2. wrapper call timeout OK")

# =============================================================
# P. timeout disabled
# =============================================================
ctxT.set_time_limit(0)
assert ctxT.eval("1 + 1") == 2
assert ctxT.eval("var s=0; for (let i=0;i<2000;i++) s+=i; s;") == 1999000
print("P. timeout disabled OK")

# =============================================================
# limits / validation
# =============================================================
try:
    ctxT.set_time_limit(-1)
    raise AssertionError("expected ValueError")
except ValueError:
    pass
ctxT.close()
try:
    ctxT.set_time_limit(100)
    raise AssertionError("expected context closed")
except RuntimeError:
    pass
print("timeout validation OK")

# =============================================================
# keyword arguments rejected on wrapper
# =============================================================
ctxE = quickjs.Context()
ctxE.eval("function k(a, b){ return a+b; }")
k = ctxE.get("k")
try:
    k(1, 2, x=3)
    raise AssertionError("expected TypeError")
except TypeError as e:
    assert "keyword" in str(e).lower(), str(e)
print("kwarg rejection OK")

# =============================================================
# this is undefined when calling through wrapper (JS_UNDEFINED this, strict fn)
# =============================================================
ctxE.eval("function readThis(){ 'use strict'; return this; }")  # strict: this stays undefined
rt = ctxE.get("readThis")
assert rt() is None
print("this is undefined OK")

ctxE.close()
print("ALL QUICKJS PHASE3 TESTS PASSED")