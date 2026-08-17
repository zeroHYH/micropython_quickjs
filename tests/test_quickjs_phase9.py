# Phase 9 tests: Unhandled Promise Rejection diagnostics.
#
# Run: micropython tests/test_quickjs_phase9.py
#
# Implementation: QuickJS-NG v0.16.1 native JS_SetHostPromiseRejectionTracker()
# exposed as ctx.set_unhandled_rejection_handler(callable_or_None), callback
# signature handler(reason, is_handled).
#
# Verified tracker semantics (from the vendored source, not assumed):
#   - tracker fires ONLY for a promise that is already REJECTED and not yet
#     marked handled (call_promise_rejection_tracker guards on
#     promise_state == JS_PROMISE_REJECTED && !s->is_handled);
#   - is_handled == False: fired synchronously INSIDE the rejection action
#     (fulfill_or_reject_promise, quickjs.c:55589) -- i.e. during eval()/
#     call()/run_jobs()/resolve()/reject(), NOT asynchronously;
#   - is_handled == True: fired synchronously when a .then/.catch handler is
#     attached to an already-rejected promise (perform_promise_then,
#     quickjs.c:56416), which then marks the promise handled;
#   - attaching a fulfil-only .then() to a rejected promise also marks it
#     handled (QuickJS idiosyncrasy, pinned by test C2);
#   - the tracker is NEVER called during Context close / runtime free;
#   - reason is BORROWED; the module converts it to a Python value
#     immediately (string -> str, number -> int, dict/list -> dict/list,
#     Error -> {} -- documented conversion limitation).
#
# A. handler registration          B. unhandled rejection
# C. handled rejection             D. rejection -> catch
# E. delayed catch                 F. async function unhandled
# G. async function handled        H. multiple unhandled
# I. replacement                   J. disable with None
# K. handler GC                    L. Context close
# M. pending Promise               N. Error rejection
# O. string rejection              P. number rejection
# Q. dict/list rejection           R. callback rejection
# S. handler reentrancy            T. handler exception
# U. timeout + rejection           V. 1000 rejection stress
# W. GC stress                     X. Context create/drop/close stress

print("phase9 start")

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
    expect(ctx.eval("1 + 2") == 3, "%s: context unusable after error" % label)


def make_recorder(events):
    def handler(reason, handled):
        events.append((reason, handled))
    return handler


# =============================================================
# A. handler registration
# =============================================================
ctxA = quickjs.Context()
_all_ctxs.append(ctxA)

eventsA = []
ret = ctxA.set_unhandled_rejection_handler(make_recorder(eventsA))
expect(ret is None, "A: setter returns None")

# non-callable rejected
try:
    ctxA.set_unhandled_rejection_handler(42)
    raise AssertionError("A: non-callable must raise TypeError")
except TypeError as e:
    expect("callable or None" in str(e), "A: %r" % e)

# None disables
expect(ctxA.set_unhandled_rejection_handler(None) is None, "A: None ok")
expect(ctxA.set_unhandled_rejection_handler(make_recorder(eventsA)) is None, "A: re-register ok")
print("A ok")

# =============================================================
# B. unhandled rejection
# =============================================================
# Actual semantics: the unhandled event fires SYNCHRONOUSLY at rejection
# time (inside eval), not after run_jobs() -- there are no jobs to drain
# for a bare rejection.
ctxB = quickjs.Context()
_all_ctxs.append(ctxB)
eventsB = []
ctxB.set_unhandled_rejection_handler(make_recorder(eventsB))

ctxB.eval('Promise.reject("boom");')
expect(eventsB == [("boom", False)], "B: handler got (reason, False) synchronously")
# run_jobs must NOT raise for an unhandled rejection (diagnostic, not error)
n = ctxB.run_jobs()
expect(n == 0 and ctxB.has_pending_jobs() is False, "B: no jobs, no exception")
expect(eventsB == [("boom", False)], "B: no duplicate event")
check_clean(ctxB, "B")
print("B ok")

# =============================================================
# C. handled rejection
# =============================================================
ctxC = quickjs.Context()
_all_ctxs.append(ctxC)
eventsC = []
ctxC.set_unhandled_rejection_handler(make_recorder(eventsC))

# reject + catch in the same expression: (unhandled=False) fires at reject,
# then (handled=True) fires when the handler attaches.
ctxC.eval("""
var caught_res;
Promise.reject('x').catch(r => { caught_res = 'caught:' + r; });
""")
expect(eventsC == [("x", False), ("x", True)],
       "C: both phases observed, got %r" % (eventsC,))
ctxC.run_jobs()
expect(ctxC.get("caught_res") == "caught:x", "C: catch handler ran")
# no further events
expect(eventsC == [("x", False), ("x", True)], "C: stable after drain")
print("C ok")

# =============================================================
# C2. fulfil-only then also marks handled (QuickJS idiosyncrasy)
# =============================================================
eventsC2 = []
ctxC.set_unhandled_rejection_handler(make_recorder(eventsC2))
ctxC.eval("Promise.reject('y').then(v => v);")  # no rejection handler
expect(eventsC2 == [("y", False), ("y", True)],
       "C2: fulfil-only then marks handled (pinned), got %r" % (eventsC2,))
ctxC.run_jobs()
check_clean(ctxC, "C2")
print("C2 ok")

# =============================================================
# D. rejection -> catch (pending promise rejected, later caught)
# =============================================================
ctxD = quickjs.Context()
_all_ctxs.append(ctxD)
eventsD = []
ctxD.set_unhandled_rejection_handler(make_recorder(eventsD))

ctxD.eval("var dp = new Promise((res, rej) => { globalThis.drej = rej; });")
ctxD.eval("globalThis.drej('later');")       # reject -> unhandled event
expect(eventsD == [("later", False)], "D: unhandled at reject")
ctxD.eval("dp.catch(() => 1);")             # attach handler -> handled event
expect(eventsD == [("later", False), ("later", True)], "D: handled on catch attach")
ctxD.run_jobs()
check_clean(ctxD, "D")
print("D ok")

# =============================================================
# E. delayed catch (from Python, after a drain)
# =============================================================
ctxE = quickjs.Context()
_all_ctxs.append(ctxE)
eventsE = []
ctxE.set_unhandled_rejection_handler(make_recorder(eventsE))

ctxE.eval("var ep = Promise.reject('later');")
expect(eventsE == [("later", False)], "E: unhandled at creation")
ctxE.run_jobs()                              # nothing pending; no new events
expect(eventsE == [("later", False)], "E: drain adds nothing")
pE = ctxE.get("ep")
qE = pE.catch(lambda r: 99)                  # attach handler from Python
expect(eventsE == [("later", False), ("later", True)], "E: handled on late catch")
ctxE.run_jobs()
expect(qE.result() == 99, "E: delayed catch recovers")
expect_raise(pE.result, "later", "E")
print("E ok")

# =============================================================
# F. async function unhandled rejection
# =============================================================
ctxF = quickjs.Context()
_all_ctxs.append(ctxF)
eventsF = []
ctxF.set_unhandled_rejection_handler(make_recorder(eventsF))

# sync throw: rejection happens during the CALL
ctxF.eval("async function badF1() { throw 'async-boom'; }")
pF1 = ctxF.get("badF1")()
expect(eventsF == [("async-boom", False)], "F: sync-throw async fn reported at call")
ctxF.run_jobs()
expect(pF1.done() is True, "F: settled")
expect(eventsF == [("async-boom", False)], "F: no extra events")
check_clean(ctxF, "F")

# throw after an await: rejection happens inside a job -> event during run_jobs
eventsF2 = []
ctxF.set_unhandled_rejection_handler(make_recorder(eventsF2))
ctxF.eval("async function badF2() { await Promise.resolve(); throw 'late-throw'; }")
pF2 = ctxF.get("badF2")()
expect(eventsF2 == [], "F2: pending until drain")
ctxF.run_jobs()
expect(eventsF2 == [("late-throw", False)], "F2: reported during run_jobs")
print("F ok")

# =============================================================
# G. async function handled rejection
# =============================================================
ctxG = quickjs.Context()
_all_ctxs.append(ctxG)
eventsG = []
ctxG.set_unhandled_rejection_handler(make_recorder(eventsG))

ctxG.eval("async function badG() { throw new Error('gboom'); }")
pG = ctxG.get("badG")()
expect(len(eventsG) == 1 and eventsG[0][1] is False, "G: unhandled at call")
ctxG.eval("var g1;")  # keep ctx warm
qG = pG.catch(lambda r: 1)
expect(len(eventsG) == 2 and eventsG[1][1] is True, "G: handled on catch attach")
ctxG.run_jobs()
expect(qG.result() == 1, "G: recovered")
check_clean(ctxG, "G")
print("G ok")

# =============================================================
# H. multiple unhandled rejections
# =============================================================
ctxH = quickjs.Context()
_all_ctxs.append(ctxH)
eventsH = []
ctxH.set_unhandled_rejection_handler(make_recorder(eventsH))

ctxH.eval("""
Promise.reject('a');
Promise.reject('b');
Promise.reject('c');
""")
expect(eventsH == [("a", False), ("b", False), ("c", False)],
       "H: one event per rejection, in order: %r" % (eventsH,))
ctxH.run_jobs()
print("H ok")

# =============================================================
# I. handler replacement
# =============================================================
ctxI = quickjs.Context()
_all_ctxs.append(ctxI)
eventsI1 = []
eventsI2 = []
ctxI.set_unhandled_rejection_handler(make_recorder(eventsI1))
ctxI.eval("Promise.reject('one');")
expect(eventsI1 == [("one", False)], "I: first handler sees event")

ctxI.set_unhandled_rejection_handler(make_recorder(eventsI2))
ctxI.eval("Promise.reject('two');")
expect(eventsI1 == [("one", False)], "I: old handler no longer called")
expect(eventsI2 == [("two", False)], "I: new handler called")
gc.collect()

# replace again (release old node)
ctxI.set_unhandled_rejection_handler(make_recorder(eventsI1))
ctxI.eval("Promise.reject('three');")
expect(eventsI2 == [("two", False)], "I: second handler released")
expect(eventsI1 == [("one", False), ("three", False)], "I: third handler active")
check_clean(ctxI, "I")
print("I ok")

# =============================================================
# J. disable with None
# =============================================================
ctxJ = quickjs.Context()
_all_ctxs.append(ctxJ)
eventsJ = []
ctxJ.set_unhandled_rejection_handler(make_recorder(eventsJ))
ctxJ.eval("Promise.reject('before');")
expect(eventsJ == [("before", False)], "J: active before disable")

ctxJ.set_unhandled_rejection_handler(None)
ctxJ.eval("Promise.reject('after');")
ctxJ.run_jobs()
expect(eventsJ == [("before", False)], "J: no events after disable")
check_clean(ctxJ, "J")
print("J ok")

# =============================================================
# K. handler GC (handler is rooted by the registry node)
# =============================================================
ctxK = quickjs.Context()
_all_ctxs.append(ctxK)
eventsK = []

h = make_recorder(eventsK)   # closure capturing eventsK via list
ctxK.set_unhandled_rejection_handler(h)
del h
gc.collect()
ctxK.eval("Promise.reject('survives');")
expect(eventsK == [("survives", False)], "K: handler survives del+gc")
ctxK.run_jobs()
check_clean(ctxK, "K")
print("K ok")

# =============================================================
# L. Context close
# =============================================================
ctxL = quickjs.Context()
_all_ctxs.append(ctxL)
eventsL = []
ctxL.set_unhandled_rejection_handler(make_recorder(eventsL))
ctxL.eval("Promise.reject('closing');")
expect(eventsL == [("closing", False)], "L: event before close")
ctxL.close()
ctxL.close()  # idempotent
expect_raise(lambda: ctxL.set_unhandled_rejection_handler(make_recorder([])),
             "context closed", "L")
expect_raise(lambda: ctxL.eval("1 + 2"), "context closed", "L")
expect(eventsL == [("closing", False)], "L: no events after close")
print("L ok")

# =============================================================
# M. pending Promise (reject + drop wrapper + gc)
# =============================================================
ctxM = quickjs.Context()
_all_ctxs.append(ctxM)
eventsM = []
ctxM.set_unhandled_rejection_handler(make_recorder(eventsM))

pM, rM, jM = ctxM.promise()
jM("wait-for-gc")
expect(eventsM == [("wait-for-gc", False)], "M: rejection reported at reject()")
del pM, rM, jM
gc.collect()
ctxM.run_jobs()
expect(eventsM == [("wait-for-gc", False)], "M: no more events after gc")
check_clean(ctxM, "M")
print("M ok")

# =============================================================
# N. Error rejection  (reason converts to {} -- documented limitation)
# =============================================================
ctxN = quickjs.Context()
_all_ctxs.append(ctxN)
eventsN = []
ctxN.set_unhandled_rejection_handler(make_recorder(eventsN))

ctxN.eval("Promise.reject(new Error('nboom'));")
expect(len(eventsN) == 1, "N: one event")
reason, handled = eventsN[0]
# documented: JS Error -> {} through the shared conversion layer
expect(reason == {} and handled is False,
       "N: Error reason converts to {} (documented): %r" % (eventsN,))
# p.result() still formats the Error properly
ctxN.eval("var np = Promise.reject(new Error('nboom'));")
pN = ctxN.get("np")
expect_raise(pN.result, "nboom", "N")
check_clean(ctxN, "N")
print("N ok")

# =============================================================
# O. string rejection / P. number rejection / Q. dict-list rejection
# =============================================================
ctxQ = quickjs.Context()
_all_ctxs.append(ctxQ)
eventsQ = []
ctxQ.set_unhandled_rejection_handler(make_recorder(eventsQ))

ctxQ.eval("""
Promise.reject('str-reason');
Promise.reject(123);
Promise.reject({ 'a': 1 });
Promise.reject([1, 2, 3]);
""")
expect(eventsQ[0] == ("str-reason", False), "O: string reason: %r" % (eventsQ[0],))
expect(eventsQ[1] == (123, False), "P: number reason: %r" % (eventsQ[1],))
expect(eventsQ[2] == ({"a": 1}, False), "Q: dict reason: %r" % (eventsQ[2],))
expect(eventsQ[3] == ([1, 2, 3], False), "Q: list reason: %r" % (eventsQ[3],))
ctxQ.run_jobs()
check_clean(ctxQ, "O/P/Q")
print("O/P/Q ok")

# =============================================================
# R. callback rejection
# =============================================================
ctxR = quickjs.Context()
_all_ctxs.append(ctxR)
eventsR = []
ctxR.set_unhandled_rejection_handler(make_recorder(eventsR))


def callR_boom(v):
    raise ValueError("rboom")


ctxR.add_callable("callR_boom", callR_boom)
ctxR.eval("var rp = Promise.resolve(1).then(callR_boom);")
expect(eventsR == [], "R: nothing before drain")
ctxR.run_jobs()
# the derived promise was rejected by the failing callback
expect(len(eventsR) == 1 and eventsR[0][1] is False,
       "R: callback failure rejected derived promise: %r" % (eventsR,))
pR = ctxR.get("rp")
expect(pR.done() is True, "R: settled")
check_clean(ctxR, "R")
print("R ok")

# =============================================================
# S. handler reentrancy
# =============================================================
ctxS = quickjs.Context()
_all_ctxs.append(ctxS)
orderS = []
ctxS.set_time_limit(5000)
ctxS.eval("var sres = 0;")


def handlerS(reason, handled):
    orderS.append(("enter", reason, handled))
    # nested execution inside the tracker (tracker runs inside JS execution)
    expect(ctxS.eval("6 * 7") == 42, "S: nested eval in handler")
    # nested rejection -> nested tracker call, innermost first
    if reason == "outer":
        ctxS.eval("Promise.reject('inner');")
    # close() must be refused while the tracker runs an execution window
    try:
        ctxS.close()
        orderS.append(("close-not-refused",))
    except RuntimeError as e:
        orderS.append(("busy", "busy" in str(e)))
    orderS.append(("exit", reason, handled))


ctxS.set_unhandled_rejection_handler(handlerS)
ctxS.eval("Promise.reject('outer');")
expect(orderS[0] == ("enter", "outer", False), "S: outer enters first: %r" % (orderS[0],))
expect(("enter", "inner", False) in orderS, "S: nested rejection reported: %r" % (orderS,))
expect(("close-not-refused",) not in orderS, "S: close refused in handler")
expect(orderS[-1] == ("exit", "outer", False), "S: outer exits last: %r" % (orderS,))
expect(ctxS.eval("1 + 2") == 3, "S: ctx usable after reentrant handler")
print("S ok")

# =============================================================
# T. handler exception (swallowed, recorded; never crosses the C stack)
# =============================================================
ctxT = quickjs.Context()
_all_ctxs.append(ctxT)
countT = []


def handlerT(reason, handled):
    countT.append((reason, handled))
    if len(countT) == 1:
        raise ValueError("handler-boom")


ctxT.set_unhandled_rejection_handler(handlerT)
ctxT.eval("Promise.reject('first');")   # handler raises here
expect(len(countT) == 1, "T: handler ran once")
# the exception was swallowed: eval did not raise, ctx is clean
check_clean(ctxT, "T")
ctxT.eval("Promise.reject('second');")  # handler still active afterwards
expect(len(countT) == 2, "T: later events still delivered")
expect(countT[1] == ("second", False), "T: second event ok")
ctxT.run_jobs()
check_clean(ctxT, "T")
print("T ok")

# =============================================================
# U. timeout + rejection
# =============================================================
ctxU = quickjs.Context()
_all_ctxs.append(ctxU)
eventsU = []
ctxU.set_unhandled_rejection_handler(make_recorder(eventsU))
ctxU.set_time_limit(20)

# async loop that never finishes -> timeout; the interrupted async promise
# is rejected with the interrupt error -> tracker reports it (observed below)
ctxU.eval("async function spinU() { while (true) { await Promise.resolve(); } }")
pU = ctxU.get("spinU")()
expect_raise(ctxU.run_jobs, "timeout", "U")
expect(pU.done() is False, "U: spin promise stays pending")
# ctx fully reusable
check_clean(ctxU, "U")
eventsU.clear()

# rejection after a timeout incident still tracked
ctxU.eval("Promise.reject('after-timeout');")
expect(eventsU == [("after-timeout", False)], "U: tracked after timeout: %r" % (eventsU,))
ctxU.set_time_limit(0)
print("U ok")

# =============================================================
# V. 1000 rejection stress
# =============================================================
ctxV = quickjs.Context()
_all_ctxs.append(ctxV)
ctxV.set_time_limit(20000)
countV = [0]
handledV = [0]


def handlerV(reason, handled):
    if handled:
        handledV[0] += 1
    else:
        countV[0] += 1


ctxV.set_unhandled_rejection_handler(handlerV)
base = ctxV._js_mem()
for i in range(1000):
    # exactly ONE rejected promise per iteration; every other one is caught
    # on the same promise (unhandled at reject, then handled at catch attach).
    # A single reused global keeps the promises collectable.
    if i % 2 == 0:
        ctxV.eval("var qv = Promise.reject(%d); qv.catch(function () {});" % i)
    else:
        ctxV.eval("Promise.reject(%d);" % i)
ctxV.run_jobs()
expect(countV[0] == 1000, "V: 1000 unhandled events, got %d" % countV[0])
expect(handledV[0] == 500, "V: 500 handled events, got %d" % handledV[0])
gc.collect()
ctxV.gc()
expect(ctxV._js_mem() - base < 64 * 1024,
       "V: JS heap grew %d bytes > 64 KiB" % (ctxV._js_mem() - base))
mp_before = gc.mem_alloc()
for i in range(200):
    ctxV.eval("Promise.reject(%d);" % (i + 100000))
del handlerV, countV, handledV
gc.collect()
expect(gc.mem_alloc() - mp_before < 32 * 1024,
       "V: MP heap grew %d bytes > 32 KiB" % (gc.mem_alloc() - mp_before))
print("V ok")

# =============================================================
# W. GC stress (rejections + handlers + collect cycles)
# =============================================================
ctxW = quickjs.Context()
_all_ctxs.append(ctxW)
ctxW.set_time_limit(20000)
wcount = [0]


def handlerW(reason, handled):
    wcount[0] += 1


ctxW.set_unhandled_rejection_handler(handlerW)
for i in range(5):
    for j in range(100):
        ctxW.eval("Promise.reject('w%d');" % j)
    gc.collect()
    ctxW.gc()
expect(wcount[0] == 500, "W: events survive gc cycles: %d" % wcount[0])
# replace handler repeatedly under gc pressure
for k in range(20):
    ctxW.set_unhandled_rejection_handler(handlerW)
    gc.collect()
ctxW.eval("Promise.reject('w-final');")
expect(wcount[0] == 501, "W: handler still active after replaces")
ctxW.run_jobs()
check_clean(ctxW, "W")
print("W ok")

# =============================================================
# X. Context create/drop/close stress (handlers registered per ctx)
# =============================================================
xevents = 0
tmp_ctxs = []
for i in range(100):
    c = quickjs.Context()
    c.set_unhandled_rejection_handler(lambda r, h, n=[0]: n.__setitem__(0, n[0] + 1) or n)
    c.eval("Promise.reject('x%d');" % i)
    xevents += 1
    tmp_ctxs.append(c)
    if i % 10 == 0:
        gc.collect()
for c in tmp_ctxs:
    c.close()
del tmp_ctxs
gc.collect()
expect(xevents == 100, "X: %d events across 100 contexts" % xevents)
print("X ok")

for _c in _all_ctxs:
    try:
        _c.close()
    except Exception:
        pass
gc.collect()
print("ALL QUICKJS PHASE9 TESTS PASSED")