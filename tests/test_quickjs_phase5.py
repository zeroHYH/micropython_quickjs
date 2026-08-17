# Phase 5 tests: reentrancy, promise bridging, this binding,
# TypedArray raw bytes, BigInt marker API, GC/lifetime audit.
#
# Run: micropython tests/test_quickjs_phase5.py
#
# Coverage map (spec A-U):
#   A. reentrant close protection during JS execution
#   B. nested callback (JS->cb->JS->cb) close rejection
#   C. callback exception -> depth restored, context usable
#   D. timeout -> depth restored, context usable
#   E. p.then() resolve
#   F. p.then() rejection propagation
#   G. p.catch()
#   H. p.finally_()
#   I. promise chain
#   J. callback returns Promise -> assimilation
#   K. callback returns Function -> pass-through
#   L. callback raises -> derived promise rejected
#   M. this binding via wrapper.call(this, *args)
#   N. cross-context this rejection
#   O. TypedArray -> raw bytes (all 8 required kinds)
#   P. quickjs.bigint() explicit API
#   Q. GC: many promises/callbacks, MP gc + JS gc
#   R. close: normal, double close, close-after-busy-rejection
#   S. finalizer: __del__-driven close on GC
#   T. stress: mixed chains then/catch/finally + close
#   U. ASAN (run under the ASAN build; see HANDOVER)
# plus: error model - a rejected promise must not poison the context
#       (ctx.eval keeps working after result() raises).

print("phase5 start")

import quickjs
import gc


def expect(cond, msg):
    if not cond:
        raise AssertionError(msg)


# =============================================================
# A. reentrant close protection during JS execution
# =============================================================
ctx = quickjs.Context()
ctx.set_time_limit(2000)


def cb_close():
    ctx.close()


ctx.add_callable("cbtest", cb_close)
try:
    ctx.eval("cbtest()")
    raise AssertionError("A: ctx.close() inside callback should be rejected")
except Exception as e:
    expect("context is busy" in str(e), str(e))
# context still fully usable
expect(ctx.eval("1+2") == 3, ctx.eval("1+2"))
print("A. reentrant close protection OK")

# =============================================================
# B. nested callback close rejection
# =============================================================


def inner_cb():
    ctx.close()


def outer_cb(x):
    return x


ctx.add_callable("inner_cb_b", inner_cb)
ctx.add_callable("outer_cb_b", outer_cb)
ctx.eval("function mid_b() { return inner_cb_b(); }")
ctx.add_callable("mid_wrap_b", lambda: ctx.eval("mid_b()"))
try:
    ctx.eval("outer_cb_b(mid_wrap_b())")
    raise AssertionError("B: nested close should be rejected")
except Exception as e:
    expect("context is busy" in str(e), str(e))
expect(ctx.eval("3*3") == 9, ctx.eval("3*3"))
print("B. nested callback OK")

# =============================================================
# C. callback exception -> depth restored
# =============================================================


def cb_boom():
    raise ValueError("boom-py")


ctx.add_callable("boomb", cb_boom)
try:
    ctx.eval("boomb()")
    raise AssertionError("C: should raise")
except Exception as e:
    expect("boom-py" in str(e), str(e))
expect(ctx.eval("1+2") == 3, "C: depth not restored after callback exception")
print("C. callback exception depth restore OK")

# =============================================================
# D. timeout -> depth restored
# =============================================================
ctx.set_time_limit(30)
try:
    ctx.eval("while(true){}")
    raise AssertionError("D: should timeout")
except Exception as e:
    expect("timeout" in str(e), str(e))
ctx.set_time_limit(0)
expect(ctx.eval("1+2") == 3, "D: depth not restored after timeout")
print("D. timeout depth restore OK")

# =============================================================
# E. p.then() resolve
# =============================================================
p = ctx.eval("Promise.resolve(10)")
q = p.then(lambda x: x * 2)
expect(not q.done(), "E: should be pending before run_jobs")
ctx.run_jobs()
expect(q.done(), "E: should settle after run_jobs")
expect(q.result() == 20, q.result())
print("E. then resolve OK")

# =============================================================
# F. p.then() rejection propagation
# =============================================================
pr = ctx.eval('Promise.reject(new Error("boom"))')
qr = pr.then(lambda x: x)
ctx.run_jobs()
try:
    qr.result()
    raise AssertionError("F: rejection should propagate")
except Exception as e:
    expect("boom" in str(e), str(e))
print("F. then reject OK")

# =============================================================
# G. p.catch()
# =============================================================
pc = ctx.eval('Promise.reject("oops")')
qc = pc.catch(lambda e: "handled:" + e)
ctx.run_jobs()
expect(qc.result() == "handled:oops", qc.result())
# catch with no handler: rejection passes through
q4 = ctx.eval('Promise.reject("keep")').catch()
ctx.run_jobs()
try:
    q4.result()
    raise AssertionError("G: passthrough catch should reject")
except Exception as e:
    expect("keep" in str(e), str(e))
print("G. catch OK")

# =============================================================
# H. p.finally_()
# =============================================================
fin = []
ctx.set("fin_h", fin)
ph = ctx.eval("Promise.resolve(5)")
qh = ph.finally_(lambda: fin.append(1))
ctx.run_jobs()
expect(len(fin) == 1, fin)
expect(qh.result() == 5, qh.result())
# finally on rejection runs callback and propagates rejection
phr = ctx.eval('Promise.reject("z")')
qhr = phr.finally_(lambda: fin.append(2))
ctx.run_jobs()
expect(len(fin) == 2, fin)
try:
    qhr.result()
    raise AssertionError("H: finally should propagate rejection")
except Exception as e:
    expect("z" in str(e), str(e))
# finally_ with a JS function wrapper
fj = ctx.eval("(function(){ return 1; })")
qhj = ctx.eval("Promise.resolve(7)").finally_(fj)
ctx.run_jobs()
expect(qhj.result() == 7, qhj.result())
print("H. finally_ OK")

# =============================================================
# I. promise chain
# =============================================================
base = ctx.eval("Promise.resolve(2)")
c1 = base.then(lambda x: x * 3)
c2 = c1.then(lambda x: x + 1)
c3 = c2.catch(lambda e: -1)
ctx.run_jobs()
expect(c1.result() == 6, c1.result())
expect(c2.result() == 7, c2.result())
expect(c3.result() == 7, c3.result())
print("I. chain OK")

# =============================================================
# J. callback returns Promise -> assimilation
# =============================================================
inner = ctx.eval("Promise.resolve(5)")
pj = ctx.eval("Promise.resolve(1)")
qj = pj.then(lambda x: inner)
ctx.run_jobs()
expect(qj.result() == 5, qj.result())
print("J. assimilation OK")

# =============================================================
# K. callback returns Function -> pass-through
# =============================================================
fnk = ctx.eval("(function(){ return 99; })")
qk = ctx.eval("Promise.resolve(0)").then(lambda x: fnk)
ctx.run_jobs()
vk = qk.result()
expect(callable(vk), vk)
expect(vk() == 99, vk())
print("K. callback return Function OK")

# =============================================================
# L. callback exception -> derived promise rejected
# =============================================================


def boom_l(x):
    raise ValueError("cb boom L")


ql = ctx.eval("Promise.resolve(1)").then(boom_l)
ctx.run_jobs()
try:
    ql.result()
    raise AssertionError("L: should reject")
except Exception as e:
    expect("cb boom L" in str(e), str(e))
print("L. callback exception -> rejected OK")

# =============================================================
# M. this binding
# =============================================================
ctx.eval("globalThis.obj_m = { value: 42, getValue: function(x) { return this.value + x; } };")
obj_m = ctx.get("obj_m")
w_m = ctx.eval("obj_m.getValue")
expect(w_m.call(obj_m, 8) == 50, w_m.call(obj_m, 8))
# default call keeps this undefined (JS semantics: sloppy -> global,
# so this.value is undefined and the sum is NaN, not 42)
r_default = w_m(8)
expect(r_default != 50, r_default)
# strict function proves this === undefined on default call
ctx.eval("globalThis.sf_m = function(x) { 'use strict'; return this === undefined; };")
sf_m = ctx.eval("sf_m")
expect(sf_m(0) is True, sf_m(0))
print("M. this binding OK")

# =============================================================
# N. cross-context this rejection
# =============================================================
ctx_n1 = quickjs.Context()
ctx_n2 = quickjs.Context()
w_n1 = ctx_n1.eval("(function(){ return this; })")
fn_n2 = ctx_n2.eval("(function(){ return 1; })")
try:
    w_n1.call(fn_n2, 0)
    raise AssertionError("N: cross-context this should be rejected")
except Exception as e:
    expect("another context" in str(e), str(e))
ctx_n1.close()
ctx_n2.close()
print("N. cross-context this OK")

# =============================================================
# O. TypedArray -> raw bytes (all required kinds)
# =============================================================
# little-endian host assumption (unix port default)
LE = True
expect(bytes(ctx.eval("new Int8Array([-1, 2, -3])")) == b"\xff\x02\xfd",
       bytes(ctx.eval("new Int8Array([-1, 2, -3])")))
expect(bytes(ctx.eval("new Uint8ClampedArray([1, 2, 300])")) == b"\x01\x02\xff",
       bytes(ctx.eval("new Uint8ClampedArray([1, 2, 300])")))
expect(bytes(ctx.eval("new Int16Array([1, -2])")) == b"\x01\x00\xfe\xff",
       bytes(ctx.eval("new Int16Array([1, -2])")))
expect(bytes(ctx.eval("new Uint16Array([0x0102, 0x0304])")) == b"\x02\x01\x04\x03",
       bytes(ctx.eval("new Uint16Array([0x0102, 0x0304])")))
expect(bytes(ctx.eval("new Int32Array([0x01020304])")) == b"\x04\x03\x02\x01",
       bytes(ctx.eval("new Int32Array([0x01020304])")))
expect(bytes(ctx.eval("new Uint32Array([0x01020304])")) == b"\x04\x03\x02\x01",
       bytes(ctx.eval("new Uint32Array([0x01020304])")))
expect(bytes(ctx.eval("new Float32Array([1.0])")) == b"\x00\x00\x80\x3f",
       bytes(ctx.eval("new Float32Array([1.0])")))
expect(bytes(ctx.eval("new Float64Array([1.0])")) == b"\x00\x00\x00\x00\x00\x00\xf0\x3f",
       bytes(ctx.eval("new Float64Array([1.0])")))
# Uint8Array unchanged
expect(bytes(ctx.eval("new Uint8Array([1,2,3])")) == b"\x01\x02\x03",
       bytes(ctx.eval("new Uint8Array([1,2,3])")))
# subarray view slicing
expect(bytes(ctx.eval("new Uint8Array([1,2,3,4,5]).subarray(1,3)")) == b"\x02\x03",
       bytes(ctx.eval("new Uint8Array([1,2,3,4,5]).subarray(1,3)")))
# detached buffer -> error, context still usable
ctx.eval("globalThis.buf_o = new ArrayBuffer(8); globalThis.ta_o = new Uint8Array(buf_o); buf_o.transfer();")
try:
    ctx.get("ta_o")
    raise AssertionError("O: detached typed array should raise")
except Exception as e:
    expect("detached" in str(e), str(e))
expect(ctx.eval("1+1") == 2, "O: context poisoned")
print("O. TypedArray raw bytes OK")

# =============================================================
# P. quickjs.bigint() explicit API
# =============================================================
b = quickjs.bigint(12345678901234567890)
ctx.set("x_p", b)
expect(ctx.eval("typeof x_p") == "bigint", ctx.eval("typeof x_p"))
expect(ctx.eval("x_p.toString()") == "12345678901234567890",
       ctx.eval("x_p.toString()"))
# negative
ctx.set("y_p", quickjs.bigint(-98765432109876543210987))
expect(ctx.eval("y_p.toString()") == "-98765432109876543210987",
       ctx.eval("y_p.toString()"))
# zero
ctx.set("z_p", quickjs.bigint(0))
expect(ctx.eval("z_p.toString()") == "0", ctx.eval("z_p.toString()"))
# small positive within int64
ctx.set("s_p", quickjs.bigint(42))
expect(ctx.eval("s_p.toString()") == "42", ctx.eval("s_p.toString()"))
# arbitrary precision
huge = 12345678901234567890123456789012345678901234567890
ctx.set("h_p", quickjs.bigint(huge))
expect(ctx.eval("h_p.toString()") == str(huge), ctx.eval("h_p.toString()"))
expect(ctx.eval("(h_p + 1n).toString()") == str(huge + 1),
       ctx.eval("(h_p + 1n).toString()"))
# arithmetic with normal numbers/BigInt
ctx.set("n_p", quickjs.bigint(100))
expect(ctx.eval("(n_p * 3n).toString()") == "300", ctx.eval("(n_p * 3n).toString()"))
# round trip within int64 via ctx.get
ctx.set("rt_p", quickjs.bigint(999999999999999999))
expect(ctx.get("rt_p") == 999999999999999999, ctx.get("rt_p"))
# normal Python int -> JS number unchanged
ctx.set("ni_p", 123)
expect(ctx.eval("typeof ni_p") == "number", ctx.eval("typeof ni_p"))
# invalid argument
try:
    quickjs.bigint("x")
    raise AssertionError("P: bigint('x') should raise ValueError")
except ValueError as e:
    expect("integer" in str(e), str(e))
print("P. bigint API OK")

# =============================================================
# error model: rejected promise does not poison the context
# =============================================================
rej = ctx.eval('Promise.reject(Error("poison"))')
ctx.run_jobs()
try:
    rej.result()
    raise AssertionError("error model: should raise")
except Exception:
    pass
expect(ctx.eval("1+2") == 3, "error model: eval poisoned after rejection")
print("P9x. error model (no context pollution) OK")

# =============================================================
# Q. GC: many promises/callbacks, MP gc + JS gc
# =============================================================
for i in range(500):
    qq = ctx.eval("Promise.resolve(%d)" % i)
    qq = qq.then(lambda x: x + 1)
    if i % 100 == 0:
        gc.collect()
ctx.run_jobs()
gc.collect()
ctx.gc()
expect(ctx.eval("1+2") == 3, "Q: context broken after GC")
print("Q. GC OK")

# =============================================================
# R. close semantics
# =============================================================
ctx.close()
ctx.close()  # idempotent
try:
    ctx.eval("1")
    raise AssertionError("R: eval after close should raise")
except Exception as e:
    expect("closed" in str(e), str(e))
# new context works
ctx_r = quickjs.Context()
ctx_r.eval("1+2")
ctx_r.close()
print("R. close OK")

# =============================================================
# S. finalizer (__del__-driven close)
# =============================================================
ctx_s = quickjs.Context()
ctx_s.eval("var arr = [1,2,3]; globalThis.g = 42;")
del ctx_s
gc.collect()
ctx_s2 = quickjs.Context()
expect(ctx_s2.eval("6*7") == 42, ctx_s2.eval("6*7"))
ctx_s2.close()
gc.collect()
print("S. finalizer OK")

# =============================================================
# T. stress: mixed chains then/catch/finally + close
# =============================================================
ctx_t = quickjs.Context()
for i in range(300):
    pt = ctx_t.eval("Promise.resolve(%d)" % i)
    qt = pt.then(lambda x: x + 1).catch(lambda e: -1)
    qt = qt.finally_(lambda: None)
ctx_t.run_jobs()
expect(ctx_t.eval("1+2") == 3, "T: context broken")
ctx_t.close()
print("T. stress OK")

# =============================================================
# U. ASAN
# =============================================================
# Run under the ASAN build:
#   make ... CFLAGS_EXTRA="-fsanitize=address --param asan-use-after-return=0 -DMP_ASAN=1"
#   rm -rf build-asan  (on flag change)
# See HANDOVER.md phase 5 notes.
print("U. ASAN (run separately under ASAN build)")

print("phase5 end")
print("ALL QUICKJS PHASE5 TESTS PASSED")
