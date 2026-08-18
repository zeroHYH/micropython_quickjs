# MicroPython QuickJS - Function & Callback Tests
#
# Tests function bridges in both directions:
# - JS Function wrappers, calling, and `this` binding (`wrapper.call`)
# - Python callable registration in JS via `ctx.add_callable()`
# - Callback exception handling & translation
# - Reentrancy and `context is busy` protection

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


print("=== Running test_function.py ===")

# =============================================================
# 1. JS Function -> Python Callable Wrapper
# =============================================================
ctx = quickjs.Context()

ctx.eval("function add(a, b) { return a + b; }")
add = ctx.get("add")
assert callable(add)
assert add(10, 20) == 30
assert add("hello ", "world") == "hello world"

# Keyword argument rejection
expect_raise(
    lambda: add(a=1, b=2),
    TypeError,
    "does not support keyword arguments",
    "kwarg rejection",
)

# Functions returning objects/arrays
ctx.eval(
    "function makeUser(name, age) { return {name: name, age: age, tags: ['user', 'active']}; }"
)
make_user = ctx.get("makeUser")
user = make_user("Alice", 25)
assert user == {"name": "Alice", "age": 25, "tags": ["user", "active"]}

# Passing a JS Function wrapper back into the same Context
ctx.eval("function callTwice(fn, x) { return fn(fn(x)); }")
call_twice = ctx.get("callTwice")
ctx.eval("function double(x) { return x * 2; }")
double = ctx.get("double")
assert call_twice(double, 5) == 20

# Passing wrapper across different Contexts is rejected
ctx2 = quickjs.Context()
ctx2.eval("function runner(f) { return f(); }")
runner = ctx2.get("runner")
expect_raise(
    lambda: runner(double),
    RuntimeError,
    "function belongs to another context",
    "cross context fn",
)
ctx2.close()

print("JS Function wrappers OK")

# =============================================================
# 2. `this` Binding (wrapper.call)
# =============================================================
ctx.eval(
    "globalThis.obj_m = { value: 42, getValue: function(x) { return this.value + x; } };"
)
obj_m = ctx.get("obj_m")
w_m = ctx.eval("obj_m.getValue")

# wrapper.call(this_obj, *args) binds `this`
assert w_m.call(obj_m, 8) == 50

# Custom dict as `this`
custom_obj = {"value": 100}
assert w_m.call(custom_obj, 7) == 107

# Default invocation has undefined this (NaN in arithmetic)
r_default = w_m(8)
assert r_default != 50

print("`this` binding via wrapper.call OK")


# =============================================================
# 3. Python Callables -> JS (ctx.add_callable)
# =============================================================
def py_sum(a, b):
    return a + b


ctx.add_callable("pySum", py_sum)
assert ctx.eval("pySum(15, 25)") == 40


# Multiple args and nested structures
def process_data(user, multipliers):
    return {
        "name": user["name"].upper(),
        "total": sum(m * user["base"] for m in multipliers),
    }


ctx.add_callable("processData", process_data)
ctx.eval("""
var res = processData({name: 'bob', base: 10}, [1, 2, 3]);
""")
assert ctx.get("res") == {"name": "BOB", "total": 60}


# Overwriting registered callable
def add_v1(x):
    return x + 1


def add_v2(x):
    return x + 10


ctx.add_callable("adder", add_v1)
assert ctx.eval("adder(5)") == 6
ctx.add_callable("adder", add_v2)
assert ctx.eval("adder(5)") == 15

# Python callable returning JS function wrapper
ctx.add_callable("getMultiplier", lambda: double)
assert ctx.eval("getMultiplier()(7)") == 14

print("Python callable registration (add_callable) OK")


# =============================================================
# 4. Callback Exceptions & Propagation
# =============================================================
def buggy_func(x):
    if x < 0:
        raise ValueError(f"negative not allowed: {x}")
    return x * 2


ctx.add_callable("buggy", buggy_func)

# Python exception surfaces as RuntimeError with type info
expect_raise(
    lambda: ctx.eval("buggy(-5)"),
    RuntimeError,
    "ValueError: negative not allowed: -5",
    "callback exception",
)

# JS code can try/catch the error
ctx.eval("""
var caughtMsg = "";
try {
    buggy(-10);
} catch (e) {
    caughtMsg = String(e);
}
""")
assert "ValueError: negative not allowed: -10" in ctx.get("caughtMsg")

# Context remains fully operational after caught/uncaught exceptions
assert ctx.eval("1 + 2") == 3

print("Callback exceptions OK")

# =============================================================
# 5. Reentrancy & Busy-Context Protection
# =============================================================
# Nested reentrancy: JS -> Python callback -> JS eval -> Python callback -> JS eval
nest_history = []


def outer_cb(x):
    nest_history.append(f"outer:{x}")
    return ctx.eval(f"innerCb({x * 10})")


def inner_cb(y):
    nest_history.append(f"inner:{y}")
    return y + 1


ctx.add_callable("outerCb", outer_cb)
ctx.add_callable("innerCb", inner_cb)

assert ctx.eval("outerCb(3)") == 31
assert nest_history == ["outer:3", "inner:30"]


# Calling ctx.close() while execution is active inside a callback must be refused
def suicide_cb():
    ctx.close()


ctx.add_callable("suicide", suicide_cb)
expect_raise(
    lambda: ctx.eval("suicide()"),
    RuntimeError,
    "context is busy",
    "close while busy",
)

# Context remains usable after refused close
assert ctx.eval("100 + 200") == 300

# Normal close outside execution succeeds
ctx.close()
expect_raise(
    lambda: ctx.eval("1 + 1"), RuntimeError, "context closed", "eval after close"
)

print("Reentrancy & busy context protection OK")

print("ALL FUNCTION TESTS PASSED")
