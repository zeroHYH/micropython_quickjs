# Phase 7 tests: Python-side Promise creation and control.
#
# Run: micropython tests/test_quickjs_phase7.py
#
# Coverage map (spec A-Z):
#   A. create promise (tuple of wrapper + resolve/reject callables)
#   B. resolve(value)
#   C. reject(value)  [str / int / dict / ValueError]
#   D. resolve-then-duplicate resolve/reject (first settlement wins)
#   E. reject-then-duplicate resolve/reject (stays rejected)
#   F. resolve(None)
#   G. resolve(dict/list)
#   H. resolve(bytes)
#   I. resolve(BigInt)
#   J. resolve(existing Promise) -> native assimilation
#   K. p.then()
#   L. p.catch()
#   M. p.finally_()
#   N. multi-level promise chain
#   O. Python callback + Promise (resolve from inside a .then callback)
#   P. callback raises -> derived promise rejected -> catch recovers
#   Q. Promise GC (del p; gc.collect(); resolve still settles)
#   R. resolve/reject GC (del one resolver; other still works)
#   S. close() then resolve/reject -> RuntimeError("context closed")
#   T. many GC orders (del resolve/reject/p in different orders + close)
#   U. timeout + promise executor/job
#   V. promise + reentrant callback (close inside .then -> busy; ctx reusable)
#   W. 1000 promise create/resolve
#   X. 1000 promise reject
#   Y. 1000 promise + callback
#   Z. memory audit (JS heap must not grow without bound)
#
# U. ASAN: run under the ASAN DEBUG build (asserts on, detection on).

print("phase7 start")

import quickjs
import gc

_all_ctxs = []


def expect(cond, msg):
    if not cond:
        raise AssertionError(msg)


def expect_raise(fn, msg_contains, label):
    try:
        fn()
    except Exception as e:
        expect(msg_contains in str(e), "%s: exception %r lacks %r" % (label, e, msg_contains))
        return e
    raise AssertionError("%s: expected exception containing %r" % (label, msg_contains))


# =============================================================
# A. create promise
# =============================================================

ctxA = quickjs.Context()
_all_ctxs.append(ctxA)
ctxA.set_time_limit(5000)

pA, resolveA, rejectA = ctxA.promise()
expect(type(pA).__name__ == "Promise", "A: p is Promise wrapper")
expect(callable(resolveA), "A: resolve callable")
expect(callable(rejectA), "A: reject callable")
expect(pA.done() is False, "A: fresh promise is pending")
expect(hasattr(pA, "then") and hasattr(pA, "catch") and hasattr(pA, "finally_"), "A: wrapper methods")
expect(hasattr(pA, "done") and hasattr(pA, "result"), "A: existing methods intact")
print("A ok")

# =============================================================
# B. resolve(value)
# =============================================================

pB, rB, jB = ctxB = None, None, None
ctxB = quickjs.Context()
_all_ctxs.append(ctxB)
ctxB.set_time_limit(5000)
pB, rB, jB = ctxB.promise()
rB(123)
expect(pB.done(), "B: settled immediately after resolve (non-thenable)")
expect(pB.result() == 123, "B: result == 123")
rB("ignored")
expect(pB.result() == 123, "B: duplicate resolve ignored")
print("B ok")

# =============================================================
# C. reject(value)
# =============================================================

def reject_raises(value, contains):
    ctxC = quickjs.Context()
    ctxC.set_time_limit(5000)
    pC, rC, jC = ctxC.promise()
    jC(value)
    ctxC.run_jobs()
    expect(pC.done(), "C: rejected settled")
    expect_raise(pC.result, contains, "C: result")
    ctxC.close()

reject_raises("failed", "failed")
reject_raises(123, "123")
reject_raises({"error": "failed"}, "[object Object]")
reject_raises(ValueError("failed"), "ValueError: failed")
print("C ok")

# =============================================================
# D. resolve then duplicate resolve/reject (first wins)
# =============================================================

ctxD = quickjs.Context()
_all_ctxs.append(ctxD)
ctxD.set_time_limit(5000)
pD, rD, jD = ctxD.promise()
rD(1)
rD(2)
jD(3)
expect(pD.result() == 1, "D: first resolution wins")
print("D ok")

# =============================================================
# E. reject then duplicate resolve/reject (stays rejected)
# =============================================================

ctxE = quickjs.Context()
_all_ctxs.append(ctxE)
ctxE.set_time_limit(5000)
pE, rE, jE = ctxE.promise()
jE(1)
rE(2)
jE(3)
expect(pE.done(), "E: rejected")
expect_raise(pE.result, "1", "E: result keeps first rejection")
print("E ok")

# =============================================================
# F. resolve(None)
# =============================================================

ctxF = quickjs.Context()
_all_ctxs.append(ctxF)
ctxF.set_time_limit(5000)
pF, rF, jF = ctxF.promise()
rF(None)
expect(pF.result() is None, "F: None -> null -> None")
print("F ok")

# =============================================================
# G. resolve(dict/list)
# =============================================================

ctxG = quickjs.Context()
_all_ctxs.append(ctxG)
ctxG.set_time_limit(5000)
pG, rG, jG = ctxG.promise()
rG([1, 2, [3, 4], "five"])
expect(pG.result() == [1, 2, [3, 4], "five"], "G: list round-trip")
pG2, rG2, jG2 = ctxG.promise()
rG2({"a": 1, "b": [True, None, {"c": "d"}]})
expect(pG2.result() == {"a": 1, "b": [True, None, {"c": "d"}]}, "G: dict round-trip")
print("G ok")

# =============================================================
# H. resolve(bytes)
# =============================================================

ctxH = quickjs.Context()
_all_ctxs.append(ctxH)
ctxH.set_time_limit(5000)
pH, rH, jH = ctxH.promise()
rH(b"\x00\x01\x02\xff")
expect(pH.result() == b"\x00\x01\x02\xff", "H: bytes -> ArrayBuffer -> bytes")
print("H ok")

# =============================================================
# I. resolve(BigInt)
# =============================================================

ctxI = quickjs.Context()
_all_ctxs.append(ctxI)
ctxI.set_time_limit(5000)
pI, rI, jI = ctxI.promise()
rI(quickjs.bigint(2**50))
expect(pI.result() == 2**50, "I: bigint int64-range round-trip")
pI2, rI2, jI2 = ctxI.promise()
rI2(quickjs.bigint(10**30 + 7))
expect_raise(pI2.result, "BigInt out of range", "I: bigint beyond int64 -> TypeError")
expect(ctxI.eval("1+2") == 3, "I: ctx usable after BigInt read error")
print("I ok")

# =============================================================
# J. resolve(existing Promise) -> native assimilation
# =============================================================

ctxJ = quickjs.Context()
_all_ctxs.append(ctxJ)
ctxJ.set_time_limit(5000)
pJ, rJ, jJ = ctxJ.promise()
pJ2 = ctxJ.eval("Promise.resolve(41)")
rJ(pJ2)
ctxJ.run_jobs()
expect(pJ.result() == 41, "J: resolve(existing settled promise) assimilated")

# assimilation when target is a pending Python-created promise:
pJ3, rJ3, jJ3 = ctxJ.promise()
pJ4, rJ4, jJ4 = ctxJ.promise()
rJ4(pJ3)          # resolve pJ4 with pending pJ3
rJ4(999)          # ignored: already resolved (assimilation started)
rJ3(7)            # settle pJ3 (which pJ4 awaits)
ctxJ.run_jobs()
expect(pJ4.result() == 7, "J: resolve(pending promise) assimilated; late value ignored")
print("J ok")

# =============================================================
# K. p.then()
# =============================================================

ctxK = quickjs.Context()
_all_ctxs.append(ctxK)
ctxK.set_time_limit(5000)
pK, rK, jK = ctxK.promise()
qK = pK.then(lambda x: x + 1)
rK(41)
ctxK.run_jobs()
expect(qK.result() == 42, "K: then chain")
print("K ok")

# =============================================================
# L. p.catch()
# =============================================================

ctxL = quickjs.Context()
_all_ctxs.append(ctxL)
ctxL.set_time_limit(5000)
pL, rL, jL = ctxL.promise()
qL = pL.catch(lambda reason: "recovered:%s" % reason)
jL("boom")
ctxL.run_jobs()
expect(qL.result() == "recovered:boom", "L: catch recovers (str reason round-trips)")
# Error-typed reasons convert to {} on the way into a callback (documented
# JS Error -> MP limitation), but catch still runs and ctx stays clean.
pL2, rL2, jL2 = ctxL.promise()
qL2 = pL2.catch(lambda reason: "ran:%s" % (type(reason).__name__,))
jL2(ValueError("boom"))
ctxL.run_jobs()
expect(qL2.result() == "ran:dict", "L: catch runs for Error reason ({} conversion)")
expect(ctxL.eval("1+2") == 3, "L: ctx unpoisoned")
print("L ok")

# =============================================================
# M. p.finally_()
# =============================================================

ctxM = quickjs.Context()
_all_ctxs.append(ctxM)
ctxM.set_time_limit(5000)
pM, rM, jM = ctxM.promise()
qM = pM.finally_(lambda: None)
rM(41)
ctxM.run_jobs()
expect(qM.result() == 41, "M: finally keeps fulfillment value")
pM2, rM2, jM2 = ctxM.promise()
qM2 = pM2.finally_(lambda: None)
jM2("still-rejected")
ctxM.run_jobs()
expect_raise(qM2.result, "still-rejected", "M: finally keeps rejection")
print("M ok")

# =============================================================
# N. multi-level chain
# =============================================================

ctxN = quickjs.Context()
_all_ctxs.append(ctxN)
ctxN.set_time_limit(5000)
pN, rN, jN = ctxN.promise()
qN = pN.then(lambda x: x + 1).then(lambda x: x * 2).then(lambda x: x - 3)
rN(10)
ctxN.run_jobs()
expect(qN.result() == 19, "N: (10+1)*2-3 == 19")
print("N ok")

# =============================================================
# O. Python callback + Promise (resolve driven from a .then callback)
# =============================================================

ctxO = quickjs.Context()
_all_ctxs.append(ctxO)
ctxO.set_time_limit(5000)
pO, rO, jO = ctxO.promise()
pO2, rO2, jO2 = ctxO.promise()
pO2.then(lambda v: rO(v * 2))
rO2(21)
ctxO.run_jobs()
expect(pO.result() == 42, "O: JS job -> Python callback -> resolve another promise")
print("O ok")

# =============================================================
# P. callback raises -> derived promise rejected -> catch recovers
# =============================================================

ctxP = quickjs.Context()
_all_ctxs.append(ctxP)
ctxP.set_time_limit(5000)
pP, rP, jP = ctxP.promise()
qP = pP.then(lambda x: 1 // 0)
cP = qP.catch(lambda reason: "caught:%s" % reason)
rP(1)
ctxP.run_jobs()
expect(cP.result().startswith("caught:"), "P: callback exception rejected derived promise")
expect(ctxP.eval("1+2") == 3, "P: ctx unpoisoned")
print("P ok")

# =============================================================
# Q. Promise GC: promise survives wrapper death via resolving funcs
# =============================================================

ctxQ = quickjs.Context()
_all_ctxs.append(ctxQ)
ctxQ.set_time_limit(5000)
pQ, rQ, jQ = ctxQ.promise()
ctxQ.set("qProm", pQ)      # publish so JS side keeps it
qObs = ctxQ.eval("qProm").then(lambda v: v * 10)
del pQ
gc.collect()
rQ(5)
ctxQ.run_jobs()
expect(qObs.result() == 50, "Q: resolve works after p wrapper collected")
print("Q ok")

# =============================================================
# R. resolve/reject GC: dropping one resolver leaves the other working
# =============================================================

ctxR = quickjs.Context()
_all_ctxs.append(ctxR)
ctxR.set_time_limit(5000)
pR, rR, jR = ctxR.promise()
del rR
gc.collect()
jR("rejected-after-resolve-gc")
expect_raise(pR.result, "rejected-after-resolve-gc", "R: reject still works")

pR2, rR2, jR2 = ctxR.promise()
del jR2
gc.collect()
rR2(77)
expect(pR2.result() == 77, "R: resolve still works after reject GC")
print("R ok")

# =============================================================
# S. close() then resolve/reject -> RuntimeError("context closed")
# =============================================================

ctxS = quickjs.Context()
_all_ctxs.append(ctxS)
ctxS.set_time_limit(5000)
pS, rS, jS = ctxS.promise()
ctxS.close()
expect_raise(lambda: rS(1), "context closed", "S: resolve after close")
expect_raise(lambda: jS(ValueError("x")), "context closed", "S: reject after close")
expect_raise(lambda: pS.result(), "context closed", "S: result after close")
expect_raise(lambda: pS.then(lambda x: x), "context closed", "S: then after close")
expect_raise(lambda: pS.done(), "context closed", "S: done after close")
print("S ok")

# =============================================================
# T. many GC orders + close (no UAF / crash / assert)
# =============================================================

def t_cycle(order):
    ctxT = quickjs.Context()
    ctxT.set_time_limit(5000)
    pT, rT, jT = ctxT.promise()
    ctxT.set("tProm", pT)
    for step in order:
        if step == "p":
            del pT
        elif step == "r":
            del rT
        elif step == "j":
            del jT
        gc.collect()
    ctxT.close()
    gc.collect()

t_cycle(["p"])
t_cycle(["r"])
t_cycle(["j"])
t_cycle(["p", "r"])
t_cycle(["r", "j"])
t_cycle(["p", "j"])
t_cycle(["p", "r", "j"])
t_cycle(["r", "p", "j"])
t_cycle(["j", "p", "r"])
# close-first then del+gc (finalisers must tolerate closed context)
ctxT2 = quickjs.Context()
_all_ctxs.append(ctxT2)
ctxT2.set_time_limit(5000)
pT2, rT2, jT2 = ctxT2.promise()
ctxT2.close()
del pT2
del rT2
del jT2
gc.collect()
print("T ok")

# =============================================================
# U. timeout + promise executor / job
# =============================================================

ctxU = quickjs.Context()
_all_ctxs.append(ctxU)
ctxU.set_time_limit(100)

# Executor runs synchronously inside `new Promise(...)`; a busy-loop executor
# is interrupted by the timeout and the Promise constructor turns the
# interrupt into a rejection (JS semantics): eval returns, promise rejected,
# context reusable, no hang.
pwU = ctxU.eval("new Promise(function () { while (true) {} })")
expect(pwU.done(), "U: executor interrupted -> settled")
expect_raise(pwU.result, "interrupted", "U: rejection carries the interrupt")
expect(ctxU.eval("1+2") == 3, "U: ctx reusable after executor timeout")

# timeout inside a pure-JS microtask job (running via run_jobs):
# the interrupt propagates out of the job runner.
ctxU2 = quickjs.Context()
_all_ctxs.append(ctxU2)
ctxU2.set_time_limit(100)
ctxU2.eval("var jobProm = Promise.resolve().then(function () { while (true) {} });")
expect_raise(
    lambda: ctxU2.run_jobs(),
    "timeout", "U: job busy loop interrupted")
expect(ctxU2.eval("3+4") == 7, "U: ctx reusable after job timeout")
print("U ok")

# =============================================================
# V. promise + reentrant callback (close inside .then -> busy)
# =============================================================

ctxV = quickjs.Context()
_all_ctxs.append(ctxV)
ctxV.set_time_limit(5000)
pV, rV, jV = ctxV.promise()


def cbV_closer(v):
    try:
        ctxV.close()
    except Exception as e:
        return "busy:%s" % e
    return "closed?"


qV = pV.then(cbV_closer)
rV(1)
ctxV.run_jobs()
expect("busy:context is busy" in qV.result(), "V: close inside then rejected with busy")
expect(ctxV.eval("1+2") == 3, "V: ctx usable after busy rejection")

# reentrant: .then callback creates + resolves a NEW promise
pV2, rV2, jV2 = ctxV.promise()


def cbV2(v):
    pn, rn, jn = ctxV.promise()
    rn(v + 100)
    return pn  # return a promise wrapper from callback -> assimilation


qV2 = pV2.then(cbV2)
rV2(1)
ctxV.run_jobs()
expect(qV2.result() == 101, "V: callback-create promise + assimilation")
print("V ok")

# =============================================================
# W. 1000 promise create/resolve
# =============================================================

ctxW = quickjs.Context()
_all_ctxs.append(ctxW)
ctxW.set_time_limit(5000)
for i in range(1000):
    pW, rW, jW = ctxW.promise()
    rW(i)
    expect(pW.result() == i, "W: round-trip %d" % i)
    if i % 100 == 0:
        gc.collect()
print("W ok")

# =============================================================
# X. 1000 promise reject
# =============================================================

ctxX = quickjs.Context()
_all_ctxs.append(ctxX)
ctxX.set_time_limit(5000)
for i in range(1000):
    pX, rX, jX = ctxX.promise()
    jX(ValueError("x%d" % i))
    expect_raise(pX.result, "x%d" % i, "X: reject %d" % i)
    if i % 100 == 0:
        gc.collect()
expect(ctxX.eval("1+2") == 3, "X: ctx usable after 1000 rejects")
print("X ok")

# =============================================================
# Y. 1000 promise + callback
# =============================================================

ctxY = quickjs.Context()
_all_ctxs.append(ctxY)
ctxY.set_time_limit(5000)
for i in range(1000):
    pY, rY, jY = ctxY.promise()
    qY = pY.then(lambda x: x + 1)
    rY(i)
    ctxY.run_jobs()
    expect(qY.result() == i + 1, "Y: chain %d" % i)
    if i % 100 == 0:
        gc.collect()
print("Y ok")

# =============================================================
# Z. memory audit: JS heap must not grow without bound
# =============================================================

ctxZ = quickjs.Context()
_all_ctxs.append(ctxZ)
ctxZ.set_time_limit(5000)

base = ctxZ._js_mem()
for i in range(1000):
    pZ, rZ, jZ = ctxZ.promise()
    qZ = pZ.then(lambda x: x + 1)
    rZ(i)
    ctxZ.run_jobs()
    expect(qZ.result() == i + 1, "Z: chain %d" % i)
    del pZ, rZ, jZ, qZ
    if i % 100 == 0:
        gc.collect()
        ctxZ.gc()
gc.collect()
ctxZ.gc()
after = ctxZ._js_mem()
expect(after - base < 64 * 1024, "Z: JS heap grew %d bytes > 64 KiB" % (after - base))
# MicroPython heap sanity: proves live objects did not accumulate unboundedly
mp_alloc_before = gc.mem_alloc()
for i in range(500):
    pZ, rZ, jZ = ctxZ.promise()
    del pZ, rZ, jZ
gc.collect()
expect(gc.mem_alloc() - mp_alloc_before < 32 * 1024,
       "Z: MP heap grew %d bytes > 32 KiB" % (gc.mem_alloc() - mp_alloc_before))
ctxZ.close()
print("Z ok")

for _c in _all_ctxs:
    try:
        _c.close()
    except Exception:
        pass
gc.collect()
print("ALL QUICKJS PHASE7 TESTS PASSED")