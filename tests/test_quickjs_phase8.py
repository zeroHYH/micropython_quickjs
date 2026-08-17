# Phase 8 tests: Promise / async-await execution model.
#
# Run: micropython tests/test_quickjs_phase8.py
#
# The QuickJS engine natively implements async functions, await and the
# Promise reaction job queue.  This module's job is to prove that the
# existing integration (run_jobs() / has_pending_jobs() / Promise wrapper /
# callback registry / ctx.promise()) drives that machinery correctly, and to
# pin down the *actual* job-queue semantics (one run_jobs() drains
# everything; async functions that complete without awaiting settle
# synchronously).
#
# Coverage map (A-T as specified, plus U):
#   A. async function return          B. async function + await
#   C. await Promise.resolve()        D. await Python-created Promise
#   E. async rejection                F. await rejection + try/catch
#   G. async Promise chain            H. async callback (reentrancy)
#   I. Python callback returning Promise (assimilation pass-through)
#   J. multiple async functions       K. nested await
#   L. many jobs                      M. timeout during async job chain
#   N. timeout 后 Context 可复用       O. close/busy during async callback
#   P. GC + pending Promise           Q. Promise wrapper 生命周期
#   R. 1000 async/await stress        S. 1000 Promise chain stress
#   T. pending jobs drain 完整性       U. error model (callback exc / OOM /
#                                        conversion error -> ctx clean)
#
# ASAN: run under the ASAN DEBUG build (asserts on, detection on).

print("phase8 start")

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


def check_clean(ctx, label):
    """Error-model invariant: no pending-exception pollution.  Every error
    path must leave the context fully usable: eval('1 + 2') == 3."""
    expect(ctx.eval("1 + 2") == 3, "%s: context unusable after error" % label)


# =============================================================
# A. async function return
# =============================================================
# Actual QuickJS semantics: an async function that completes without any
# suspension point resolves its result promise SYNCHRONOUSLY -- no job is
# enqueued, done() is true immediately after the call.
ctxA = quickjs.Context()
_all_ctxs.append(ctxA)
ctxA.set_time_limit(5000)

ctxA.eval("async function fooA() { return 42; }")
pA = ctxA.get("fooA")()
expect(pA.done() is True, "A: sync-resolved async fn is done immediately")
expect(ctxA.has_pending_jobs() is False, "A: no job for sync-completing async fn")
nA = ctxA.run_jobs()
expect(nA == 0, "A: run_jobs with no jobs returns 0")
expect(pA.result() == 42, "A: result")
print("A ok")

# =============================================================
# B. async function + await
# =============================================================
# With a real suspension point the promise must stay pending until jobs run.
ctxB = quickjs.Context()
_all_ctxs.append(ctxB)
ctxB.set_time_limit(5000)

ctxB.eval("async function fooB() { const x = await 1; return x + 41; }")
pB = ctxB.get("fooB")()
expect(pB.done() is False, "B: pending before jobs run")
expect(ctxB.has_pending_jobs() is True, "B: await enqueued a job")
nB = ctxB.run_jobs()
expect(pB.done() is True, "B: done after run_jobs")
expect(pB.result() == 42, "B: result")
expect(ctxB.has_pending_jobs() is False, "B: drained")
print("B ok")

# =============================================================
# C. await Promise.resolve()
# =============================================================
ctxC = quickjs.Context()
_all_ctxs.append(ctxC)
ctxC.set_time_limit(5000)

ctxC.eval("async function fooC() { const x = await Promise.resolve(41); return x + 1; }")
pC = ctxC.get("fooC")()
expect(pC.done() is False, "C: pending before")
nC = ctxC.run_jobs()
expect(nC > 0, "C: some jobs ran")
expect(pC.result() == 42, "C: result")
expect(ctxC.has_pending_jobs() is False, "C: fully drained")
# empty drain returns 0
expect(ctxC.run_jobs() == 0, "C: empty drain -> 0")
print("C ok")

# =============================================================
# D. await Python-created Promise
# =============================================================
ctxD = quickjs.Context()
_all_ctxs.append(ctxD)
ctxD.set_time_limit(5000)

pD0, rD, jD = ctxD.promise()
ctxD.set("pD0", pD0)
ctxD.eval("async function fooD() { const x = await pD0; return x + 1; }")
pD = ctxD.get("fooD")()
expect(pD.done() is False, "D: pending while awaiting python promise")
rD(41)
nD = ctxD.run_jobs()
expect(pD.result() == 42, "D: result after resolve + drain")
expect(nD > 0, "D: jobs ran")
# also rejected path: awaiting a python-rejected promise without handler
pD1, rD1, jD1 = ctxD.promise()
ctxD.set("pD1", pD1)
ctxD.eval("async function fooD2() { const x = await pD1; return x; }")
pD2 = ctxD.get("fooD2")()
jD1("rejected-by-python")
ctxD.run_jobs()
expect(pD2.done() is True, "D2: rejected promise settled")
expect_raise(pD2.result, "rejected-by-python", "D2")
check_clean(ctxD, "D2")
print("D ok")

# =============================================================
# E. async rejection
# =============================================================
ctxE = quickjs.Context()
_all_ctxs.append(ctxE)
ctxE.set_time_limit(5000)

ctxE.eval("async function badE() { throw new Error('boom'); }")
pE = ctxE.get("badE")()
ctxE.run_jobs()
expect(pE.done() is True, "E: rejected promise is settled")
expect_raise(pE.result, "boom", "E")
check_clean(ctxE, "E")

# rejection via await of a rejected native promise
ctxE.eval("async function badE2() { await Promise.reject('nope'); }")
pE2 = ctxE.get("badE2")()
ctxE.run_jobs()
expect_raise(pE2.result, "nope", "E2")
check_clean(ctxE, "E2")

# rejection via async throw of a non-Error value
ctxE.eval("async function badE3() { throw 12345; }")
pE3 = ctxE.get("badE3")()
ctxE.run_jobs()
expect_raise(pE3.result, "12345", "E3")
check_clean(ctxE, "E3")
print("E ok")

# =============================================================
# F. await rejection + try/catch
# =============================================================
ctxF = quickjs.Context()
_all_ctxs.append(ctxF)
ctxF.set_time_limit(5000)

ctxF.eval("""
async function goodF() {
    try {
        await Promise.reject('x');
        return 0;
    } catch (e) {
        return 7;
    }
}
""")
pF = ctxF.get("goodF")()
ctxF.run_jobs()
expect(pF.result() == 7, "F: try/catch recovers from awaited rejection")

# catch recovers a previously rejected async promise
ctxF.eval("async function badF() { throw new Error('fboom'); }")
pF2 = ctxF.get("badF")()
ctxF.run_jobs()
hits = []


def on_rej(r):
    hits.append("caught")
    return "recovered"


qF = pF2.catch(on_rej)
ctxF.run_jobs()
expect(qF.result() == "recovered", "F: catch on async-rejected promise")
expect(hits == ["caught"], "F: handler ran")
check_clean(ctxF, "F")
print("F ok")

# =============================================================
# G. async Promise chain
# =============================================================
ctxG = quickjs.Context()
_all_ctxs.append(ctxG)
ctxG.set_time_limit(5000)

ctxG.eval("""
async function stepG1(x) { return x + 1; }
async function stepG2(x) { const y = await stepG1(x); return y * 2; }
""")
pG = ctxG.get("stepG2")(10)
ctxG.run_jobs()
expect(pG.result() == 22, "G: async fn chain")

# JS-side chain with async handlers inside .then
ctxG.eval("""
var chainG = Promise.resolve(1)
    .then(async x => { const y = await Promise.resolve(x + 1); return y + 1; })
    .then(x => x * 2);
""")
pG2 = ctxG.get("chainG")
ctxG.run_jobs()
expect(pG2.result() == 6, "G: .then(async) chain")

# Python .then on an async-derived promise
pG3 = ctxG.get("stepG2")(1)
qG3 = pG3.then(lambda x: x + 100)
ctxG.run_jobs()
expect(qG3.result() == 104, "G: python .then after async fn")
check_clean(ctxG, "G")
print("G ok")

# =============================================================
# H. async callback (Python callable invoked from inside a job)
# =============================================================
ctxH = quickjs.Context()
_all_ctxs.append(ctxH)
ctxH.set_time_limit(5000)

orderH = []


def cbH(v):
    orderH.append("job")
    # reentrancy: nested execution window from inside the job pump
    inner = ctxH.eval("2 * 3")
    return v + inner


ctxH.add_callable("cbH", cbH)
ctxH.eval("async function hH() { const v = await Promise.resolve(); return (v || 0) + await cbH(5); }")
p = ctxH.get("hH")()
ctxH.run_jobs()
expect(p.result() == 11, "H: callback value flows through async fn")
expect(orderH == ["job"], "H: callback ran during job pump")
check_clean(ctxH, "H")
print("H ok")

# =============================================================
# I. Python callback returning Promise wrapper (assimilation)
# =============================================================
ctxI = quickjs.Context()
_all_ctxs.append(ctxI)
ctxI.set_time_limit(5000)

pI0, rI, jI = ctxI.promise()
ctxI.set("pI0", pI0)


def cbI():
    return pI0  # promise wrapper pass-through -> JS assimilation


ctxI.add_callable("cbI", cbI)
ctxI.eval("async function iI() { const x = await cbI(); return x + 1; }")
pI = ctxI.get("iI")()
expect(pI.done() is False, "I: pending while awaiting callback promise")
rI(41)
ctxI.run_jobs()
expect(pI.result() == 42, "I: callback-returned promise assimilated")

# callback returning an ALREADY-SETTLED python promise
pI1, rI1, jI1 = ctxI.promise()
rI1(99)


def cbI2():
    return pI1


ctxI.add_callable("cbI2", cbI2)
ctxI.eval("async function iI2() { return await cbI2(); }")
p = ctxI.get("iI2")()
ctxI.run_jobs()
expect(p.result() == 99, "I2: settled python promise from callback")
check_clean(ctxI, "I")
print("I ok")

# =============================================================
# J. multiple async functions
# =============================================================
ctxJ = quickjs.Context()
_all_ctxs.append(ctxJ)
ctxJ.set_time_limit(5000)

ctxJ.eval("""
var jlog = [];
async function jA() { jlog.push('A'); await Promise.resolve(); jlog.push('a'); return 'A'; }
async function jB() { jlog.push('B'); await Promise.resolve(); jlog.push('b'); return 'B'; }
async function jC() { jlog.push('C'); await Promise.resolve(); jlog.push('c'); return 'C'; }
""")
pA = ctxJ.get("jA")()
pB = ctxJ.get("jB")()
pC = ctxJ.get("jC")()
n = ctxJ.run_jobs()
expect(n == 3, "J: one job per started async fn")
expect(pA.result() == "A" and pB.result() == "B" and pC.result() == "C", "J: results")
expect(ctxJ.eval("jlog") == ["A", "B", "C", "a", "b", "c"], "J: interleaving order (sync prologue, async continuation)")
expect(ctxJ.has_pending_jobs() is False, "J: drained")
print("J ok")

# =============================================================
# K. nested await
# =============================================================
ctxK = quickjs.Context()
_all_ctxs.append(ctxK)
ctxK.set_time_limit(5000)

ctxK.eval("""
async function deepK() {
    const a = await Promise.resolve(1);
    const b = await Promise.resolve(a + 1);
    const c = await Promise.resolve(b + 1);
    const d = await Promise.resolve(c + 1);
    return d;
}
""")
pK = ctxK.get("deepK")()
ctxK.run_jobs()
expect(pK.result() == 4, "K: 4-level nested await")

# async fn awaiting an async fn's promise (mixed nesting)
ctxK.eval("""
async function midK() { const v = await deepK(); return v * 10; }
async function topK() { return await midK(); }
""")
pK2 = ctxK.get("topK")()
ctxK.run_jobs()
expect(pK2.result() == 40, "K: nested async calls")
print("K ok")

# =============================================================
# L. many jobs
# =============================================================
ctxL = quickjs.Context()
_all_ctxs.append(ctxL)
ctxL.set_time_limit(10000)

ctxL.eval("""
var lcnt = 0;
async function bumpL() { lcnt += 1; await Promise.resolve(); lcnt += 10; }
""")
for _ in range(200):
    ctxL.get("bumpL")()
n = ctxL.run_jobs()
expect(n == 200, "L: all 200 jobs executed in one drain")
expect(ctxL.eval("lcnt") == 200 * 11, "L: every async fn completed")
expect(ctxL.has_pending_jobs() is False, "L: drained")
expect(ctxL.run_jobs() == 0, "L: second drain 0")
print("L ok")

# =============================================================
# M. timeout during async job chain
# =============================================================
ctxM = quickjs.Context()
_all_ctxs.append(ctxM)
ctxM.set_time_limit(20)

# M1: async fn that never finishes (await loop) -> run_jobs must time out
ctxM.eval("async function spinM() { while (true) { await Promise.resolve(); } }")
pM = ctxM.get("spinM")()
expect_raise(ctxM.run_jobs, "timeout", "M1")
expect(pM.done() is False, "M1: spin promise still pending")

# M2: sync infinite loop inside a .then job (pure JS, no await)
ctxM.eval("""
var spinPromise = Promise.resolve(1).then(function () { while (true) {} });
""")
expect_raise(ctxM.run_jobs, "timeout", "M2")

# M3: long but finite async chain exceeds the deadline
ctxM.set_time_limit(20)
ctxM.eval("async function slowM() { for (var i = 0; i < 200000; i++) { await Promise.resolve(); } return 1; }")
pM = ctxM.get("slowM")()
expect_raise(ctxM.run_jobs, "timeout", "M3")
expect(pM.done() is False, "M3: promise still pending after mid-flight timeout")
print("M ok")

# =============================================================
# N. timeout 后 Context 可复用
# =============================================================
# (continues on ctxM)
check_clean(ctxM, "N")
expect(ctxM.eval("3 + 4") == 7, "N: eval after timeout")
ctxM.eval("async function okN() { return await Promise.resolve(5); }")
pN = ctxM.get("okN")()
ctxM.run_jobs()
expect(pN.result() == 5, "N: fresh async work after timeout")
ctxM.set_time_limit(0)
pN2 = ctxM.get("okN")()
ctxM.run_jobs()
expect(pN2.result() == 5, "N: works with timeout disabled")
print("N ok")

# =============================================================
# O. close/busy during async callback
# =============================================================
ctxO = quickjs.Context()
_all_ctxs.append(ctxO)
ctxO.set_time_limit(5000)

# uncaught: close() invoked from inside the job pump is refused with
# "context is busy"; the RuntimeError crosses as a JS exception -> the
# async promise is rejected.  The context itself stays fully usable.
def closerO():
    ctxO.close()
    return 1


ctxO.add_callable("closerO", closerO)
ctxO.eval("async function tO() { const v = await Promise.resolve(); return (v || 0) + closerO(); }")
p = ctxO.get("tO")()
ctxO.run_jobs()
expect(p.done() is True, "O: rejected promise settled")
expect_raise(p.result, "context is busy", "O")
expect(ctxO.has_pending_jobs() is False, "O: drained")
check_clean(ctxO, "O")

# caught variant: containing the refusal inside the callback keeps the
# promise fulfilled; the busy-guard proof is recorded separately.
flagsO = []


def closerO2():
    try:
        ctxO.close()
        flagsO.append("REFUSED")
    except RuntimeError as e:
        flagsO.append("busy" in str(e))
    return 0


ctxO.add_callable("closerO2", closerO2)
ctxO.eval("async function tO2() { const v = await Promise.resolve(); return (v || 0) + closerO2(); }")
p2 = ctxO.get("tO2")()
ctxO.run_jobs()
expect(flagsO == [True], "O2: close refused with busy (caught)")
expect(p2.result() == 0, "O2: promise fulfilled when error contained")

# close() OUTSIDE any execution window succeeds; afterwards every access
# path reports "context closed".
ctxO.close()
expect_raise(ctxO.run_jobs, "context closed", "O3")
expect_raise(p.done, "context closed", "O3")
expect_raise(p.result, "context closed", "O3")
ctxO.close()  # idempotent
print("O ok")

# =============================================================
# P. GC + pending Promise
# =============================================================
ctxP = quickjs.Context()
_all_ctxs.append(ctxP)
ctxP.set_time_limit(5000)

# pending promise survives gc; resolve afterwards still settles it
pp, rp, jp = ctxP.promise()
ctxP.set("pp", pp)
ctxP.eval("async function gP() { return await pp; }")
pP = ctxP.get("gP")()
gc.collect()
ctxP.gc()
expect(pP.done() is False, "P: still pending after gc")
rp(21)
ctxP.run_jobs()
expect(pP.result() == 21, "P: settled after resolve post-gc")
del pp, rp, jp
gc.collect()

# context survives deletion of the only Python variable (wrapper strong ref)
ctxP2 = quickjs.Context()
_all_ctxs.append(ctxP2)
ctxP2.eval("async function gP2() { return 33; }")
pP2 = ctxP2.get("gP2")()
ctxP2_alias = ctxP2
del ctxP2
gc.collect()
expect(pP2.done() is True, "P2: wrapper keeps context (state) alive")
expect(pP2.result() == 33, "P2: result via surviving context")
expect(ctxP2_alias.eval("1 + 2") == 3, "P2: context fully usable")
print("P ok")

# =============================================================
# Q. Promise wrapper 生命周期
# =============================================================
ctxQ = quickjs.Context()
_all_ctxs.append(ctxQ)
ctxQ.set_time_limit(5000)

# del async function, gc -> in-flight promise must still complete
ctxQ.eval("async function fQ() { const x = await Promise.resolve(1); return x + 8; }")
fQ_wrap = ctxQ.get("fQ")
pQ = fQ_wrap()
del fQ_wrap
gc.collect()
ctxQ.run_jobs()
expect(pQ.result() == 9, "Q: promise survives async fn wrapper collection")
del pQ
gc.collect()

# del promise wrapper while pending -> resolve still settles (phase7 model)
pQ2, rQ2, jQ2 = ctxQ.promise()
ctxQ.set("pQ2", pQ2)
ctxQ.eval("async function fQ2() { return await pQ2; }")
qQ2 = ctxQ.get("fQ2")()
del pQ2
gc.collect()
rQ2(4)
ctxQ.run_jobs()
expect(qQ2.result() == 4, "Q2: wrapper deleted, resolve still settles")

# del derived wrapper: parent chain unaffected
qQ3 = qQ2.then(lambda x: x * 2)
ctxQ.run_jobs()
expect(qQ3.result() == 8, "Q3: derived promise works after drain")
del qQ2, qQ3, rQ2, jQ2
gc.collect()
expect(ctxQ.eval("1 + 2") == 3, "Q: ctx clean after wrapper churn")
print("Q ok")

# =============================================================
# R. 1000 async/await stress
# =============================================================
ctxR = quickjs.Context()
_all_ctxs.append(ctxR)
ctxR.set_time_limit(20000)

ctxR.eval("""
var racc = 0;
async function arR(x) {
    const a = await Promise.resolve(x);
    const b = await Promise.resolve(a + 1);
    racc += b;
    return b;
}
""")
base = ctxR._js_mem()
total_jobs = 0
for i in range(1000):
    p = ctxR.get("arR")(i)
    total_jobs += ctxR.run_jobs()
    expect(p.result() == i + 1, "R: result %d" % i)
    del p
    if i % 100 == 0:
        gc.collect()
        ctxR.gc()
gc.collect()
ctxR.gc()
expect(total_jobs == 2000, "R: exactly 2 jobs per async invocation")
expect(ctxR.eval("racc") == 500500, "R: accumulator")
expect(ctxR._js_mem() - base < 64 * 1024,
       "R: JS heap grew %d bytes > 64 KiB" % (ctxR._js_mem() - base))
mp_before = gc.mem_alloc()
for i in range(500):
    p = ctxR.get("arR")(i)
    ctxR.run_jobs()
    del p
gc.collect()
expect(gc.mem_alloc() - mp_before < 32 * 1024,
       "R: MP heap grew %d bytes > 32 KiB" % (gc.mem_alloc() - mp_before))
print("R ok")

# =============================================================
# S. 1000 Promise chain stress
# =============================================================
ctxS = quickjs.Context()
_all_ctxs.append(ctxS)
ctxS.set_time_limit(20000)

# 1000-deep JS-side .then chain
src = "var sc = Promise.resolve(1);" + "sc = sc.then(x => x + 1);" * 1000
base = ctxS._js_mem()
ctxS.eval(src)
n = ctxS.run_jobs()
expect(n == 1000, "S: 1000 reaction jobs")
expect(ctxS.get("sc").result() == 1001, "S: chain result")
expect(ctxS.has_pending_jobs() is False, "S: drained")
gc.collect()
ctxS.gc()
expect(ctxS._js_mem() - base < 64 * 1024,
       "S: JS heap grew %d bytes > 64 KiB" % (ctxS._js_mem() - base))

# Python-side .then chain (200 links, wrapper churn)
pS0, rS0, jS0 = ctxS.promise()
qS = pS0
for i in range(200):
    qS = qS.then(lambda x, i=i: x + 1)
rS0(1)
n = ctxS.run_jobs()
expect(n == 200, "S2: 200 python-then jobs")
expect(qS.result() == 201, "S2: python chain result")
del pS0, rS0, jS0, qS
gc.collect()
ctxS.gc()
expect(ctxS.eval("1 + 2") == 3, "S: ctx clean after chain stress")
print("S ok")

# =============================================================
# T. pending jobs drain 完整性
# =============================================================
ctxT = quickjs.Context()
_all_ctxs.append(ctxT)
ctxT.set_time_limit(10000)

ctxT.eval("""
var tlog = [];
async function tmain() {
    await Promise.resolve();       // job 1
    tlog.push(1);
    await Promise.resolve();       // job 2
    tlog.push(2);
    await Promise.resolve();       // job 3
    tlog.push(3);
}
""")
p = ctxT.get("tmain")()
n = ctxT.run_jobs()
expect(p.done() is True, "T: one run_jobs completes the whole chain")
expect(ctxT.eval("tlog") == [1, 2, 3], "T: all stages ran in order")
expect(ctxT.has_pending_jobs() is False, "T: no pending after drain")
expect(ctxT.run_jobs() == 0, "T: second run_jobs returns 0")

# reentrant drain: job -> python callback -> resolve() enqueues more jobs,
# the SAME run_jobs() call must keep draining to the end.
ctxT2 = quickjs.Context()
_all_ctxs.append(ctxT2)
ctxT2.set_time_limit(10000)
p2, r2, j2 = ctxT2.promise()
ctxT2.set("mp2", p2)


def trigT2():
    r2(5)  # settle a Python promise from inside a JS job
    return 1


ctxT2.add_callable("trigT2", trigT2)
ctxT2.eval("""
async function touter() {
    const v1 = await trigT2();        // job runs callback -> resolves mp2
    const v2 = await mp2;             // reaction enqueued in the same drain
    return v1 + v2;
}
""")
q = ctxT2.get("touter")()
n = ctxT2.run_jobs()
expect(n >= 2, "T2: multiple jobs in one call (got %d)" % n)
expect(q.result() == 6, "T2: chained reaction completed in same drain")
expect(ctxT2.has_pending_jobs() is False, "T2: drained")
print("T ok")

# =============================================================
# U. error model: callback exception / conversion error / OOM
# =============================================================
ctxU = quickjs.Context()
_all_ctxs.append(ctxU)
ctxU.set_time_limit(5000)

# callback raises inside async job -> async fn rejects; ctx clean
def boomU():
    raise ValueError("kaboom-u")


ctxU.add_callable("boomU", boomU)
ctxU.eval("async function vU() { const x = await boomU(); return x; }")
p = ctxU.get("vU")()
ctxU.run_jobs()
expect_raise(p.result, "kaboom-u", "U1")
check_clean(ctxU, "U1")

# conversion error in resolve -> promise stays pending; ctx clean
p3, r3, j3 = ctxU.promise()
ctxU.set("p3", p3)
ctxU.eval("async function cU() { return await p3; }")
cp = ctxU.get("cU")()
try:
    r3(10 ** 300)
    raise AssertionError("U2: resolve(int-overflow) should raise")
except (TypeError, RuntimeError, OverflowError):
    pass
expect(cp.done() is False, "U2: async fn stays pending after failed resolve")
check_clean(ctxU, "U2")

# single bounded OOM -> surfaced exception; ctx immediately clean
ctxU.set_memory_limit(512 * 1024)
expect_raise(lambda: ctxU.eval("var humongous = new Array(100000000).fill(1); 99"),
             "out of memory", "U3")
check_clean(ctxU, "U3")
ctxU.set_memory_limit(0)
print("U ok")

for _c in _all_ctxs:
    try:
        _c.close()
    except Exception:
        pass
gc.collect()
print("ALL QUICKJS PHASE8 TESTS PASSED")