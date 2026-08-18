# MicroPython QuickJS - ES Module and Loader Tests
#
# Tests ES module evaluation (ctx.eval_module), custom Python module loaders
# (ctx.set_module_loader), MicroPython VFS file loading, and module error handling.

import os

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


print("=== Running test_module.py ===")

# ---------------------------------------------------------------------------
# Section 1: Basic Module Evaluation
# ---------------------------------------------------------------------------
ctx = quickjs.Context()

# Direct module evaluation with top-level export/assignment
ctx.eval_module("""
export const VALUE = 42;
export function double(x) { return x * 2; }
globalThis.moduleLoaded = true;
globalThis.doubleVal = double(21);
""")

expect(ctx.get("moduleLoaded") is True)
expect(ctx.get("doubleVal") == 42)

ctx.close()
print("Basic module evaluation OK")

# ---------------------------------------------------------------------------
# Section 2: Custom Python Module Loader (Source Code & Bytecode)
# ---------------------------------------------------------------------------
ctx = quickjs.Context()


# 1. Loader returning JS source code (str)
def python_source_loader(name):
    if name == "virtual:math":
        return """
        export function square(x) { return x * x; }
        export const PI = 3.14159;
        """
    if name == "virtual:strings":
        return """
        export function greet(who) { return `Hello, ${who}!`; }
        """
    return None


ctx.set_module_loader(python_source_loader)

ctx.eval_module("""
import { square, PI } from 'virtual:math';
import { greet } from 'virtual:strings';

globalThis.sq = square(8);
globalThis.piVal = PI;
globalThis.greeting = greet('QuickJS');
""")

expect(ctx.get("sq") == 64)
expect(abs(ctx.get("piVal") - 3.14159) < 0.0001)
expect(ctx.get("greeting") == "Hello, QuickJS!")

# 2. Loader returning precompiled bytecode (bytes)
temp_ctx = quickjs.Context()
bytecode_mod = temp_ctx.compile_bytecode(
    "export function power3(x) { return x * x * x; };",
    "virtual:power",
    True,
)
temp_ctx.close()


def python_bytecode_loader(name):
    if name == "virtual:power":
        return bytecode_mod
    return None


ctx.set_module_loader(python_bytecode_loader)

ctx.eval_module("""
import { power3 } from 'virtual:power';
globalThis.powResult = power3(4);
""")

expect(ctx.get("powResult") == 64)

ctx.close()
print("Custom Python module loader OK")

# ---------------------------------------------------------------------------
# Section 3: MicroPython VFS File Loader Fallback
# ---------------------------------------------------------------------------
ctx = quickjs.Context()

# Create temporary JS module files on the filesystem
tmp_file1 = "temp_test_mod_a.js"
tmp_file2 = "temp_test_mod_b.js"

with open(tmp_file1, "w") as f:
    f.write("""
    export function add(a, b) { return a + b; }
    export const secret = 1337;
    """)

with open(tmp_file2, "w") as f:
    f.write(f"""
    import {{ add, secret }} from './{tmp_file1}';
    export function compute(x) {{ return add(x, secret); }}
    """)

try:
    ctx.eval_module(f"""
    import {{ compute }} from './{tmp_file2}';
    globalThis.vfsResult = compute(100);
    """)

    expect(ctx.get("vfsResult") == 1437)
finally:
    # Cleanup files
    try:
        os.remove(tmp_file1)
    except OSError:
        pass
    try:
        os.remove(tmp_file2)
    except OSError:
        pass

ctx.close()
print("MicroPython VFS file loading OK")

# ---------------------------------------------------------------------------
# Section 4: Module Error Handling
# ---------------------------------------------------------------------------
ctx = quickjs.Context()

# Non-existent module without custom loader
expect_raise(
    lambda: ctx.eval_module("import { nothing } from './non_existent_file.js';"),
    RuntimeError,
    "could not load module",
)

# Custom loader returning invalid type
ctx.set_module_loader(lambda name: 12345)
expect_raise(
    lambda: ctx.eval_module("import { x } from 'virtual:bad';"),
    RuntimeError,
    "could not load module",
)


# Custom loader throwing an exception
def failing_loader(name):
    raise ValueError("custom loader crashed")


ctx.set_module_loader(failing_loader)
expect_raise(
    lambda: ctx.eval_module("import { x } from 'virtual:fail';"),
    RuntimeError,
    "module loader failed",
)

# Reset loader to None
ctx.set_module_loader(None)

# Closed context error
ctx.close()
expect_raise(
    lambda: ctx.eval_module("export const a = 1;"),
    RuntimeError,
    "context closed",
)
expect_raise(
    lambda: ctx.set_module_loader(lambda n: None),
    RuntimeError,
    "context closed",
)

print("Module error handling OK")

print("ALL MODULE TESTS PASSED")
