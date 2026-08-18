# MicroPython QuickJS - Console Object Tests
#
# Tests built-in console object (log, warn, error, info, debug) across
# singleton and Context instances, formatting of primitives, objects, and arrays.

import quickjs


def expect(cond, msg="Assertion failed"):
    if not cond:
        raise AssertionError(msg)


print("=== Running test_console.py ===")

# ---------------------------------------------------------------------------
# Section 1: Basic Console Output
# ---------------------------------------------------------------------------
ctx = quickjs.Context()

# console.log with strings and numbers
ctx.eval("console.log('Testing console.log:', 123, 45.67, true, null, undefined);")

# console.info & debug
ctx.eval("console.info('Testing console.info:', 'status OK');")
ctx.eval("console.debug('Testing console.debug:', 'debug trace');")

# console.warn & error
ctx.eval("console.warn('Testing console.warn:', 'low memory warning');")
ctx.eval("console.error('Testing console.error:', 'connection failed');")

print("Basic console methods OK")

# ---------------------------------------------------------------------------
# Section 2: Complex Objects and Arrays
# ---------------------------------------------------------------------------
# Object and Array formatting
ctx.eval("console.log('Object test:', { temp: 25.4, unit: 'C', active: true });")
ctx.eval("console.log('Array test:', [1, 'two', 3.0, [4, 5]]);")

# Inside functions and loops
ctx.eval("""
function report(items) {
    for (let i = 0; i < items.length; i++) {
        console.log(`Item [${i}]:`, items[i]);
    }
}
report(['apple', 'banana', 'orange']);
""")

ctx.close()
print("Complex object console logging OK")

# ---------------------------------------------------------------------------
# Section 3: Module Singleton Console
# ---------------------------------------------------------------------------
quickjs.eval("console.log('Singleton console.log:', 999);")
quickjs.eval("console.warn('Singleton console.warn:', 'caution');")
quickjs.eval("console.error('Singleton console.error:', 'alert');")

print("Singleton console OK")

print("ALL CONSOLE TESTS PASSED")
