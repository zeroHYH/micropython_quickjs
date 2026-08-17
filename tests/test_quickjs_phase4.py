# Phase 4 tests: Promise / job queue / Function wrapper pass-through / lifecycle.
# Run with: micropython tests/test_quickjs_phase4.py
#
# Covers (per spec section 17):
#   A run_jobs basic, B Promise.resolve, C Promise.reject, D then, E catch,
#   F finally, G multiple promises, H promise chain, I promise exception,
#   J job queue multiple runs, K no pending job, L function pass-through,
#   M callback returns JS Function, N cross-context rejected, O func wrapper GC,
#   P promise wrapper GC, Q close-after promise, R close-after function,
#   S 1000 promises/jobs, T 1000 pass-through, U GC stress, V exception stress,
#   W memory limit, X timeout + promise/job.

import quickjs
import gc
import time

print("phase4 start")

# =============================================================
# K. no pending job (do first: fresh context)
# =============================================================
ctx = quickjs.Context()
assert ctx.has_pending_jobs() == False
assert ctx.run_jobs() == 0
print("K. no pending job OK")

# =============================================================
# A. run_jobs basic
# =============================================================
ctx.eval("Promise.resolve(10).then(x => globalThis.a = x)")
assert ctx.has_pending_jobs() == True
n = ctx.run_jobs()
assert n == 1, n
assert ctx.has_pending_jobs() == False
assert ctx.get("a") == 10
print("A. run_jobs basic OK")

# =============================================================
# B. Promise.resolve
# =============================================================
p = ctx.eval("Promise.resolve(42)")
assert p.done() == True
assert p.result() == 42
assert ctx.eval("Promise.resolve('hello')").result() == "hello"
assert ctx.eval("Promise.resolve([1, 2, 3])").result() == [1, 2, 3]
assert ctx.eval("Promise.resolve({k: 1})").result() == {"k": 1}
print("B. Promise.resolve OK")

# =============================================================
# C. Promise.reject
# =============================================================
p = ctx.eval("Promise.reject(new Error('boom'))")
assert p.done() == True
try:
    p.result()
    raise AssertionError("expected rejection")
except RuntimeError as e:
    assert "boom" in str(e), str(e)
# non-Error rejection reason
p = ctx.eval("Promise.reject('oops')")
try:
    p.result()
    raise AssertionError("expected rejection")
except RuntimeError as e:
    assert "oops" in str(e), str(e)
print("C. Promise.reject OK")

# =============================================================
# D. then
# =============================================================
p = ctx.eval("Promise.resolve(1).then(x => x + 1)")
assert p.done() == False
assert ctx.run_jobs() == 1
assert p.done() == True
assert p.result() == 2
print("D. then OK")

# =============================================================
# E. catch
# =============================================================
ctx.eval("""
globalThis.catchRes = 'unset';
Promise.reject(new Error('x'))
    .catch(e => globalThis.catchRes = 'caught:' + e.message);
""")
ctx.run_jobs()
assert ctx.get("catchRes") == "caught:x"
# catch via wrapper
p = ctx.eval("Promise.reject(new Error('y')).catch(e => 'handled:' + e.message)")
assert p.done() == False
ctx.run_jobs()
assert p.done() == True
assert p.result() == "handled:y"
print("E. catch OK")

# =============================================================
# F. finally
# =============================================================
ctx.eval("""
globalThis.finRun = 0;
globalThis.finVal = -1;
Promise.resolve(1)
    .finally(() => { globalThis.finRun += 1; return 99; })
    .then(v => globalThis.finVal = v);
""")
ctx.run_jobs()
assert ctx.get("finRun") == 1
assert ctx.get("finVal") == 1, ctx.get("finVal")  # finally return ignored for fulfillment
# finally on rejection still propagates rejection
ctx.eval("""
globalThis.finRun2 = 0;
globalThis.finCaught = 'unset';
Promise.reject(new Error('z'))
    .finally(() => { globalThis.finRun2 += 1; })
    .catch(e => globalThis.finCaught = e.message);
""")
ctx.run_jobs()
assert ctx.get("finRun2") == 1
assert ctx.get("finCaught") == "z"
print("F. finally OK")

# =============================================================
# G. multiple promises
# =============================================================
ctx.eval("""
globalThis.m1 = null; globalThis.m2 = null; globalThis.m3 = null;
Promise.resolve(1).then(x => globalThis.m1 = x);
Promise.resolve(2).then(x => globalThis.m2 = x);
Promise.resolve(3).then(x => globalThis.m3 = x);
""")
n = ctx.run_jobs()
assert n == 3, n
assert ctx.get("m1") == 1
assert ctx.get("m2") == 2
assert ctx.get("m3") == 3
print("G. multiple promises OK")

# =============================================================
# H. promise chain
# =============================================================
p = ctx.eval("""
Promise.resolve(1)
    .then(x => x * 2)
    .then(x => x + 3)
    .then(x => x * 4)
""")
n = ctx.run_jobs()
assert n == 3, n
assert p.result() == 20, p.result()
print("H. promise chain OK")

# =============================================================
# special test: chained side effect via globalThis
# =============================================================
ctx2 = quickjs.Context()
ctx2.eval("""
    Promise.resolve(1)
        .then(x => x + 1)
        .then(x => globalThis.result = x)
""")
ctx2.run_jobs()
assert ctx2.get("result") == 2
print("special chain-to-global OK")

# =============================================================
# I. promise exception (rejection from .then)
# =============================================================
p = ctx.eval("Promise.resolve(1).then(() => { throw new Error('chain fail'); })")
ctx.run_jobs()
try:
    p.result()
    raise AssertionError("expected rejection")
except RuntimeError as e:
    assert "chain fail" in str(e), str(e)
print("I. promise exception OK")

# =============================================================
# J. job queue multiple runs (drain semantics + repeated use)
# =============================================================
ctx.eval("globalThis.jobRun = 0;")
ctx.eval("""
function step() { globalThis.jobRun += 1; }
Promise.resolve().then(step).then(step).then(step);
""")
# run_jobs() drains the whole queue in one call (js_std_loop pattern)
n = ctx.run_jobs()
assert n == 3, n
assert ctx.get("jobRun") == 3
assert ctx.has_pending_jobs() == False
# repeated use of run_jobs across batches
ctx.eval("Promise.resolve().then(step);")
assert ctx.run_jobs() == 1
assert ctx.get("jobRun") == 4
assert ctx.has_pending_jobs() == False
assert ctx.run_jobs() == 0
print("J. job queue multiple runs OK")

# =============================================================
# L. Function wrapper pass-through
# =============================================================
ctx.eval("function pt(x){ return x + 1; }")
f = ctx.get("pt")
ctx.set("pt2", f)
assert ctx.eval("pt2(41)") == 42
assert ctx.call("pt2", 10) == 11
# getting it back is still a wrapper
w = ctx.get("pt2")
assert callable(w)
assert w(100) == 101
print("L. function pass-through OK")

# =============================================================
# M. callback returns JS Function
# =============================================================
ctx.eval("function mkfn(a, b){ return a * b; }")
mkfn = ctx.get("mkfn")

def retfn():
    return mkfn

ctx.add_callable("retfn", retfn)
assert ctx.eval("retfn()(3, 4)") == 12
assert ctx.call("retfn")(5, 6) == 30

# nested: callback returns list containing the wrapper
def pack():
    return [mkfn, 1]

ctx.add_callable("pack", pack)
assert ctx.eval("pack()[0](7, 8)") == 56

# dict containing wrapper
def dct():
    return {"fn": mkfn}

ctx.add_callable("dct", dct)
assert ctx.eval("dct()['fn'](9, 10)") == 90
print("M. callback returns JS Function OK")

# =============================================================
# N. cross-context rejection
# =============================================================
ctxA = quickjs.Context()
ctxA.eval("function fa(){ return 'A'; }")
fA = ctxA.get("fa")
try:
    ctx.set("leak", fA)
    raise AssertionError("expected cross-context error")
except RuntimeError as e:
    assert "another context" in str(e), str(e)
# target context must NOT have the leaked property
assert ctx.get("leak") is None

# via callback return value
def badpick():
    return fA

ctx.add_callable("badpick", badpick)
try:
    ctx.eval("badpick()")
    raise AssertionError("expected cross-context error via callback")
except RuntimeError as e:
    assert "another context" in str(e), str(e)
ctxA.close()
print("N. cross-context rejection OK")

# =============================================================
# O. Function wrapper GC
# =============================================================
ctxo = quickjs.Context()
ctxo.eval("function g(){ return 1; }")
for i in range(200):
    w = ctxo.get("g")
    del w
    if i % 50 == 0:
        gc.collect()
gc.collect()
assert ctxo.eval("g()") == 1
# wrapper keeps Context alive across Python GC
ctxo.eval("function keep(){ return 42; }")
w = ctxo.get("keep")
del ctxo
gc.collect()
assert w() == 42
del w
gc.collect()
print("O. function wrapper GC OK")

# =============================================================
# P. Promise wrapper GC
# =============================================================
ctxp = quickjs.Context()
for i in range(200):
    pr = ctxp.eval("Promise.resolve(%d)" % i)
    assert pr.result() == i
    del pr
    if i % 50 == 0:
        gc.collect()
gc.collect()
# promise wrapper keeps Context alive across Python GC
pr = ctxp.eval("Promise.resolve(123)")
del ctxp
gc.collect()
assert pr.result() == 123
del pr
gc.collect()
print("P. promise wrapper GC OK")

# =============================================================
# Q. Context close after Promise
# =============================================================
ctxq = quickjs.Context()
pq = ctxq.eval("Promise.resolve(1)")
ctxq.close()
for meth in (pq.done, pq.result):
    try:
        meth()
        raise AssertionError("expected context closed")
    except RuntimeError as e:
        assert "context closed" in str(e), str(e)
del pq
gc.collect()
print("Q. close-after-promise OK")

# =============================================================
# R. Context close after Function
# =============================================================
ctxr = quickjs.Context()
ctxr.eval("function rf(){ return 1; }")
fr = ctxr.get("rf")
ctxr.close()
try:
    fr()
    raise AssertionError("expected context closed")
except RuntimeError as e:
    assert "context closed" in str(e), str(e)
del fr
gc.collect()
print("R. close-after-function OK")

# =============================================================
# S. 1000 promises / jobs
# =============================================================
ctxs = quickjs.Context()
for i in range(1000):
    ctxs.eval("Promise.resolve(%d).then(x => globalThis.last = x)" % i)
    n = ctxs.run_jobs()
    assert n == 1, n
    assert ctxs.get("last") == i
print("S. 1000 promises/jobs OK")

# =============================================================
# T. 1000 function pass-through
# =============================================================
ctxt = quickjs.Context()
ctxt.eval("function tfn(x){ return x + 1; }")
tf = ctxt.get("tfn")
for i in range(1000):
    ctxt.set("tmp", tf)
    assert ctxt.eval("tmp(%d)" % i) == i + 1
print("T. 1000 pass-through OK")

# =============================================================
# U. GC stress: create/drop contexts with promise + callback
# =============================================================
for i in range(100):
    cu = quickjs.Context()
    cu.add_callable("cb", lambda: 1)
    pu = cu.eval("Promise.resolve(1).then(x => cb())")
    cu.run_jobs()
    assert pu.result() == 1
    del pu
    del cu
    if i % 20 == 0:
        gc.collect()
gc.collect()
print("U. GC stress OK")

# =============================================================
# V. exception stress: 100 rejections + 100 job-thrown errors
# =============================================================
ctxv = quickjs.Context()
for i in range(100):
    p = ctxv.eval("Promise.reject(new Error('err%d'))" % i)
    try:
        p.result()
        raise AssertionError("expected rejection")
    except RuntimeError as e:
        assert "err%d" % i in str(e), str(e)
    del p
for i in range(100):
    p = ctxv.eval("Promise.resolve().then(() => { throw new Error('t%d') })" % i)
    ctxv.run_jobs()
    try:
        p.result()
        raise AssertionError("expected rejection")
    except RuntimeError as e:
        assert "t%d" % i in str(e), str(e)
    del p
print("V. exception stress OK")

# =============================================================
# W. memory limit
# =============================================================
ctxw = quickjs.Context()
ctxw.set_memory_limit(200000)
try:
    # a typed array allocates real backing in the JS heap -> trips the limit
    ctxw.eval("var big = new Uint8Array(1000000); big;")
    raised = False
except RuntimeError as e:
    raised = True
    assert "out of memory" in str(e).lower() or "memory" in str(e).lower(), str(e)
assert raised, "expected JS heap allocation failure under small limit"
ctxw.set_memory_limit(0)
assert ctxw.eval("1 + 1") == 2
print("W. memory limit OK")

# =============================================================
# X. timeout + promise/job
# =============================================================
ctxx = quickjs.Context()

# (1) plain eval timeout (regression from phase 3)
ctxx.set_time_limit(100)
t0 = time.ticks_ms()
try:
    ctxx.eval("while (true) {}")
    raise AssertionError("expected timeout")
except RuntimeError as e:
    assert "timeout" in str(e).lower(), str(e)
    dt = time.ticks_diff(time.ticks_ms(), t0)
    assert dt < 2000, "took too long: %d ms" % dt

# (2) timeout triggered inside a queued job (promise reaction)
ctxx.set_time_limit(0)
ctxx.eval("globalThis.spin = function(){ while(true){} };")
ctxx.set_time_limit(100)
try:
    ctxx.eval("Promise.resolve().then(() => spin())")
    ctxx.run_jobs()
    raise AssertionError("expected timeout in job")
except RuntimeError as e:
    assert "timeout" in str(e).lower(), str(e)

# (3) executor throw is absorbed by the Promise constructor (JS spec):
#     the promise becomes rejected, no exception escapes the eval
ctxx.set_time_limit(100)
p = ctxx.eval("new Promise(function(res){ while(true){} })")
ctxx.set_time_limit(0)
assert p.done() == True  # rejected with the internal "interrupted" error

# context usable after timeouts
assert ctxx.eval("1 + 2") == 3
print("X. timeout + promise/job OK")

# =============================================================
# singleton run_jobs / has_pending_jobs
# =============================================================
quickjs.init()
assert quickjs.has_pending_jobs() == False
assert quickjs.run_jobs() == 0
# note: singleton has no Context opaque -> eval() of a Promise value raises
# TypeError ("unsupported QuickJS value type"); end the script with
# "undefined" so the return value is not a promise (jobs still enqueue)
quickjs.eval("Promise.resolve(7).then(x => globalThis.sres = x); undefined;")
assert quickjs.has_pending_jobs() == True
assert quickjs.run_jobs() == 1
assert quickjs.eval("sres") == 7
print("singleton jobs OK")

print("ALL QUICKJS PHASE4 TESTS PASSED")
