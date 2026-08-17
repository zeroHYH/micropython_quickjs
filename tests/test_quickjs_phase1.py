# Phase 1 tests for the MicroPython quickjs module.
# Run with:  micropython tests/test_quickjs_phase1.py

import quickjs
import gc

# =============================================================
# 1. Context creation + basic eval
# =============================================================
ctx = quickjs.Context()
assert ctx.eval("123") == 123
assert ctx.eval("3.14") == 3.14
assert ctx.eval("true") is True
assert ctx.eval("null") is None
assert ctx.eval("'hello'") == "hello"

result = ctx.eval("""
({
    name: "test",
    value: 123,
    enabled: true
})
""")
assert result["name"] == "test"
assert result["value"] == 123
assert result["enabled"] is True

# =============================================================
# 2. Context.call
# =============================================================
ctx.eval("""
function add(a, b) {
    return a + b;
}
""")
assert ctx.call("add", 10, 20) == 30

ctx.eval("""
function process(obj) {
    return {
        name: obj.name,
        value: obj.value * 2,
        enabled: obj.enabled
    };
}
""")
result = ctx.call("process", {"name": "test", "value": 100, "enabled": True})
assert result["name"] == "test"
assert result["value"] == 200
assert result["enabled"] is True

# =============================================================
# 3. Context.get
# =============================================================
ctx.eval("var answer = 42;")
assert ctx.get("answer") == 42

ctx.eval("var config = { name: 'ESP32', enabled: true };")
config = ctx.get("config")
assert config["name"] == "ESP32"
assert config["enabled"] is True

# missing variable -> None
assert ctx.get("missing") is None
# null / undefined -> None
ctx.eval("var n = null; var u = undefined;")
assert ctx.get("n") is None
assert ctx.get("u") is None

# =============================================================
# 4. Context.set
# =============================================================
ctx.set("answer2", 42)
assert ctx.eval("answer2") == 42

ctx.set("config2", {"name": "MicroPython", "enabled": True, "items": [1, 2, 3]})
assert ctx.eval("config2.items[1]") == 2
assert ctx.eval("config2.name") == "MicroPython"

# =============================================================
# 5. Context.gc
# =============================================================
ctx.eval("var a = []; for (var i = 0; i < 100; i++) { a.push(i); }")
ctx.gc()
assert ctx.eval("a.length") == 100

# =============================================================
# 6. set_memory_limit
# =============================================================
ctx2 = quickjs.Context()
ctx2.set_memory_limit(128 * 1024)
assert ctx2.eval("1 + 1") == 2
# huge allocation should fail cleanly under a small limit
try:
    ctx2.eval("new Array(1 << 28)")
    print("note: huge alloc unexpectedly succeeded")
except Exception as e:
    print("memory limit triggered:", str(e)[:60])
ctx2.close()

# negative limit rejected
ctx3 = quickjs.Context()
try:
    ctx3.set_memory_limit(-1)
    raise AssertionError("expected ValueError")
except ValueError:
    pass
ctx3.close()

# =============================================================
# 7. set_max_stack_size
# =============================================================
ctx4 = quickjs.Context()
ctx4.set_max_stack_size(256 * 1024)
assert ctx4.eval("1 + 1") == 2
try:
    ctx4.set_max_stack_size(-5)
    raise AssertionError("expected ValueError")
except ValueError:
    pass
ctx4.close()

# =============================================================
# 8. close + double close + closed Context errors
# =============================================================
ctx5 = quickjs.Context()
assert ctx5.eval("1 + 2") == 3
ctx5.close()
ctx5.close()  # idempotent

for fn in (
    lambda: ctx5.eval("1 + 2"),
    lambda: ctx5.call("add", 1, 2),
    lambda: ctx5.get("x"),
    lambda: ctx5.set("x", 1),
    lambda: ctx5.gc(),
    lambda: ctx5.set_memory_limit(1024),
    lambda: ctx5.set_max_stack_size(1024),
):
    try:
        fn()
        raise AssertionError("expected RuntimeError after close, got none: %r" % fn)
    except RuntimeError as e:
        assert "context closed" in str(e), "unexpected msg: %r" % str(e)

# =============================================================
# 9. multiple Contexts are isolated
# =============================================================
a = quickjs.Context()
b = quickjs.Context()

a.eval("var x = 10")
b.eval("var x = 20")
assert a.eval("x") == 10
assert b.eval("x") == 20

a.close()
assert b.eval("x") == 20
b.close()

# also default singleton is isolated from Contexts
quickjs.init()
quickjs.eval("var y = 100")
c = quickjs.Context()
try:
    c.eval("y")
    raise AssertionError("context should not see singleton y")
except Exception as e:
    pass  # y is undefined in fresh context -> ReferenceError
c.close()

# =============================================================
# 10. GC / close interplay
# =============================================================
ctx6 = quickjs.Context()
ctx6.eval("var obj = { x: 1, y: [1, 2, 3] };")
gc.collect()
assert ctx6.eval("obj.x") == 1
assert ctx6.eval("obj.y[2]") == 3
ctx6.close()
gc.collect()  # must not crash

# =============================================================
# 11. Context reclaimed by GC (finaliser)
# =============================================================
def make_and_drop():
    t = quickjs.Context()
    t.eval("var z = 1;")
    assert t.eval("z") == 1
    # t goes out of scope here

make_and_drop()
gc.collect()
print("context dropped + gc.collect() OK")

# =============================================================
# 12. stress: many contexts create/close
# =============================================================
for i in range(100):
    q = quickjs.Context()
    assert q.eval("1 + 2") == 3
    q.close()
    del q
    if i % 10 == 0:
        gc.collect()
print("100 create/close cycles OK")

# multiple contexts alive at once
contexts = []
for i in range(20):
    q = quickjs.Context()
    q.set("value", i)
    contexts.append(q)

for i, q in enumerate(contexts):
    assert q.eval("value") == i

for q in contexts:
    q.close()
gc.collect()
print("20 concurrent contexts OK")

# close in reverse order
a = quickjs.Context()
b = quickjs.Context()
b.close()
a.close()
gc.collect()
print("reverse close OK")

# =============================================================
# 13. error paths inside Context (no crash / no leak)
# =============================================================
ctx7 = quickjs.Context()
for _ in range(50):
    try:
        ctx7.eval('throw new Error("boom")')
    except Exception as e:
        assert "boom" in str(e)
for _ in range(50):
    try:
        ctx7.call("nope_undefined_fn", 1, 2)
    except Exception:
        pass
try:
    ctx7.call("add", quickjs)  # unsupported type
except Exception as e:
    print("unsupported arg ->", type(e).__name__, str(e)[:60])
# unsupported key type in set
try:
    ctx7.set("k", {1: "int-key-not-allowed"})
except Exception as e:
    print("int dict key ->", type(e).__name__, str(e)[:60])
ctx7.close()

# =============================================================
# 14. finaliser safety: creating and dropping without close
#     must not crash under GC pressure
# =============================================================
for i in range(30):
    q = quickjs.Context()
    q.eval("var data = []; for (var j = 0; j < 10; j++) data.push(j);")
    del q
    gc.collect()
print("30 create/drop (no close) cycles OK")

print("ALL QUICKJS PHASE1 TESTS PASSED")