# MicroPython QuickJS - Bytecode Compilation & Execution Tests
#
# Tests bytecode compilation (quickjs.compile_bytecode, ctx.compile_bytecode)
# and bytecode execution (quickjs.eval_bytecode, ctx.eval_bytecode),
# cross-context execution, async bytecode, and error handling.

import quickjs


def expect(cond, msg="Assertion failed"):
    if not cond:
        raise AssertionError(msg)


def expect_raise(fn, exc_type, msg_contains="", label=""):
    try:
        fn()
    except exc_type as e:
        if msg_contains:
            expect(msg_contains in str(e), f"{label}: {e!r} lacks {msg_contains!r}")
        return e
    except Exception as e:
        raise AssertionError(f"{label}: expected {exc_type.__name__}, got {e!r}") from e
    raise AssertionError(
        f"{label}: expected exception {exc_type.__name__} containing {msg_contains!r}"
    )


print("=== Running test_bytecode.py ===")

# ---------------------------------------------------------------------------
# Section 1: Basic Bytecode Compilation & Execution
# ---------------------------------------------------------------------------
# 1. Module-level compile & eval
bc = quickjs.compile_bytecode("10 + 25")
expect(isinstance(bc, bytes), f"Expected bytes, got {type(bc)}")
expect(len(bc) > 0, "Bytecode should not be empty")

res = quickjs.eval_bytecode(bc)
expect(res == 35, f"Expected 35, got {res}")

# 2. Context-level compile & eval
ctx = quickjs.Context()
bc2 = ctx.compile_bytecode("function mul(a, b) { return a * b; }; mul(6, 7);")
expect(isinstance(bc2, bytes))
res2 = ctx.eval_bytecode(bc2)
expect(res2 == 42, f"Expected 42, got {res2}")

# 3. Context state persistence across bytecode chunks
ctx.eval_bytecode(ctx.compile_bytecode("globalThis.counter = 100;"))
expect(ctx.get("counter") == 100)
ctx.eval_bytecode(ctx.compile_bytecode("globalThis.counter += 50;"))
expect(ctx.get("counter") == 150)

ctx.close()
print("Basic bytecode compilation & execution OK")

# ---------------------------------------------------------------------------
# Section 2: Cross-Context Bytecode Sharing
# ---------------------------------------------------------------------------
ctx1 = quickjs.Context()
ctx2 = quickjs.Context()

# Compile on ctx1
bc_shared = ctx1.compile_bytecode(
    "function greet(name) { return 'Hello, ' + name + '!'; }; greet('World');"
)

# Run on ctx1
expect(ctx1.eval_bytecode(bc_shared) == "Hello, World!")

# Run the exact same bytecode on ctx2
expect(ctx2.eval_bytecode(bc_shared) == "Hello, World!")

ctx1.close()
ctx2.close()
print("Cross-context bytecode sharing OK")

# ---------------------------------------------------------------------------
# Section 3: Complex Types and Async Bytecode
# ---------------------------------------------------------------------------
ctx = quickjs.Context()

# Object / Array return
bc_obj = ctx.compile_bytecode(
    "({ status: 'ok', numbers: [1, 2, 3], nested: { val: 42 } })"
)
res_obj = ctx.eval_bytecode(bc_obj)
expect(res_obj == {"status": "ok", "numbers": [1, 2, 3], "nested": {"val": 42}})

# Async / Promise in bytecode
bc_async = ctx.compile_bytecode(
    "async function compute() { return await Promise.resolve(99); }; compute();"
)
p = ctx.eval_bytecode(bc_async)
expect(p.done() is False)
ctx.run_jobs()
expect(p.done() is True)
expect(p.result() == 99)

ctx.close()
print("Complex types and async bytecode OK")

# ---------------------------------------------------------------------------
# Section 4: Bytecode Error Handling
# ---------------------------------------------------------------------------
ctx = quickjs.Context()

# Syntax error during compile
expect_raise(
    lambda: ctx.compile_bytecode("function (invalid syntax"),
    RuntimeError,
    "SyntaxError",
)

# Runtime error inside bytecode
bc_err = ctx.compile_bytecode("throw new Error('boom in bytecode');")
expect_raise(lambda: ctx.eval_bytecode(bc_err), RuntimeError, "boom in bytecode")

# Non-bytes input to eval_bytecode
expect_raise(lambda: ctx.eval_bytecode(12345), TypeError)

# Corrupted / invalid bytecode
corrupted = b"INVALID_BYTECODE_DATA_HEADER"
expect_raise(lambda: ctx.eval_bytecode(corrupted), RuntimeError, "invalid version")

# Closed context
ctx.close()
expect_raise(lambda: ctx.compile_bytecode("1 + 1"), RuntimeError, "context closed")
expect_raise(lambda: ctx.eval_bytecode(b"\x00"), RuntimeError, "context closed")

print("Bytecode error handling OK")

print("ALL BYTECODE TESTS PASSED")
