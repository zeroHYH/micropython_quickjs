# Phase 6 tests: execution safety and reentrancy hardening.
#
# Run: micropython tests/test_quickjs_phase6.py
#
# Coverage map (spec A-O, plus regression P/Q):
#   A. nested reentrancy (JS->cb->JS->cb) depth balance
#   B. close while executing / double close idempotent
#   C. callback -> eval -> callback
#   D. callback -> close rejection (incl. wrapper- and run_jobs-driven)
#   E. timeout -> context reuse
#   F. timeout -> run_jobs
#   G. timeout -> close
#   H. Promise -> callback -> exception (rejected promise; ctx unpoisoned)
#   I. Promise -> callback -> close (busy rejected; chain still settles)
#   J. pending exception contamination (each error followed by eval 1+2 == 3)
#   K. 1000 exceptions in a row, then eval still works
#   L. 1000 timeout/reuse cycles
#   M. 1000 callback/reentrancy cycles
#   N. 100 create/drop/close cycles
#   O. stress: mixed operations
#   P. regression: nested eval must NOT cancel the outer timeout budget
#   Q. regression: MP->JS conversion failure leaks nothing (assert/leak-free,
#      error surfaces as RuntimeError embedding the Python error, ctx reusable)
#
# U. ASAN: run under the ASAN DEBUG build (asserts on, detection on).

print("phase6 start")

import quickjs
import gc


def expect(cond, msg):
    if not cond:
        raise AssertionError(msg)


# =============================================================
# A. nested reentrancy: JS -> cb -> JS -> cb, depth balanced
# =============================================================
ctxA = quickjs.Context()
ctxA.set_time_limit(5000)


def cbA_inner(x):
    return x * 2


def cbA_mid():
    # Python callback re-enters JS: outer depth +1, inner window +1 more
    return ctxA.call("innerA", 21)


ctxA.add_callable("innerA", cbA_inner)
ctxA.add_callable("midA", cbA_mid)
ctxA.eval("function outerA() { return midA(); }")
expect(ctxA.call("outerA") == 42, "A: nested call result")
# depth must be fully restored: close() is only allowed at depth 0
ctxA.close()
ctxA.close()  # idempotent
print("A. nested reentrancy OK")

# =============================================================
# B. close while executing / double close
# =============================================================
ctxB = quickjs.Context()
ctxB.set_time_limit(5000)
closed_b = []


def cbB_close():
    try:
        ctxB.close()
        closed_b.append("NOT-REJECTED")
    except RuntimeError as e:
        closed_b.append("busy" in str(e))
    return 123


ctxB.add_callable("cbB", cbB_close)
r = ctxB.eval("cbB()")
expect(r == 123, "B: callback still returns")
expect(closed_b == [True], "B: close during execution must raise busy")
# context fully usable after the busy rejection
expect(ctxB.eval("1+2") == 3, "B: usable after busy")
ctxB.eval("function maxB(a, b){ return Math.max(a, b); }")
expect(ctxB.call("maxB", 3, 9) == 9, "B: call works after busy")
ctxB.close()
try:
    ctxB.eval("1")
    raise AssertionError("B: eval after close must raise")
except RuntimeError as e:
    expect("context closed" in str(e), str(e))
ctxB.close()  # double close no-op
print("B. close while executing OK")

# =============================================================
# C. callback -> eval -> callback
# =============================================================
ctxC = quickjs.Context()
ctxC.set_time_limit(5000)
order_c = []


def cbC_leaf(v):
    order_c.append("leaf")
    return v + 1


def cbC_mid(v):
    order_c.append("mid")
    # re-enter JS from a callback; that eval itself calls a callback
    s = ctxC.eval("function leafC(x){ return leafCb(x); } leafC(%d)" % v)
    return s * 10


ctxC.add_callable("leafCb", cbC_leaf)
ctxC.add_callable("midCb", cbC_mid)
r = ctxC.eval("midCb(5)")
expect(r == 60, "C: %r" % r)
expect(order_c == ["mid", "leaf"], "C: order %r" % (order_c,))
expect(ctxC.eval("11*11") == 121, "C: ctx usable")
ctxC.close()
print("C. callback -> eval -> callback OK")

# =============================================================
# D. callback -> close (three nesting shapes), no UAF/double-free
# =============================================================
# D1. Python callback -> eval -> callback -> close
ctxD = quickjs.Context()
ctxD.set_time_limit(5000)


def cbD_deep():
    return ctxD.eval("1 + 1")


def cbD_closer(*a):
    try:
        ctxD.close()
        return "NOT-REJECTED"
    except RuntimeError as e:
        return "busy" if "busy" in str(e) else "other:" + str(e)


ctxD.add_callable("deepD", cbD_deep)
ctxD.add_callable("closerD", cbD_closer)
ctxD.eval("function wrapD(){ return deepD(); }")
ctxD.add_callable("wrapDcb", lambda: ctxD.eval("wrapD()"))
r = ctxD.eval("closerD(wrapDcb())")
expect(r == "busy", "D1: %r" % (r,))
expect(ctxD.eval("5+5") == 10, "D1: usable after nested close attempt")

# D2. JS function wrapper -> Python callback -> close
ctxD.add_callable("pyD", cbD_closer)
fD = ctxD.eval("(function(){ return pyD(); })")
expect(fD() == "busy", "D2: wrapper->callback->close: %r" % (fD(),))
expect(ctxD.eval("2+2") == 4, "D2: usable")

# D3. run_jobs() -> callback -> close
ctxD.eval("var pD = new Promise(function(res, rej){ res(1); }); pD")
ctxD.add_callable("jobCloserD", cbD_closer)
ctxD.eval("var pD2 = Promise.resolve(1).then(function(){ return jobCloserD(); });")
n = ctxD.run_jobs()
expect(n >= 1, "D3: jobs ran: %d" % n)
expect(ctxD.eval("3+3") == 6, "D3: usable after close attempt inside job")

ctxD.close()
print("D. callback -> close OK")

# =============================================================
# E. timeout -> reuse
# =============================================================
ctxE = quickjs.Context()
ctxE.eval("function slowE(x){ while(true){} }")
ctxE.eval("function maxE(a, b){ return Math.max(a, b); }")
ctxE.set_time_limit(30)
try:
    ctxE.eval("slowE()")
    raise AssertionError("E: expected timeout")
except RuntimeError as e:
    expect("timeout" in str(e), str(e))
expect(ctxE.eval("1+2") == 3, "E: eval after timeout")
expect(ctxE.call("maxE", 7, 2) == 7, "E: call after timeout")
ctxE.set_time_limit(0)
expect(ctxE.eval("3*7") == 21, "E: usable with timeout off")
ctxE.close()
print("E. timeout -> reuse OK")

# =============================================================
# F. timeout -> run_jobs
# =============================================================
ctxF = quickjs.Context()
ctxF.set_time_limit(30)
try:
    ctxF.eval("while(true){}")
    raise AssertionError("F: expected timeout")
except RuntimeError as e:
    expect("timeout" in str(e), str(e))
# queue a promise job and pump it
ctxF.set_time_limit(0)
ctxF.eval("var pF = new Promise(function(res){ res(41); });")
ctxF.eval("pF.then(function(x){ gF = x + 1; });")
ctxF.set_time_limit(400)
n = ctxF.run_jobs()
expect(n >= 1, "F: run_jobs after timeout")
expect(ctxF.eval("gF") == 42, "F: job result")
ctxF.set_time_limit(0)
ctxF.close()
print("F. timeout -> run_jobs OK")

# =============================================================
# G. timeout -> close
# =============================================================
ctxG = quickjs.Context()
ctxG.set_time_limit(30)
try:
    ctxG.eval("while(true){}")
    raise AssertionError("G: expected timeout")
except RuntimeError as e:
    expect("timeout" in str(e), str(e))
ctxG.close()  # depth must be restored by the timeout path
print("G. timeout -> close OK")

# =============================================================
# H. Promise -> callback -> exception (rejection as state)
# =============================================================
ctxH = quickjs.Context()


def cbH_boom(v):
    raise ValueError("H boom v=%s" % (v,))


ctxH.add_callable("boomH", cbH_boom)
ctxH.eval("var pH = new Promise(function(res, rej){ res(7); });")
p = ctxH.eval("pH.then(function(x){ return boomH(x); }).catch(function(e){ return 'caught:' + e.message; });")
ctxH.run_jobs()
expect(p.result() == "caught:ValueError: H boom v=7", "H: %r" % (p.result(),))
expect(ctxH.eval("1+2") == 3, "H: ctx unpoisoned")
ctxH.close()
print("H. Promise -> callback -> exception OK")

# =============================================================
# I. Promise -> callback -> close (busy inside job; chain settles)
# =============================================================
ctxI = quickjs.Context()


def cbI_close(v):
    try:
        ctxI.close()
        return "NOT-REJECTED"
    except RuntimeError as e:
        return "busy" if "busy" in str(e) else "other:" + str(e)


ctxI.add_callable("closeI", cbI_close)
ctxI.eval("var pI = new Promise(function(res, rej){ res(1); });")
p = ctxI.eval("pI.then(function(x){ return closeI(x); });")
ctxI.run_jobs()
expect(p.result() == "busy", "I: %r" % (p.result(),))
expect(ctxI.eval("9*9") == 81, "I: usable after busy inside job")
ctxI.close()
print("I. Promise -> callback -> close OK")

# =============================================================
# J. pending exception contamination
# =============================================================
ctxJ = quickjs.Context()


def check_clean(tag):
    expect(ctxJ.eval("1+2") == 3, "J: %s contaminated" % tag)


def cbJ_err():
    raise ValueError("J py error")


ctxJ.add_callable("errJ", cbJ_err)

# 1. plain JS exception
try:
    ctxJ.eval("throw new Error('j1')")
except RuntimeError:
    pass
check_clean("js-exc")

# 2. callback raises Python exception
try:
    ctxJ.eval("errJ()")
except RuntimeError:
    pass
check_clean("cb-exc")

# 3. promise rejection + result() raise
ctxJ.eval("var pJr = new Promise(function(res, rej){ rej({code: 5}); }); pJr")
ctxJ.run_jobs()
try:
    ctxJ.eval("pJr").result()
    raise AssertionError("J: rejection should raise")
except RuntimeError:
    pass
check_clean("promise-rejection")

# 4. run_jobs job exception (uncatchable: interrupt inside a queued job)
ctxJ.set_time_limit(0)
ctxJ.eval("Promise.resolve().then(function(){ for(;;){} });")
ctxJ.set_time_limit(20)
try:
    ctxJ.run_jobs()
    raise AssertionError("J: run_jobs job should timeout")
except RuntimeError as e:
    expect("timeout" in str(e), str(e))
ctxJ.set_time_limit(0)
check_clean("run_jobs-job-exc")
expect(ctxJ.run_jobs() >= 0, "J: run_jobs still works")

# 5. conversion failure (unsupported type + raw int overflow regression)
try:
    ctxJ.set("zJ", object())
except RuntimeError:
    pass
check_clean("conv-unsupported")
try:
    ctxJ.set("zJ2", 10 ** 300)
except RuntimeError as e:
    expect("OverflowError" in str(e), "J: %r" % (e,))
check_clean("conv-overflow")

# 6. timeout
ctxJ.set_time_limit(20)
try:
    ctxJ.eval("while(true){}")
except RuntimeError:
    pass
ctxJ.set_time_limit(0)
check_clean("timeout")

# 7. error inside a queued job callback (Python exception -> rejection)
ctxJ.eval("Promise.resolve(1).then(function(){ return errJ(); });")
ctxJ.run_jobs()
check_clean("job-cb-exc")

ctxJ.close()
print("J. pending exception contamination OK")

# =============================================================
# K. 1000 exceptions in a row, then eval still works
# =============================================================
ctxK = quickjs.Context()


def cbK_err(x):
    raise TypeError("K boom %d" % (x,))


ctxK.add_callable("errK", cbK_err)
ctxK.eval("function boomK(i){ if (i % 2) throw new Error('k' + i); return errK(i); }")
for i in range(1000):
    try:
        ctxK.eval("boomK(%d)" % i)
        raise AssertionError("K: should raise at %d" % i)
    except RuntimeError:
        pass
    if i % 25 == 0:
        expect(ctxK.eval("1+2") == 3, "K: contaminated at %d" % i)
expect(ctxK.eval("3+4") == 7, "K: final")
ctxK.close()
print("K. 1000 exceptions OK")

# =============================================================
# L. 1000 timeout/reuse cycles
# =============================================================
ctxL = quickjs.Context()
ctxL.set_time_limit(5)
for i in range(1000):
    try:
        ctxL.eval("while(true){}")
        raise AssertionError("L: no timeout at %d" % i)
    except RuntimeError as e:
        expect("timeout" in str(e), str(e))
    if i % 50 == 0:
        expect(ctxL.eval("1+2") == 3, "L: contaminated at %d" % i)
ctxL.set_time_limit(0)
ctxL.eval("function sqrtL(x){ return Math.sqrt(x); }")
expect(ctxL.call("sqrtL", 64) == 8, "L: call after 1000 timeouts")
ctxL.close()
print("L. 1000 timeout/reuse OK")

# =============================================================
# M. 1000 callback/reentrancy cycles
# =============================================================
ctxM = quickjs.Context()


def cbM_sqr(x):
    return x * x


def cbM_eval(x):
    return ctxM.eval("sqrM(%d)" % x) + 1


ctxM.add_callable("sqrM", cbM_sqr)
ctxM.add_callable("evalM", cbM_eval)
ctxM.eval("function chainM(i){ return evalM(i); }")
for i in range(1000):
    r = ctxM.call("chainM", i)
    expect(r == i * i + 1, "M: %d -> %r" % (i, r))
ctxM.close()  # depth must be 0 after 1000 reentrant calls
print("M. 1000 callback/reentrancy OK")

# =============================================================
# N. 100 create/drop/close cycles
# =============================================================
for i in range(100):
    c = quickjs.Context()
    c.eval("function fN(){ return 'n' + %d; }" % i)
    expect(c.call("fN") == "n%d" % i, "N: ctx %d" % i)
    c.set("xN", i)
    expect(c.eval("xN") == i, "N: set/get %d" % i)
    p = c.eval("new Promise(function(res){ res(%d); });" % i)
    c.run_jobs()
    expect(p.result() == i, "N: promise %d" % i)
    c.close()
    if i % 10 == 9:
        gc.collect()
print("N. 100 create/drop/close OK")

# =============================================================
# O. stress: mixed operations
# =============================================================
ctxO = quickjs.Context()
ctxO.set_time_limit(3000)


def cbO_add(a, b):
    return a + b


ctxO.add_callable("addO", cbO_add)


def expectO(cond, msg):
    expect(cond, msg)


ctxO.eval("""
function fibO(n){ return n < 2 ? n : fibO(n-1) + fibO(n-2); }
function loopO(n){ var s = 0; for (var i = 0; i < n; i++) s += i; return s; }
var bigO = 123456789012345678901234567890n;
""")
# wrappers + bigint round trip
f = ctxO.eval("(function wO(x){ return x * 2; })")
expectO(f(21) == 42, "O: wrapper")
expectO(f.call({"k": 1}, 21) == 42, "O: wrapper.call this")
big = quickjs.bigint(10 ** 30 + 7)
ctxO.set("mybig", big)
r = ctxO.eval("mybig.toString()")
expectO(r == str(10 ** 30 + 7), "O: bigint round trip")

# promise chains + callbacks
ctxO.eval("var pO = Promise.resolve(2).then(function(x){ return addO(x, 40); });")
ctxO.run_jobs()
expectO(ctxO.eval("pO").result() == 42, "O: promise chain")

# conversion error once
try:
    ctxO.set("badO", 10 ** 300)
    expectO(False, "O: conversion should fail")
except RuntimeError as e:
    expectO("OverflowError" in str(e), "O: %r" % (e,))

# mixed exec after error
expectO(ctxO.eval("loopO(100)") == 4950, "O: loopO")
expectO(ctxO.eval("fibO(15)") == 610, "O: fibO")
expectO(ctxO.eval("typeof bigO") == "bigint", "O: bigint typeof")

# gc churn
for _ in range(30):
    for _ in range(10):
        ctxO.eval("var tmpO = [1,2,3]; tmpO")
    gc.collect()
    ctxO.gc()
expectO(ctxO.eval("10+10") == 20, "O: after churn")

# timeout interleaved
ctxO.set_time_limit(20)
try:
    ctxO.eval("while(true){}")
    expectO(False, "O: expected timeout")
except RuntimeError as e:
    expectO("timeout" in str(e), str(e))
ctxO.set_time_limit(0)
expectO(ctxO.eval("1+2") == 3, "O: after timeout")

# typed array path still fine
ta = ctxO.eval("new Uint16Array([1,2,3])")
expectO(len(ta) == 6, "O: typedarray bytes len")
expectO(ta == bytes([1, 0, 2, 0, 3, 0]), "O: typedarray bytes val")

ctxO.close()
print("O. stress mixed OK")

# =============================================================
# P. regression: nested eval must not cancel the outer timeout
# =============================================================
ctxP = quickjs.Context()
ctxP.set_time_limit(150)
countP = [0]


def cbP_eval():
    countP[0] += 1
    r = ctxP.eval("1 + 2")
    expect(r == 3, "P: inner eval")
    return None


ctxP.add_callable("cbP", cbP_eval)
try:
    # outer infinite loop; every iteration calls a Python callback that
    # itself runs an eval. Outer deadline must stay armed (shared budget).
    ctxP.eval("for(;;){ cbP(); }")
    raise AssertionError("P: outer loop escaped the timeout budget")
except RuntimeError as e:
    expect("timeout" in str(e), str(e))
expect(countP[0] > 0, "P: callback ran")
ctxP.set_time_limit(0)
expect(ctxP.eval("1+2") == 3, "P: usable after nested-budget timeout")
ctxP.close()
print("P. nested timeout budget OK (%d inner evals)" % countP[0])

# =============================================================
# Q. regression: conversion failure ownership
# =============================================================
ctxQ = quickjs.Context()
ctxQ.eval("function fQ(a, b) { return a; }")

# each failing conversion must raise RuntimeError embedding the Python error
cases = [
    ("arg-str", lambda: ctxQ.call("fQ", "leak-me-str", 10 ** 300)),
    ("arg-list", lambda: ctxQ.call("fQ", ["a", "b"], 10 ** 300)),
    ("set-huge", lambda: ctxQ.set("hugeQ", 10 ** 300)),
    ("set-list", lambda: ctxQ.set("hugeQ", [1, 10 ** 300])),
]
for tag, fn in cases:
    try:
        fn()
        raise AssertionError("Q: %s should raise" % tag)
    except RuntimeError as e:
        expect("OverflowError" in str(e), "Q: %s: %r" % (tag, e))
    expect(ctxQ.eval("1+2") == 3, "Q: usable after %s" % tag)

# nested container failure (deep partial conversion)
try:
    ctxQ.call("fQ", [1, [2, 10 ** 300]])
    raise AssertionError("Q: nested should raise")
except RuntimeError as e:
    expect("OverflowError" in str(e), "Q: nested: %r" % (e,))
expect(ctxQ.eval("1+2") == 3, "Q: usable after nested")

# wrapper.call this-conversion failure
mQ = ctxQ.eval("(function mQ(x){ return this.base + x; })")
expect(mQ.call({"base": 5}, 2) == 7, "Q: call-this ok")
try:
    mQ.call({"base": 5, "x": 10 ** 300}, 2)
    raise AssertionError("Q: call-this should raise")
except RuntimeError as e:
    expect("OverflowError" in str(e), "Q: call-this: %r" % (e,))
expect(ctxQ.eval("1+2") == 3, "Q: usable after call-this")

# bigint marker path unaffected (huge ints work through the explicit API)
ctxQ.set("bigQ", quickjs.bigint(10 ** 300))
expect(ctxQ.eval("typeof bigQ") == "bigint", "Q: bigint type")
expect(ctxQ.eval("bigQ.toString()") == str(10 ** 300), "Q: bigint round trip")

# close must be clean: no leaked JSValue survived the failed conversions
ctxQ.close()
print("Q. conversion ownership OK")

print("ALL QUICKJS PHASE6 TESTS PASSED")