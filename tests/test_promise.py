# MicroPython QuickJS - Promise & Async Tests
#
# Tests asynchronous JavaScript execution features:
# - Promise wrapper inspection (`p.done()`, `p.result()`)
# - Job queue execution (`ctx.run_jobs()`, `ctx.has_pending_jobs()`)
# - Combinators & reactions (`p.then()`, `p.catch()`, `p.finally_()`)
# - Python-side Promise creation (`ctx.promise()`, `resolve`, `reject`)
# - Async / Await execution and assimilation
# - Unhandled rejection tracker (`ctx.set_unhandled_rejection_handler()`)

import quickjs


def expect(cond, msg="Assertion failed"):
    if not cond:
        raise AssertionError(msg)


def expect_raise(fn, exc_type, msg_contains, label=""):
    try:
        fn()
    except exc_type as e:
        expect(msg_contains in str(e), f"{label}: {e!r} lacks {msg_contains!r}")
        return e
    except Exception as e:
        raise AssertionError(f"{label}: expected {exc_type.__name__}, got {e!r}") from e
    raise AssertionError(
        f"{label}: expected exception {exc_type.__name__} containing {msg_contains!r}"
    )


print("=== Running test_promise.py ===")

ctx = quickjs.Context()

# =============================================================
# 1. Promise Basics & Job Queue
# =============================================================
# Fulfilled promise
p_resolved = ctx.eval("Promise.resolve(42)")
assert p_resolved.done() is True
assert p_resolved.result() == 42

# Rejected promise
p_rejected = ctx.eval("Promise.reject(new Error('init failure'))")
assert p_rejected.done() is True
expect_raise(p_rejected.result, RuntimeError, "Error: init failure", "rejected result")

# Pending promise and microtask execution
p_pending = ctx.eval("""
var deferred = Promise.resolve(10).then(function(x) { return x * 3; });
deferred;
""")
# Before draining jobs
if not p_pending.done():
    expect_raise(
        p_pending.result, RuntimeError, "promise not settled", "pending result"
    )

# Run jobs
assert ctx.has_pending_jobs() is True
jobs_run = ctx.run_jobs()
assert jobs_run > 0
assert ctx.has_pending_jobs() is False

# After draining
assert p_pending.done() is True
assert p_pending.result() == 30

print("Promise basics & job queue OK")

# =============================================================
# 2. Promise Combinators & Python Reactions (then, catch, finally_)
# =============================================================
p_base = ctx.eval("Promise.resolve(100)")

# p.then(python_callable)
q_then = p_base.then(lambda x: x + 50)
ctx.run_jobs()
assert q_then.result() == 150

# Chained thens
q_chain = p_base.then(lambda x: x * 2).then(lambda x: str(x) + " items")
ctx.run_jobs()
assert q_chain.result() == "200 items"


# Exception in python reaction rejects derived promise
def fail_reaction(x):
    raise IndexError("reaction failure")


q_err = p_base.then(fail_reaction)
ctx.run_jobs()
assert q_err.done() is True
expect_raise(q_err.result, RuntimeError, "IndexError", "reaction error")

# p.catch recovers from rejection
q_recovered = q_err.catch(lambda err: "recovered from error")
ctx.run_jobs()
assert q_recovered.result() == "recovered from error"

# p.finally_ executes callback and propagates outcome
finally_called = []
q_finally = p_base.finally_(lambda: finally_called.append(True))
ctx.run_jobs()
assert finally_called == [True]
assert q_finally.result() == 100

# Reaction returning another Promise wrapper (native assimilation)
p_inner = ctx.eval("Promise.resolve('nested value')")
q_assimilated = p_base.then(lambda x: p_inner)
ctx.run_jobs()
assert q_assimilated.result() == "nested value"

print("Promise combinators & reactions OK")

# =============================================================
# 3. Python-side Promise Creation (ctx.promise)
# =============================================================
p, resolve, reject = ctx.promise()
assert p.done() is False

# Resolve with value
resolve(999)
assert p.done() is True
assert p.result() == 999

# First settlement wins (duplicate resolve/reject ignored)
resolve(111)
reject("error")
assert p.result() == 999

# Reject with Python exception
p2, resolve2, reject2 = ctx.promise()
reject2(ValueError("custom failure"))
assert p2.done() is True
expect_raise(
    p2.result,
    RuntimeError,
    "ValueError: custom failure",
    "reject with exception",
)

# Resolve with thenable / promise (assimilation)
p_src = ctx.eval("Promise.resolve('assimilated data')")
p3, resolve3, reject3 = ctx.promise()
resolve3(p_src)
ctx.run_jobs()
assert p3.result() == "assimilated data"

# Handlers attached to Python-created promises
p4, resolve4, reject4 = ctx.promise()
q4 = p4.then(lambda v: v.upper())
resolve4("hello")
ctx.run_jobs()
assert q4.result() == "HELLO"

print("Python-side Promise creation (ctx.promise) OK")

# =============================================================
# 4. Native Async / Await Execution Model
# =============================================================
ctx.eval("""
async function asyncAdd(a, b) {
    const va = await Promise.resolve(a);
    const vb = await Promise.resolve(b);
    return va + vb;
}
""")

async_add = ctx.get("asyncAdd")
p_async = async_add(10, 20)
assert isinstance(p_async, type(p))

ctx.run_jobs()
assert p_async.done() is True
assert p_async.result() == 30


# Awaiting Python callbacks
def py_delay_val(x):
    p_sub, res_sub, _ = ctx.promise()
    res_sub(x * 5)
    return p_sub


ctx.add_callable("pyDelayVal", py_delay_val)
ctx.eval("""
async function testAwaitedCallback() {
    const val = await pyDelayVal(8);
    return val + 2;
}
""")

p_cb_async = ctx.get("testAwaitedCallback")()
ctx.run_jobs()
assert p_cb_async.result() == 42

# Complete drain of deep async chains
ctx.eval("""
async function deepChain(n) {
    let cur = 0;
    for (let i = 0; i < n; i++) {
        cur = await Promise.resolve(cur + 1);
    }
    return cur;
}
""")
p_deep = ctx.get("deepChain")(50)
assert ctx.run_jobs() > 0
assert p_deep.result() == 50
assert ctx.has_pending_jobs() is False

print("Native async / await execution model OK")

# =============================================================
# 5. Unhandled Rejection Diagnostics
# =============================================================
events = []


def rejection_tracker(reason, is_handled):
    events.append((str(reason), bool(is_handled)))


ctx.set_unhandled_rejection_handler(rejection_tracker)

# Trigger unhandled rejection
p_unhandled = ctx.eval("Promise.reject('boom')")
assert ("boom", False) in events

# Attach catch handler to mark it handled
p_unhandled.catch(lambda r: None)
ctx.run_jobs()
assert ("boom", True) in events

# Disabling tracker
ctx.set_unhandled_rejection_handler(None)
events.clear()
ctx.eval("Promise.reject('silent')")
assert len(events) == 0

ctx.close()
print("Unhandled rejection tracker OK")

print("ALL PROMISE & ASYNC TESTS PASSED")
