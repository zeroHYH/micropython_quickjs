# MicroPython QuickJS - Basic & Execution Tests
#
# Tests module-level singleton functions, Context instance basics,
# property access (eval/call/get/set), and execution timeout controls.

import math

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


print("=== Running test_basic.py ===")

# =============================================================
# 1. Module-level singleton (quickjs.init, eval, call, version, help)
# =============================================================
quickjs.init()
quickjs.init()  # idempotent

assert quickjs.eval("1 + 2") == 3
assert quickjs.eval("'hello ' + 'world'") == "hello world"
assert quickjs.eval("true && false") is False

# Function registration and calling in singleton
quickjs.eval("function add(a, b) { return a + b; }")
assert quickjs.call("add", 10, 20) == 30
assert quickjs.call("add", "foo", "bar") == "foobar"

# JS float nan/inf handling
quickjs.eval("function testFloat(a, b, c) { return {a: a, b: b, c: c}; }")
r = quickjs.call("testFloat", float("nan"), float("inf"), float("-inf"))
assert math.isnan(r["a"])
assert r["b"] == float("inf")
assert r["c"] == float("-inf")

# Version string
ver = quickjs.version()
assert isinstance(ver, str) and len(ver) > 0

# Repeated execution in singleton
for _ in range(500):
    assert quickjs.eval("1 + 2") == 3

# Singleton job queue methods
assert quickjs.has_pending_jobs() is False
assert quickjs.run_jobs() == 0

print("Module singleton basics OK")

# =============================================================
# 2. Context creation, basic eval, call, get, set
# =============================================================
ctx = quickjs.Context()

# Eval basic expressions
assert ctx.eval("10 * 20") == 200
assert ctx.eval("[1, 2, 3]") == [1, 2, 3]
assert ctx.eval("({a: 1, b: 'two'})") == {"a": 1, "b": "two"}

# Call functions defined in context
ctx.eval("function multiply(x, y) { return x * y; }")
assert ctx.call("multiply", 6, 7) == 42
assert ctx.call("multiply", 2.5, 4) == 10.0

# Context.get()
ctx.eval("var myVar = 'quickjs'; var myNum = 12345; var myObj = {k: 'v'};")
assert ctx.get("myVar") == "quickjs"
assert ctx.get("myNum") == 12345
assert ctx.get("myObj") == {"k": "v"}
assert ctx.get("nonExistentVar") is None

# Context.set()
ctx.set("pyString", "from python")
ctx.set("pyInt", 999)
ctx.set("pyList", [10, 20, 30])
ctx.set("pyDict", {"x": 100, "y": 200})

assert ctx.eval("pyString") == "from python"
assert ctx.eval("pyInt") == 999
assert ctx.eval("pyList") == [10, 20, 30]
assert ctx.eval("pyDict.x + pyDict.y") == 300

# Context GC and memory inspection
assert ctx.gc() is None
mem = ctx._js_mem()
assert isinstance(mem, int) and mem > 0

ctx.close()
print("Context basic eval/call/get/set OK")

# =============================================================
# 3. Execution timeout (set_time_limit)
# =============================================================
ctx_t = quickjs.Context()

# Timeout validation
expect_raise(
    lambda: ctx_t.set_time_limit(-1),
    ValueError,
    "time limit must be non-negative",
    "neg timeout",
)

# Timeout triggering on infinite loop
ctx_t.set_time_limit(100)  # 100 ms
expect_raise(
    lambda: ctx_t.eval("while (true) {}"),
    RuntimeError,
    "JavaScript execution timeout",
    "eval timeout",
)

# Context remains reusable after timeout
assert ctx_t.eval("1 + 2") == 3

# Timeout on ctx.call
ctx_t.eval("function infinite() { while (true) {} }")
expect_raise(
    lambda: ctx_t.call("infinite"),
    RuntimeError,
    "JavaScript execution timeout",
    "call timeout",
)
assert ctx_t.eval("1 + 2") == 3

# Disabling timeout (0 ms)
ctx_t.set_time_limit(0)
assert ctx_t.eval("var s = 0; for (var i = 0; i < 10000; i++) s += i; s;") == 49995000

# Nested execution shares outermost timeout budget
ctx_t.set_time_limit(200)
loop_count = 0


def inner_work():
    global loop_count
    loop_count += 1
    ctx_t.eval("1 + 1")


ctx_t.add_callable("innerWork", inner_work)
expect_raise(
    lambda: ctx_t.eval("while (true) { innerWork(); }"),
    RuntimeError,
    "JavaScript execution timeout",
    "nested timeout",
)
expect(loop_count > 0, "innerWork executed multiple times before timeout")

# Context reusable again
ctx_t.set_time_limit(0)
assert ctx_t.eval("10 + 20") == 30

ctx_t.close()
print("Execution timeout OK")

print("ALL BASIC TESTS PASSED")
