# Phase 11 tests: final release hardening.
#
# Run: micropython tests/test_quickjs_phase11.py
#
# B. Error-model matrix: every error class surfaces as a documented
#    MicroPython exception, the Context stays usable afterwards
#    (ctx.eval("1+2") == 3), the next call sees no stale JS pending
#    exception, and the error type is stable across repeats.  (JS
#    exceptions -> RuntimeError; Symbol / BigInt-out-of-range and
#    conversion errors -> TypeError; timeouts -> RuntimeError
#    "JavaScript execution timeout"; heap cap -> RuntimeError
#    "InternalError: out of memory"; busy close -> RuntimeError
#    "context is busy"; closed -> RuntimeError "context closed".)
#
# C. Context lifecycle model (final set):
#    1. del ctx            -> wrapper stays alive and usable (strong refs)
#    2. del ctx            -> Promise stays alive and usable
#    3. del ctx            -> resolver stays alive and usable
#    4. del callable       -> JS closure stays alive and callable
#    5. del wrapper        -> its entry is reclaimed (no stale token use)
#    6. close()            -> every wrapper/Context lookup fails cleanly
#    7. GC in arbitrary order -> no assertion / no crash
#    8. close in arbitrary order -> idempotent, safe
#    9. double close       -> no-op
#   10. busy close         -> RuntimeError("context is busy"), state intact
#    No reliance on finaliser execution order.
#
# E. Final stress: 10000x each of Contexts, function wrappers, promises,
#    resolvers, callbacks, then/catch/finally, rejections, rejection
#    tracker events, run_jobs; then a 10000x mixed chain
#    (Context -> callback -> Promise -> then -> callback -> rejection ->
#    tracker -> nested eval -> GC -> close), with gc.collect()/ctx.gc()
#    interleaved and gc.mem_alloc()/gc.mem_free()/ctx._js_mem() bounds
#    (never RSS).

print("phase11 start")

import quickjs
import gc

_all_ctxs = []


def track(ctx):
    _all_ctxs.append(ctx)


def expect(cond, msg):
    if not cond:
        raise AssertionError(msg)


def expect_raise(fn, exc_type, msg_contains, label):
    try:
        fn()
    except exc_type as e:
        expect(msg_contains in str(e),
               "%s: %r lacks %r" % (label, e, msg_contains))
        return e
    except Exception as e:
        raise AssertionError("%s: expected %s containing %r, got %r"
                             % (label, exc_type.__name__, msg_contains, e))
    raise AssertionError("%s: expected %s containing %r"
                         % (label, exc_type.__name__, msg_contains))


def check_clean(ctx, label):
    expect(ctx.eval("1 + 2") == 3, "%s: context unusable after error" % label)


# =============================================================
# B. Error-model matrix
# =============================================================
ctxB = quickjs.Context()
track(ctxB)
ctxB.eval("""
function h() { throw new Error('boom'); }
function ht() { throw new TypeError('badtype'); }
function hs() { throw 'str err'; }
function hn() { throw 42; }
function hl() { throw null; }
function spinB() { for(;;){} }
""")

# 1. JS SyntaxError -> RuntimeError, usable after
expect_raise(lambda: ctxB.eval("var ="), RuntimeError, "SyntaxError", "B1")
check_clean(ctxB, "B1")

# 2. JS Error / 3. TypeError / 4. throw string / 5. throw number / 6. null
expect_raise(lambda: ctxB.eval("h()"), RuntimeError, "boom", "B2")
check_clean(ctxB, "B2")
expect_raise(lambda: ctxB.eval("ht()"), RuntimeError, "badtype", "B3")
check_clean(ctxB, "B3")
expect_raise(lambda: ctxB.eval("hs()"), RuntimeError, "str err", "B4")
check_clean(ctxB, "B4")
expect_raise(lambda: ctxB.eval("hn()"), RuntimeError, "42", "B5")
check_clean(ctxB, "B5")
expect_raise(lambda: ctxB.eval("hl()"), RuntimeError, "null", "B6")
check_clean(ctxB, "B6")

# 7. Promise rejection surfaced by result()
pB = ctxB.eval("Promise.reject('rej reason')")
expect_raise(lambda: pB.result(), RuntimeError, "rej reason", "B7")
check_clean(ctxB, "B7")

# 8. callback Python exception -> JS-style TypeError, no C-stack crossing
def cb_bad():
    raise ValueError("py exc")


ctxB.add_callable("cb_bad", cb_bad)
expect_raise(lambda: ctxB.eval("cb_bad()"), RuntimeError,
             "ValueError: py exc", "B8")
check_clean(ctxB, "B8")

# 9. MP -> JS conversion error (huge int -> number)
expect_raise(lambda: ctxB.set("huge", 10**300), RuntimeError,
             "TypeError", "B9")
check_clean(ctxB, "B9")

# 10. JS -> MP conversion error (BigInt beyond int64)
ctxB.set("big", quickjs.bigint(1 << 70))
expect_raise(lambda: ctxB.get("big"), TypeError, "out of range", "B10")
check_clean(ctxB, "B10")

# 11. timeout -> RuntimeError, then context reusable
ctxB.set_time_limit(50)
expect_raise(lambda: ctxB.eval("spinB()"), RuntimeError,
             "timeout", "B11")
ctxB.set_time_limit(0)
check_clean(ctxB, "B11")

# 12. OOM (JS heap cap) -> RuntimeError, then reusable after releasing cap
ctxB.set_memory_limit(200000)
expect_raise(lambda: ctxB.eval(
    "var bb=[]; for (var i=0;i<100000;i++) bb.push(i);"), RuntimeError,
    "out of memory", "B12")
ctxB.set_memory_limit(0)
check_clean(ctxB, "B12")

# 13. busy close (close inside a callback)
def cb_close():
    try:
        ctxB.close()
        return "no-error"
    except RuntimeError as e:
        return str(e)


ctxB.add_callable("cb_close", cb_close)
expect(ctxB.eval("cb_close()") == "context is busy", "B13: busy close")
check_clean(ctxB, "B13")

# 14. context closed
ctxB.close()
expect_raise(lambda: ctxB.eval("1 + 2"), RuntimeError, "context closed", "B14")
expect_raise(lambda: ctxB.get("x"), RuntimeError, "context closed", "B14b")
expect(ctxB.close() is None, "B14c: close idempotent after close")

# 15./16. cross-context Function / Promise
c2 = quickjs.Context()
track(c2)
c2.eval("function f2(){ return 1; }")
w2 = c2.get("f2")
p2 = c2.eval("Promise.resolve(1)")
c3 = quickjs.Context()
track(c3)
expect_raise(lambda: c3.set("f", w2), RuntimeError,
             "belongs to another context", "B15")
expect_raise(lambda: c3.set("p", p2), RuntimeError,
             "belongs to another context", "B16")
check_clean(c3, "B15/16")
c2.close()
c3.close()

# 17. unsupported JS type (Symbol)
c4 = quickjs.Context()
track(c4)
c4.eval("var sym4 = Symbol('s');")
expect_raise(lambda: c4.get("sym4"), TypeError, "unsupported", "B17")
check_clean(c4, "B17")
c4.close()

# 18. unsupported Python type (complex)
c5 = quickjs.Context()
track(c5)
expect_raise(lambda: c5.set("c", 1 + 2j), RuntimeError, "TypeError", "B18")
check_clean(c5, "B18")
c5.close()

print("phase11 B (error matrix) OK")

# =============================================================
# C. Context lifecycle model
# =============================================================
# C1. del ctx -> wrapper stays alive (strong ref)
ctxC = quickjs.Context()
ctxC.eval("function addC(a, b) { return a + b; }")
wC = ctxC.get("addC")
del ctxC
gc.collect()
expect(wC(2, 3) == 5, "C1: wrapper alive after del ctx")
# close it through the remaining ref? no API for that; del wrapper
del wC
gc.collect()

# C2. del ctx -> Promise alive
ctxC2 = quickjs.Context()
pC2 = ctxC2.eval("Promise.resolve(7)")
del ctxC2
gc.collect()
expect(pC2.result() == 7, "C2: promise alive after del ctx")
del pC2
gc.collect()

# C3. del ctx -> resolver alive
ctxC3 = quickjs.Context()
pC3, resC3, rejC3 = ctxC3.promise()
del ctxC3
gc.collect()
resC3(9)
expect(pC3.result() == 9, "C3: resolver alive after del ctx")
del pC3, resC3, rejC3
gc.collect()

# C4. del callable -> JS closure stays alive and callable
ctxC4 = quickjs.Context()


def cbC4(x):
    return x + 1


ctxC4.add_callable("cbC4", cbC4)
ctxC4.eval("function useC4(v){ return cbC4(v); }")
del cbC4
gc.collect()
expect(ctxC4.eval("useC4(10)") == 11, "C4: closure alive after del callable")
ctxC4.close()

# C5. del wrapper -> entry reclaimed (new wrapper still works, no stale token)
ctxC5 = quickjs.Context()
ctxC5.eval("function f5(){ return 5; }")
for _ in range(10):
    w = ctxC5.get("f5")
    expect(w() == 5, "C5: wrapper %d" % _)
    del w
    gc.collect()
ctxC5.close()

# C6. close -> all lookups fail cleanly
ctxC6 = quickjs.Context()
ctxC6.eval("function f6(){ return 6; }")
w6 = ctxC6.get("f6")
p6 = ctxC6.eval("Promise.resolve(6)")
r6 = ctxC6.promise()
ctxC6.close()
expect_raise(lambda: w6(), RuntimeError, "context closed", "C6")
expect_raise(lambda: p6.result(), RuntimeError, "context closed", "C6b")
expect_raise(lambda: r6[1](1), RuntimeError, "context closed", "C6c")
del w6, p6, r6
gc.collect()

# C7. GC in arbitrary order (mixed alive objects)
ctxC7 = quickjs.Context()
ctxC7.eval("function f7(a){ return a; }")
objs = []
for _ in range(30):
    objs.append(ctxC7.get("f7"))
    objs.append(ctxC7.eval("Promise.resolve(%d)" % _))
    objs.append(ctxC7.promise())
for i in sorted(range(0, len(objs), 3), reverse=True):
    del objs[i]
    gc.collect()
ctxC7.close()
del objs
gc.collect()

# C8/C9. close in arbitrary order, double close
ctxC8 = quickjs.Context()
ctxC8.close()
ctxC8.close()  # double close: no-op
gc.collect()
expect(ctxC8.close() is None, "C9: third close also no-op")

# C10. busy close
ctxC10 = quickjs.Context()
b10 = []


def cbC10():
    b10.append("called")
    try:
        ctxC10.close()
        b10.append("closed-ok")
    except RuntimeError:
        b10.append("busy")


ctxC10.add_callable("cbC10", cbC10)
ctxC10.eval("cbC10()")
expect(b10 == ["called", "busy"], "C10: busy close inside callback: %r" % (b10,))
check_clean(ctxC10, "C10")
ctxC10.close()

print("phase11 C (lifecycle model) OK")

# =============================================================
# E. Final stress: 10000x each, then a 10000x mixed chain.
# Memory evidence: gc.mem_alloc / gc.mem_free / ctx._js_mem (no RSS).
# =============================================================

# E1. 10000 Context create/close
b = gc.mem_alloc()
for i in range(10000):
    c = quickjs.Context()
    c.close()
    if i % 1000 == 0:
        gc.collect()
gc.collect()
expect(gc.mem_alloc() - b < 64 * 1024,
       "E1: MP drift %d" % (gc.mem_alloc() - b))

# E2. 10000 function wrappers
cE = quickjs.Context()
track(cE)
cE.eval("function fE(a, b) { return a + b; }")
jb = cE._js_mem()
mb = gc.mem_alloc()
for i in range(10000):
    w = cE.get("fE")
    expect(w(1, 2) == 3, "E2: wrapper %d" % i)
    del w
    if i % 1000 == 0:
        gc.collect()
        cE.gc()
gc.collect()
cE.gc()
expect(cE._js_mem() - jb < 128 * 1024, "E2: JS drift %d" % (cE._js_mem() - jb))
expect(gc.mem_alloc() - mb < 64 * 1024,
       "E2: MP drift %d" % (gc.mem_alloc() - mb))

# E3. 10000 promises + E4. resolvers
for i in range(10000):
    p, r, j = cE.promise()
    r(i % 100)
    del p, r, j
    if i % 1000 == 0:
        gc.collect()
        cE.gc()
cE.run_jobs()
gc.collect()
cE.gc()
expect(cE._js_mem() - jb < 192 * 1024, "E3/4: JS drift %d" % (cE._js_mem() - jb))

# E5. 10000 callbacks
cnt = [0]


def cbE():
    cnt[0] += 1
    return cnt[0]


cE.add_callable("cbE", cbE)
cE.eval("var accE = 0; for (var i = 0; i < 10000; i++) { accE += cbE(); }")
expect(cE.eval("accE") == 50005000, "E5: callback total")

# E6. 10000 then/catch/finally (drain periodically; see phase10 note on
#     the JS heap hard cap)
for i in range(10000):
    p = cE.eval("Promise.resolve(%d)" % (i % 7))
    p = p.then(lambda x: x + 1)
    p = p.catch(lambda e: -1)
    p = p.finally_(lambda: None)
    if i % 500 == 0:
        cE.run_jobs()
        gc.collect()
        cE.gc()
cE.run_jobs()
gc.collect()
cE.gc()
expect(cE._js_mem() - jb < 256 * 1024, "E6: JS drift %d" % (cE._js_mem() - jb))

# E7. 10000 rejections (no tracker -> silent; done()/catch sanity)
for i in range(10000):
    p = cE.eval("Promise.reject(%d);" % i)
    del p
    if i % 1000 == 0:
        cE.run_jobs()
        gc.collect()
cE.run_jobs()

# E8. 10000 rejection tracker events
ev = []


def hE(reason, handled):
    ev.append(handled)


cE.set_unhandled_rejection_handler(hE)
for i in range(10000):
    cE.eval("Promise.reject(%d);" % i)
    if i % 1000 == 0:
        gc.collect()
expect(len(ev) == 10000 and ev.count(False) == 10000,
       "E8: tracker events %d" % len(ev))
cE.set_unhandled_rejection_handler(None)

# E9. 10000 run_jobs
cE.eval("globalThis.accJ = 0;"
        "for (var i = 0; i < 10000; i++) {"
        "  Promise.resolve(i).then(function(v){ globalThis.accJ += v; });"
        "}")
n = cE.run_jobs()
expect(n == 10000, "E9: pumped %d" % n)
expect(cE.eval("accJ") == sum(range(10000)), "E9: total")
cE.close()

# E10. mixed chain, 10000x:
#   Context -> callback -> Promise -> then -> callback -> rejection ->
#   tracker -> nested eval -> GC -> close
def mixed_chain():
    c = quickjs.Context()
    seen = []

    def cb1(v):
        seen.append("cb1")
        return v + 1

    c.add_callable("cb1", cb1)
    c.eval("function big(v){ return v * 2; }")
    # callback -> Promise -> then -> callback
    c.eval("var q = Promise.resolve(1).then(function(v){ return cb1(v); });")
    c.run_jobs()
    expect(c.eval("q") is not None, "E10: chain promise")
    # rejection -> tracker
    evt = []
    c.set_unhandled_rejection_handler(lambda r, h: evt.append(h))
    c.eval("Promise.reject('x');")
    expect(len(evt) == 1, "E10: tracker event")
    # nested eval
    expect(c.eval("big(cb1(3))") == 8, "E10: nested eval")
    # GC
    gc.collect()
    c.gc()
    # close
    c.close()


mb = gc.mem_alloc()
for i in range(10000):
    mixed_chain()
    if i % 500 == 0:
        gc.collect()
gc.collect()
expect(gc.mem_alloc() - mb < 64 * 1024,
       "E10: mixed-chain MP drift %d" % (gc.mem_alloc() - mb))

print("ALL QUICKJS PHASE11 TESTS PASSED")