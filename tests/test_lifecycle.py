# MicroPython QuickJS - Lifecycle, GC & Stress Tests
#
# Tests runtime integrity, memory limits, and stress resilience:
# - Multiple concurrent Context instances & isolation
# - Wrapper lifecycle & GC anchoring (strong Context reference)
# - Memory limits (`set_memory_limit`, `set_max_stack_size`, OOM handling)
# - Comprehensive stress loops (1000+ iterations of eval, promises, async)

import gc

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


print("=== Running test_lifecycle.py ===")

# =============================================================
# 1. Multiple Concurrent Contexts & Isolation
# =============================================================
contexts = [quickjs.Context() for _ in range(10)]

# Each context has independent global scope
for i, c in enumerate(contexts):
    c.set("id", i * 10)

for i, c in enumerate(contexts):
    assert c.eval("id") == i * 10

# Closing contexts in arbitrary order
for c in reversed(contexts):
    c.close()

# Dropping contexts without explicit close (GC cleanup)
for _ in range(30):
    c = quickjs.Context()
    c.eval("var x = 'temp';")
    del c
gc.collect()

print("Multiple concurrent Contexts & isolation OK")

# =============================================================
# 2. Wrapper Strong Reference to Context
# =============================================================
# Function wrapper keeps Context alive even after dropping python variable
ctx_fn = quickjs.Context()
ctx_fn.eval("function secret(x) { return x * 100; }")
fn_wrap = ctx_fn.get("secret")
del ctx_fn
gc.collect()

assert fn_wrap(5) == 500
del fn_wrap
gc.collect()

# Promise wrapper keeps Context alive
ctx_p = quickjs.Context()
p, resolve, reject = ctx_p.promise()
del ctx_p
gc.collect()

resolve("alive")
assert p.result() == "alive"
del p, resolve, reject
gc.collect()

print("Wrapper lifetime & Context anchoring OK")

# =============================================================
# 3. Memory Limits & OOM Robustness
# =============================================================
ctx_mem = quickjs.Context()

# Setting memory limit
ctx_mem.set_memory_limit(256 * 1024)

# Setting max stack size
ctx_mem.set_max_stack_size(128 * 1024)
expect_raise(
    lambda: ctx_mem.set_memory_limit(-1),
    ValueError,
    "memory limit must be >= 0",
    "neg mem limit",
)
expect_raise(
    lambda: ctx_mem.set_max_stack_size(-1),
    ValueError,
    "max stack size must be >= 0",
    "neg stack size",
)

# Huge allocation triggers memory limit gracefully without crash
try:
    ctx_mem.eval("new Uint8Array(1 << 28)")
except (RuntimeError, MemoryError):
    pass

# Context remains clean and usable after allocation failure
assert ctx_mem.eval("1 + 2") == 3
ctx_mem.close()

print("Memory limits & OOM robustness OK")

# =============================================================
# 4. High-Volume Stress Loops
# =============================================================
ctx_s = quickjs.Context()
ctx_s.set_time_limit(30000)

# 1000 eval iterations
for i in range(1000):
    assert ctx_s.eval(f"{i} + 1") == i + 1

# 500 function wrapper calls
ctx_s.eval("function f(x) { return x * 2; }")
f = ctx_s.get("f")
for i in range(500):
    assert f(i) == i * 2

# 500 Promise resolve & drain cycles
for i in range(500):
    p, r, _ = ctx_s.promise()
    r(i)
    assert p.result() == i

# 200 async/await executions
ctx_s.eval("async function addAsync(a, b) { return (await Promise.resolve(a)) + b; }")
add_async = ctx_s.get("addAsync")
for i in range(200):
    p_a = add_async(i, 1)
    ctx_s.run_jobs()
    assert p_a.result() == i + 1

# Interleaved GC
gc.collect()
ctx_s.gc()
assert ctx_s.eval("40 + 2") == 42

ctx_s.close()
print("High-volume stress loops OK")

# =============================================================
# 5. Mixed Lifecycle & Error Matrix Regression
# =============================================================
# Double close is safe and idempotent
ctx_dc = quickjs.Context()
ctx_dc.close()
ctx_dc.close()

# Operations after close raise RuntimeError("context closed")
expect_raise(
    lambda: ctx_dc.eval("1"), RuntimeError, "context closed", "eval after close"
)
expect_raise(
    lambda: ctx_dc.call("f"), RuntimeError, "context closed", "call after close"
)
expect_raise(lambda: ctx_dc.get("x"), RuntimeError, "context closed", "get after close")
expect_raise(
    lambda: ctx_dc.set("x", 1),
    RuntimeError,
    "context closed",
    "set after close",
)
expect_raise(
    lambda: ctx_dc.run_jobs(),
    RuntimeError,
    "context closed",
    "run_jobs after close",
)
expect_raise(
    lambda: ctx_dc.promise(),
    RuntimeError,
    "context closed",
    "promise after close",
)

print("Error matrix & regression validation OK")

print("ALL LIFECYCLE & STRESS TESTS PASSED")
