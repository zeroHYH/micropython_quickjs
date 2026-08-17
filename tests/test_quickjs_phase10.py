# Phase 10 tests: final ownership / lifecycle / GC-order audit + stress.
#
# Run: micropython tests/test_quickjs_phase10.py
#
# Scope (per phase brief):
#   B.  Lifecycle attack matrix: Context/Function-wrapper/Promise/callback/
#       PromiseResolver GC ordering, alive/dead x close, reentrancy combos,
#       close-busy contract (close -> RuntimeError("context is busy") ->
#       state unchanged -> eval("1+2")==3 -> final close() succeeds),
#       timeout combos, OOM combos.
#   C.  Stress: 10000 x Context create/close, wrapper churn, promise
#       create/resolve/reject, callbacks, then/catch/finally, run_jobs,
#       rejection tracker events, mixed workload -- with gc.collect(),
#       ctx.gc(), JS_ComputeMemoryUsage (ctx._js_mem()), gc.mem_alloc()
#       bounds (NO RSS evidence).
#
# OOM-path defects fixed in this phase and regressed here:
#   D1 (BUG-1): ctx.promise() must not read/free uninitialized
#       resolving_funcs[] when JS_NewPromiseCapability fails at an early
#       engine OOM point (js_promise_new returns JS_EXCEPTION without
#       writing the array).  Regression: tiny memory limits -> promise
#       attempt either fails cleanly or succeeds; context stays usable,
#       close succeeds (ASAN build must stay clean).
#   D2 (BUG-2): quickjs_object_to_mp leaked the JSPropertyEnum props array
#       (+atoms) when mp_obj_new_dict raised MemoryError (LSan: 4,600,057 B
#       before the fix).  Regression: big JS object -> MemoryError ->
#       context still usable -> close; ASAN/LSan run must be clean.
#   D3 (BUG-3): quickjs_string_to_mp skipped JS_FreeCString on the MP-OOM
#       path (ref escape; immediate orphan for non-ASCII/wide strings).
#       Regression: wide-string conversion MemoryError -> usable + close.
#   D4 (BUG-6/7, contract fixes): argv m_new in call paths and the owned
#       JS_PromiseThen result must be released on every failure path; the
#       success paths (hundreds of wraps) are exercised here, failure paths
#       are covered by ASAN/LSan + DEBUG runs of the D2/D3 scenarios.
#
# Also re-verified here: no free of an entry while JS is executing, no
# double-free on close+wrapper-__del__ interleavings, entry table removal
# by __del__ after close is a no-op.

print("phase10 start")

import quickjs
import gc

_all_ctxs = []


def track(ctx):
    _all_ctxs.append(ctx)


def expect(cond, msg):
    if not cond:
        raise AssertionError(msg)


def expect_raise(fn, msg_contains, label):
    try:
        fn()
    except Exception as e:
        expect(msg_contains in str(e),
               "%s: exception %r lacks %r" % (label, e, msg_contains))
        return e
    raise AssertionError("%s: expected exception containing %r"
                         % (label, msg_contains))


def check_clean(ctx, label):
    expect(ctx.eval("1 + 2") == 3, "%s: context unusable after error" % label)


def busy_close_contract(ctx, label):
    # The full close-busy contract from the phase brief.
    expect_raise(lambda: ctx.close(), "context is busy", label)
    check_clean(ctx, label)
    ctx.close()
    expect_raise(lambda: ctx.eval("1 + 2"), "context closed", label)
    expect(ctx.close() is None, "%s: close idempotent after close" % label)


def gc_everything():
    gc.collect()


# =============================================================
# 1. Context x Function-wrapper GC ordering
# =============================================================
ctx1 = quickjs.Context()
track(ctx1)
ctx1.eval("function add1(a, b) { return a + b; }")
gc_everything()
w1 = ctx1.get("add1")
expect(callable(w1), "1: wrapper callable")
gc_everything()
expect(w1(2, 3) == 5, "1: wrapper works after gc")
# wrapper dead -> close
del w1
gc_everything()
ctx1.close()
gc_everything()
expect_raise(lambda: ctx1.get("add1"), "context closed", "1: after close")

# wrapper alive -> close -> wrapper __del__ runs later (no-op)
ctx1b = quickjs.Context()
track(ctx1b)
ctx1b.eval("function add1b(a, b) { return a + b; }")
w1b = ctx1b.get("add1b")
ctx1b.close()
gc_everything()
expect_raise(lambda: w1b(1, 2), "context closed", "1b: wrapper after close")
del w1b
gc_everything()

print("phase10 section 1 OK")

# =============================================================
# 2. Context x Promise wrapper GC ordering
# =============================================================
ctx2 = quickjs.Context()
track(ctx2)
p2 = ctx2.eval("Promise.resolve(7)")
expect(p2.result() == 7, "2: promise works")
gc_everything()
expect(p2.done() is True, "2: done checked")
# promise dead -> close
del p2
gc_everything()
ctx2.close()
gc_everything()

ctx2b = quickjs.Context()
track(ctx2b)
p2b = ctx2b.eval("Promise.reject('x2')")
ctx2b.close()
gc_everything()
expect_raise(lambda: p2b.result(), "context closed", "2b: promise after close")
del p2b
gc_everything()

print("phase10 section 2 OK")

# =============================================================
# 3. Context x callback GC ordering
# =============================================================
ctx3 = quickjs.Context()
track(ctx3)
calls3 = []


def cb3(v):
    calls3.append(v)
    return v * 2


ctx3.add_callable("cb3", cb3)
gc_everything()
expect(ctx3.eval("cb3(21)") == 42, "3: callback works after gc")
gc_everything()
# callback dead (Python side) -> JS closure still alive -> close
calls3 = None
del cb3
gc_everything()
ctx3.close()
gc_everything()

# callback alive -> close
ctx3b = quickjs.Context()
track(ctx3b)
ctx3b.add_callable("cb3b", lambda v: v + 1)
ctx3b.close()
gc_everything()
expect_raise(lambda: ctx3b.eval("cb3b(1)"), "context closed", "3b")

print("phase10 section 3 OK")

# =============================================================
# 4. Context x PromiseResolver GC ordering
# =============================================================
ctx4 = quickjs.Context()
track(ctx4)
p4, res4, rej4 = ctx4.promise()
gc_everything()
res4(11)
expect(ctx4.run_jobs() >= 0, "4: jobs pumped")
expect(p4.result() == 11, "4: resolved after gc")
gc_everything()
# resolver alive -> close
ctx4.close()
gc_everything()
expect_raise(lambda: p4.result(), "context closed", "4: promise after close")
del p4, res4, rej4
gc_everything()

ctx4b = quickjs.Context()
track(ctx4b)
p4b, res4b, rej4b = ctx4b.promise()
# resolver dead -> close
del res4b, rej4b
gc_everything()
ctx4b.close()
gc_everything()
del p4b
gc_everything()

print("phase10 section 4 OK")

# =============================================================
# 5. Reentrancy combos
# =============================================================
# 5a. callback -> eval -> callback
ctx5a = quickjs.Context()
track(ctx5a)
depth5 = []


def cb5a_inner(x):
    depth5.append(x)
    return x + 1


def cb5a_outer():
    # Python callback re-enters JS: eval that itself calls another callback
    return ctx5a.eval("cb5a_inner(40) + 1")


ctx5a.add_callable("cb5a_outer", cb5a_outer)
ctx5a.add_callable("cb5a_inner", cb5a_inner)
expect(ctx5a.eval("cb5a_outer()") == 42, "5a: callback->eval->callback")
expect(ctx5a.eval("1 + 2") == 3, "5a: clean after combo")
ctx5a.close()

# 5b. callback -> Promise -> callback (callback returns a Promise wrapper;
#     reaction job assimilates the thenable, then a JS then runs another cb)
ctx5b = quickjs.Context()
track(ctx5b)


def cb5b_mk():
    p, r, _ = ctx5b.promise()
    r(33)
    return p


ctx5b.add_callable("cb5b_mk", cb5b_mk)
ctx5b.eval("var q5b = Promise.resolve(1).then(function(){ return cb5b_mk(); });")
ctx5b.run_jobs()
q5b = ctx5b.get("q5b")
expect(q5b.result() == 33, "5b: callback -> promise -> JS then")
ctx5b.close()

# 5c. callback -> Promise.resolve -> then -> callback (full circle)
ctx5c = quickjs.Context()
track(ctx5c)
vals5c = []


def cb5c_h(v):
    vals5c.append(v)
    return v + 10


ctx5c.add_callable("cb5c_h", cb5c_h)
ctx5c.eval("var q5c = Promise.resolve(5).then(function(v){ return cb5c_h(v); });")
ctx5c.run_jobs()
q5c = ctx5c.get("q5c")
expect(vals5c == [5], "5c: handler saw 5")
expect(q5c.result() == 15, "5c: final value")
ctx5c.close()

# 5d. rejection tracker -> callback -> eval
ctx5d = quickjs.Context()
track(ctx5d)
ev5d = []


def h5d(reason, handled):
    ev5d.append((reason, handled))
    expect(ctx5d.eval("6 * 7") == 42, "5d: tracker re-enters eval")


ctx5d.set_unhandled_rejection_handler(h5d)
ctx5d.eval("Promise.reject('r5d')")
expect(ev5d == [("r5d", False)], "5d: rejection event")
ctx5d.close()

# 5e. rejection tracker -> callback -> reject (resolver inside tracker)
ctx5e = quickjs.Context()
track(ctx5e)
p5e, res5e, rej5e = ctx5e.promise()
ev5e = []


def h5e(reason, handled):
    ev5e.append((reason, handled))
    rej5e("inner")


ctx5e.set_unhandled_rejection_handler(h5e)
ctx5e.eval("Promise.reject('outer')")
ctx5e.run_jobs()
expect(ev5e == [("outer", False), ("inner", False)],
       "5e: events %r" % (ev5e,))
expect_raise(lambda: p5e.result(), "inner", "5e: inner rejection")
ctx5e.close()

print("phase10 section 5 OK")

# =============================================================
# 6. close() inside every reentrancy context must be "busy"
# =============================================================
# 6a. close inside a callback
ctx6a = quickjs.Context()
track(ctx6a)


def cb6a():
    try:
        ctx6a.close()
        return "no-error"
    except RuntimeError as e:
        return str(e)


ctx6a.add_callable("cb6a", cb6a)
expect(ctx6a.eval("cb6a()") == "context is busy", "6a: busy in callback")
check_clean(ctx6a, "6a")
ctx6a.close()

# 6b. close inside a promise handler (microtask)
ctx6b = quickjs.Context()
track(ctx6b)


def h6b(v):
    try:
        ctx6b.close()
        return "no-error"
    except RuntimeError as e:
        return str(e)


p6b = ctx6b.eval("Promise.resolve(1)")
q6b = p6b.then(h6b)
n = ctx6b.run_jobs()
expect(n >= 1, "6b: job ran")
expect(q6b.result() == "context is busy", "6b: busy in promise handler")
check_clean(ctx6b, "6b")
ctx6b.close()

# 6c. close inside a rejection handler (tracker) -- must not crash,
#     exception swallowed and recorded, context usable afterwards.
ctx6c = quickjs.Context()
track(ctx6c)


def h6c(reason, handled):
    try:
        ctx6c.close()
    except RuntimeError:
        pass


ctx6c.set_unhandled_rejection_handler(h6c)
ctx6c.eval("Promise.reject('r6c')")
check_clean(ctx6c, "6c")
ctx6c.close()

# 6d. close inside a job (JS then handler calling a Python callback)
ctx6d = quickjs.Context()
track(ctx6d)


def cb6d():
    try:
        ctx6d.close()
        return "no-error"
    except RuntimeError as e:
        return str(e)


ctx6d.add_callable("cb6d", cb6d)
ctx6d.eval("var q6d = Promise.resolve(1).then(function(){ return cb6d(); });")
ctx6d.run_jobs()
q6d = ctx6d.get("q6d")
expect(q6d.result() == "context is busy", "6d: busy in job")
check_clean(ctx6d, "6d")
ctx6d.close()

print("phase10 section 6 OK")

# =============================================================
# 7. Timeout combos
# =============================================================
# 7a. timeout + callback (callback runs inside an armed window; then the
#     JS side spins)
ctx7a = quickjs.Context()
track(ctx7a)
ctx7a.add_callable("cb7a", lambda: 42)
ctx7a.eval("function spin7a(){ for(;;){} } function go7a(){ cb7a(); spin7a(); }")
ctx7a.set_time_limit(100)
expect_raise(lambda: ctx7a.eval("go7a()"), "timeout", "7a: timeout after cb")
check_clean(ctx7a, "7a")
ctx7a.set_time_limit(0)
ctx7a.close()

# 7b. timeout + Promise (busy loop inside a microtask)
ctx7b = quickjs.Context()
track(ctx7b)
ctx7b.eval("var spin7b = Promise.resolve(1).then(function(){ for(;;){} });")
ctx7b.set_time_limit(100)
expect_raise(lambda: ctx7b.run_jobs(), "timeout", "7b: timeout in job")
check_clean(ctx7b, "7b")
ctx7b.set_time_limit(0)
ctx7b.close()

# 7c. timeout + run_jobs (same machinery via the pump)
ctx7c = quickjs.Context()
track(ctx7c)
ctx7c.eval("var spin7c = Promise.resolve(1).then(function(){ for(;;){} });")
ctx7c.set_time_limit(80)
expect_raise(lambda: ctx7c.run_jobs(), "timeout", "7c: timeout via run_jobs")
expect(ctx7c.run_jobs() == 0, "7c: queue drained after interrupt")
check_clean(ctx7c, "7c")
ctx7c.set_time_limit(0)
ctx7c.close()

# 7d. timeout + nested eval (callback performs an inner eval that spins;
#     the inner window inherits the outer deadline budget)
ctx7d = quickjs.Context()
track(ctx7d)


def cb7d():
    return ctx7d.eval("for(;;){}")


ctx7d.add_callable("cb7d", cb7d)
ctx7d.eval("function go7d(){ return cb7d(); }")
ctx7d.set_time_limit(200)
expect_raise(lambda: ctx7d.eval("go7d()"), "timeout", "7d: nested timeout")
check_clean(ctx7d, "7d")
ctx7d.set_time_limit(0)
ctx7d.close()

print("phase10 section 7 OK")

# =============================================================
# 8. OOM combos
# =============================================================
# 8a. OOM + conversion (regression D2: props array must not leak;
#     LSan asserted externally on the ASAN run)
ctx8a = quickjs.Context()
track(ctx8a)
ctx8a.set_memory_limit(0)
ctx8a.eval("var big8a = {}; for (var i = 0; i < 300000; i++) big8a['k' + i] = i; 1;")
def _expect_memerr(fn, label):
    try:
        fn()
    except MemoryError:
        return
    except Exception as e:
        raise AssertionError("%s: unexpected %r" % (label, e))
    raise AssertionError("%s: expected MemoryError" % label)


_expect_memerr(lambda: ctx8a.get("big8a"), "8a: dict OOM")
check_clean(ctx8a, "8a")
ctx8a.close()

# 8b. OOM + conversion of a wide string (regression D3: JS_FreeCString
#     must run on the MP-OOM path)
ctx8b = quickjs.Context()
track(ctx8b)
ctx8b.set_memory_limit(0)
ctx8b.eval("var w8b = ''; for (var i = 0; i < 750000; i++) w8b += '\\u597d'; 1;")
_expect_memerr(lambda: ctx8b.get("w8b"), "8b: string OOM")
check_clean(ctx8b, "8b")
ctx8b.close()

# 8c. OOM + callback (JS memory limit high enough for tiny code, low
#     enough to blow up a big allocation)
ctx8c = quickjs.Context()
track(ctx8c)
calls8c = []


def cb8c():
    calls8c.append(1)
    return 1


ctx8c.add_callable("cb8c", cb8c)
ctx8c.set_memory_limit(200000)
ctx8c.eval("cb8c()")
expect(len(calls8c) == 1, "8c: callback ran")
expect_raise(lambda: ctx8c.eval(
    "var b=[]; for (var i=0;i<100000;i++) b.push(i);"), "out of memory",
    "8c: OOM after callback")
check_clean(ctx8c, "8c")
ctx8c.set_memory_limit(0)
ctx8c.close()

# 8d. OOM + Promise (huge allocation inside a microtask; the engine turns
#     the uncatchable OOM into a rejection of the derived promise)
ctx8d = quickjs.Context()
track(ctx8d)
ctx8d.set_memory_limit(200000)
ctx8d.eval("var q8d = Promise.resolve(1).then(function(){ "
           "var b=[]; for (var i=0;i<100000;i++) b.push(i); return b.length; });")
expect(ctx8d.run_jobs() >= 1, "8d: job ran")
q8d = ctx8d.get("q8d")
expect(q8d.done() is True, "8d: derived promise settled")
expect_raise(lambda: q8d.result(), "out of memory", "8d: OOM as rejection")
check_clean(ctx8d, "8d")
ctx8d.set_memory_limit(0)
ctx8d.close()

print("phase10 section 8 OK")

# =============================================================
# 9. Regression D1 (BUG-1): ctx.promise() under tiny memory limits.
#    js_promise_new can fail before writing resolving_funcs; the module
#    must not read/free uninitialized stack slots (UB).  Either outcome
#    (clean failure or success) is acceptable; a crash / ASAN report is
#    the regression being guarded.
# =============================================================
for lim in (512, 2048, 8192, 32768, 65536, 131072):
    ctx9 = quickjs.Context()
    track(ctx9)
    ctx9.set_memory_limit(lim)
    try:
        pr, rr, jr = ctx9.promise()
        expect(pr is not None, "9: promise created at limit %d" % lim)
    except RuntimeError:
        pass  # acceptable: JS_NewPromiseCapability OOM
    except Exception as e:
        raise AssertionError("9: unexpected %r at limit %d" % (e, lim))
    ctx9.close()
print("phase10 section 9 OK")

# =============================================================
# 10. Entry-table double-removal safety: del wrapper (entry removed),
#     then close (must not double-free); and close then del wrapper.
# =============================================================
ctx10 = quickjs.Context()
track(ctx10)
ctx10.eval("function f10(){ return 1; }")
for _ in range(20):
    w = ctx10.get("f10")
    del w
    gc_everything()
ctx10.close()
gc_everything()

ctx10b = quickjs.Context()
track(ctx10b)
ctx10b.eval("function f10b(){ return 2; }")
w10b = ctx10b.get("f10b")
del w10b
gc_everything()
w10c = ctx10b.get("f10b")  # re-create wrapper for same token? no: new token
expect(w10c() == 2, "10b: second wrapper works")
ctx10b.close()
gc_everything()

print("phase10 section 10 OK")

# =============================================================
# C. Stress (10000 each), with gc.collect / ctx.gc / JS_ComputeMemoryUsage
#    / gc.mem_alloc bounds -- RSS is never used as evidence.
# =============================================================

# 11. 10000 Context create/close
mp_before = gc.mem_alloc()
for i in range(10000):
    c = quickjs.Context()
    c.close()
    if i % 1000 == 0:
        gc.collect()
gc.collect()  # sweep everything before measuring (no RSS evidence)
expect(gc.mem_alloc() - mp_before < 64 * 1024,
       "11: MP heap drifted %d bytes" % (gc.mem_alloc() - mp_before))
print("phase10 section 11 OK")

# 12. 10000 wrapper create/drop
ctx12 = quickjs.Context()
track(ctx12)
ctx12.eval("function add12(a, b) { return a + b; }")
js_base12 = ctx12._js_mem()
mp_base12 = gc.mem_alloc()
for i in range(10000):
    w = ctx12.get("add12")
    expect(w(1, 2) == 3, "12: wrapper %d" % i)
    del w
    if i % 1000 == 0:
        gc.collect()
        ctx12.gc()
gc.collect()
ctx12.gc()
expect(ctx12._js_mem() - js_base12 < 128 * 1024,
       "12: JS heap grew %d" % (ctx12._js_mem() - js_base12))
expect(gc.mem_alloc() - mp_base12 < 64 * 1024,
       "12: MP heap grew %d" % (gc.mem_alloc() - mp_base12))
ctx12.close()
print("phase10 section 12 OK")

# 13. 10000 Promise create/resolve
ctx13 = quickjs.Context()
track(ctx13)
js_base13 = ctx13._js_mem()
for i in range(10000):
    p, r, j = ctx13.promise()
    r(i % 1000)
    del p, r, j
    if i % 1000 == 0:
        gc.collect()
        ctx13.gc()
ctx13.run_jobs()
gc.collect()
ctx13.gc()
expect(ctx13._js_mem() - js_base13 < 128 * 1024,
       "13: JS heap grew %d" % (ctx13._js_mem() - js_base13))
ctx13.close()
print("phase10 section 13 OK")

# 14. 10000 Promise reject (no tracker -> no events; all become
#     unhandled diagnostics only if a tracker is set; assert none here)
ctx14 = quickjs.Context()
track(ctx14)
for i in range(10000):
    p, r, j = ctx14.promise()
    j("boom%d" % i)
    # keep every other promise referenced (like phase9 V) or drop:
    del p, r, j
    if i % 1000 == 0:
        gc.collect()
ctx14.run_jobs()
ctx14.close()
print("phase10 section 14 OK")

# 15. 10000 callbacks (JS loop calling a Python callback)
ctx15 = quickjs.Context()
track(ctx15)
cnt15 = [0]


def cb15():
    cnt15[0] += 1
    return cnt15[0]


ctx15.add_callable("cb15", cb15)
ctx15.eval("var r15 = 0; for (var i = 0; i < 10000; i++) { r15 += cb15(); }")
expect(ctx15.eval("r15") == 50005000, "15: callback aggregate")
expect(cnt15[0] == 10000, "15: callback count")
ctx15.close()
print("phase10 section 15 OK")

# 16. 10000 then/catch/finally chains (wrapper + closure churn)
ctx16 = quickjs.Context()
track(ctx16)
js_base16 = ctx16._js_mem()
mp_base16 = gc.mem_alloc()
# NOTE: the JS heap has a hard cap (QUICKJS_DEFAULT_MEMORY_LIMIT,
# overridden to 8 MiB on the Make-style Unix port).  Piling up
# undrained promise chains eventually exhausts it (engine then throws
# JS_NULL as its OOM fallback, surfaced as RuntimeError "null"); this
# is the intended guard, so the churn loop drains the job queue
# periodically (and the queue is fully pumped after the loop).
for i in range(10000):
    p = ctx16.eval("Promise.resolve(%d)" % (i % 7))
    p = p.then(lambda x: x + 1)
    p = p.catch(lambda e: -1)
    p = p.finally_(lambda: None)
    if i % 500 == 0:
        ctx16.run_jobs()
        gc.collect()
        ctx16.gc()
ctx16.run_jobs()
gc.collect()
ctx16.gc()
expect(ctx16._js_mem() - js_base16 < 256 * 1024,
       "16: JS heap grew %d" % (ctx16._js_mem() - js_base16))
expect(gc.mem_alloc() - mp_base16 < 128 * 1024,
       "16: MP heap grew %d" % (gc.mem_alloc() - mp_base16))
ctx16.close()
print("phase10 section 16 OK")

# 17. 10000 run_jobs (10000 queued microtasks, pumped once)
ctx17 = quickjs.Context()
track(ctx17)
ctx17.eval(
    "globalThis.acc17 = 0;"
    "for (var i = 0; i < 10000; i++) {"
    "  Promise.resolve(i).then(function(v){ globalThis.acc17 += v; });"
    "}")
n17 = ctx17.run_jobs()
expect(n17 == 10000, "17: pumped %d jobs" % n17)
expect(ctx17.eval("acc17") == sum(range(10000)), "17: acc17 total")
ctx17.close()
print("phase10 section 17 OK")

# 18. 10000 rejection tracker events
ctx18 = quickjs.Context()
track(ctx18)
ev18 = []


def h18(reason, handled):
    ev18.append(handled)


ctx18.set_unhandled_rejection_handler(h18)
for i in range(10000):
    if i % 2 == 0:
        ctx18.eval("var q18 = Promise.reject(%d); q18.catch(function(){});" % i)
    else:
        ctx18.eval("Promise.reject(%d);" % i)
    if i % 1000 == 0:
        gc.collect()
expect(len(ev18) == 15000, "18: expected 15000 events, got %d" % len(ev18))
expect(ev18.count(False) == 10000 and ev18.count(True) == 5000,
       "18: event mix")
ctx18.close()
print("phase10 section 18 OK")

# 19. Mixed stress: interleave everything, with all evidence channels.
ctx19 = quickjs.Context()
track(ctx19)
ctx19.add_callable("cb19", lambda v: v + 1)
js_base19 = ctx19._js_mem()
mp_base19 = gc.mem_alloc()
for i in range(10000):
    if i % 4 == 0:
        w = ctx19.get("cb19")
        expect(w(40) == 41, "19: wrapper %d" % i)
    if i % 4 == 1:
        p, r, j = ctx19.promise()
        r(1)
    if i % 4 == 2:
        q = ctx19.eval("Promise.resolve(%d)" % (i % 5)).then(lambda x: x * 2)
    if i % 4 == 3:
        ctx19.eval("cb19(%d);" % i)
    if i % 500 == 0:
        gc.collect()
        ctx19.gc()
        expect(ctx19.eval("1 + 2") == 3, "19: clean at %d" % i)
ctx19.run_jobs()
expect(ctx19.eval("1 + 2") == 3, "19: clean at end")
gc.collect()
ctx19.gc()
expect(ctx19._js_mem() - js_base19 < 256 * 1024,
       "19: JS heap grew %d" % (ctx19._js_mem() - js_base19))
expect(gc.mem_alloc() - mp_base19 < 128 * 1024,
       "19: MP heap grew %d" % (gc.mem_alloc() - mp_base19))
ctx19.close()
print("phase10 section 19 OK")

# 20. Lifecycle stress: context churn with wrappers/promises alive at close
for k in range(2000):
    c = quickjs.Context()
    c.eval("function f20(){ return 1; }")
    w = c.get("f20")
    p = c.eval("Promise.resolve(2)")
    p2, r2, j2 = c.promise()
    r2(3)
    c.close()          # close with wrappers/promise/resolver still alive
    del w, p, p2, r2, j2
    if k % 500 == 0:
        gc.collect()

# 21. Regression (BUG-8): promise-handler closures must not leak their MP
#     callback nodes into the registry.  Before the fix each closure kept a
#     ~96 B node alive until ctx.close() (measured 96/192/288 B per chain
#     for 1/2/3 handlers); the enter-time reap reclaims them, so the MP
#     heap must stay flat across handler churn.
ctx21 = quickjs.Context()
track(ctx21)
mp_base21 = gc.mem_alloc()
for i in range(3000):
    p = ctx21.eval("Promise.resolve(%d)" % (i % 7))
    p = p.then(lambda x: x + 1)
    p = p.catch(lambda e: -1)
    p = p.finally_(lambda: None)
    if i % 500 == 0:
        ctx21.run_jobs()
        gc.collect()
        ctx21.gc()
ctx21.run_jobs()
gc.collect()
ctx21.gc()
gc.collect()
expect(gc.mem_alloc() - mp_base21 < 32 * 1024,
       "21: MP heap grew %d across handler churn" % (gc.mem_alloc() - mp_base21))
ctx21.close()
print("phase10 section 21 OK")

# 22. JS heap hard cap (documented limitation): piling up undrained promise
#     chains runs into QUICKJS_DEFAULT_MEMORY_LIMIT; an allocation failure
#     surfaces as some RuntimeError (possibly "null" -- the engine's
#     JS_ThrowError2 OOM fallback value), the context must stay usable, and
#     raising the limit back to unlimited must restore full operation.
ctx22 = quickjs.Context()
track(ctx22)
ctx22.set_memory_limit(256 * 1024)
raised = False
for i in range(2000):
    try:
        p = ctx22.eval("Promise.resolve(1)")
        p = p.then(lambda x: x + 1)
        p = p.finally_(lambda: None)
    except RuntimeError:
        raised = True
        break
    except Exception as e:
        raise AssertionError("22: unexpected %r" % (e,))
expect(raised, "22: heap cap must eventually trip")
# while the cap is tight, further JS compilation may still OOM (expected);
# the contract is that LIFTING the cap restores full operation:
ctx22.set_memory_limit(0)
check_clean(ctx22, "22")
p22 = ctx22.eval("Promise.resolve(42)")
q22 = p22.then(lambda x: x)
ctx22.run_jobs()
expect(q22.result() == 42, "22: full operation restored after lifting cap")
ctx22.close()
print("phase10 section 22 OK")

print("ALL QUICKJS PHASE10 TESTS PASSED")