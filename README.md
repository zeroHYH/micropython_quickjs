# MicroPython QuickJS-NG binding

Portable MicroPython binding for the [QuickJS-NG](https://github.com/quickjs-ng/quickjs)
JavaScript engine (vendored under `src/`, v0.16.1).

- No ESP32/FreeRTOS/board-specific code — only public MicroPython + QuickJS C APIs.
- Builds on Unix, ESP32, RP2, STM32, ESP8266, nRF via standard `USER_C_MODULES`.

## Build

### Make-based ports (unix, stm32, esp8266, nrf, ...)

```sh
cd <micropython>/ports/unix
make -j$(nproc) USER_C_MODULES=/path/to/micropython_quickjs MICROPY_PY_FFI=0
```

(`MICROPY_PY_FFI=0` is only needed if the host lacks libffi.)

### CMake-based ports (esp32, rp2, ...)

Pass the module directory through `USER_C_MODULES` on the CMake command line
(the project already ships `micropython.cmake`).

> The default QuickJS heap limit is 64 KiB (matching the original ESP32
> build). On 64-bit hosts QuickJS-NG needs more than 64 KiB just to create a
> JSContext, so Make-based builds raise the default via
> `-DQUICKJS_DEFAULT_MEMORY_LIMIT` (default 8 MiB; override on the make
> command line). CMake ports keep 64 KiB.

## API

### Default singleton (compatibility layer)

```python
import quickjs

quickjs.init()            # idempotent
quickjs.eval("1 + 2")     # -> 3
quickjs.call("add", 1, 2)
quickjs.version()         # -> "0.16.1"
quickjs.help()
```

### Context (isolated runtime)

```python
ctx = quickjs.Context()        # independent JSRuntime + JSContext

ctx.eval("function add(a,b){ return a+b; }")
ctx.call("add", 10, 20)        # -> 30
ctx.get("answer")              # None if missing
ctx.set("config", {"name": "MicroPython", "items": [1,2,3]})

ctx.gc()                       # run the JS garbage collector
ctx.set_memory_limit(128*1024) # JS heap limit, 0 = unlimited
ctx.set_max_stack_size(256*1024) # JS stack limit, 0 = unlimited (default 1 MiB)

ctx.close()                    # free runtime; idempotent
```

Each `Context` owns an independent runtime, so global state is fully
isolated between contexts. `close()` is the primary release path; a
`__del__` finaliser also runs when `MICROPY_ENABLE_FINALISER` is enabled,
but never rely on it alone.

After `close()`, every Context method raises `RuntimeError("context closed")`.

### JS Function <-> MicroPython callable

```python
ctx.eval("function add(a,b){ return a+b; }")
add = ctx.get("add")           # JS function -> Python callable wrapper
assert callable(add)
assert add(1, 2) == 3
```

The wrapper keeps its `Context` alive (strong reference), so it never dangles.
After `ctx.close()` the wrapper raises `RuntimeError("context closed")`.
Keyword arguments are rejected with `TypeError`.

```python
def multiply(a, b):
    return a * b
ctx.add_callable("multiply", multiply)   # Python callable -> JS function
assert ctx.eval("multiply(6, 7)") == 42
```

Python callables registered this way are kept alive by a per-Context
callback registry (GC-rooted, no `static mp_obj_t`). The registered
callable is called through a `JS_NewCClosure` bridge; a Python exception
raised inside the callback is caught (never crosses the QuickJS C stack)
and re-thrown as a JS error, then mapped back to MicroPython:

```python
def fail():
    raise ValueError("callback failure")
ctx.add_callable("fail", fail)
try:
    ctx.eval("fail()")
except Exception as e:
    print(e)   # TypeError: ValueError: callback failure
```

JS code can also `try/catch` the error. Registering the same name twice
replaces the previous callable.

### Execution timeout

```python
ctx.set_time_limit(100)          # ms; 0 disables; negative -> ValueError

try:
    ctx.eval("while (true) {}")
except RuntimeError as e:
    print(e)                     # JavaScript execution timeout

ctx.set_time_limit(0)
assert ctx.eval("1 + 2") == 3   # context is reusable after a timeout
```

The timeout covers `eval`, `call`, and function-wrapper calls. It is
implemented with `JS_SetInterruptHandler` + `mp_hal_ticks_ms()`; the
interrupt handler only reads the clock and sets a flag (no MicroPython
API calls, no GC). The timeout state is per-execution and never poisons
the Context.

## Type conversions

| MicroPython | JS |
|---|---|
| `None` | `null` / `undefined` |
| `bool` | `boolean` |
| `int` | `number` (int32 if it fits, else float64) |
| `float` | `number` |
| `str` | `string` |
| `list` / `tuple` | `Array` |
| `dict` | `Object` (string keys only) |
| `bytes` | `ArrayBuffer` |
| `bytearray` | `Uint8Array` |
| Python callable | `Function` (via `add_callable`) |

| JS | MicroPython |
|---|---|
| `null` / `undefined` | `None` |
| `boolean` | `bool` |
| `number` | `int` (integral) or `float` |
| `string` | `str` |
| `Array` | `list` |
| `Object` | `dict` |
| `ArrayBuffer` | `bytes` |
| `Uint8Array` | `bytes` |
| `BigInt` (int64 range) | `int` |
| `Function` | Python callable wrapper |

`bytes` / `ArrayBuffer` / `bytearray` / `Uint8Array` conversions are **copy
semantics** — no borrowed pointers in either direction, so the Python object
and the JS value never alias.

### Not supported (yet)

- Promise / async bridging
- JS `this` binding for wrappers (calls pass `JS_UNDEFINED`)
- `Uint8Array` is the only supported TypedArray; others raise
  `TypeError: unsupported typed array`
- `Symbol` -> raises `TypeError`
- `BigInt` outside int64 range -> raises `TypeError: BigInt out of range`
  (never silently converted to float or truncated)
- Python `int` -> JS BigInt (Python ints stay int32/float64)

### Exceptions

JS exceptions are mapped to MicroPython `RuntimeError`. For `Error` objects
the message includes `name: message` plus the JS `stack` when available:

```
Error: hello
stack:
    at <eval>:1:...
```

## Conversion safety

- **Recursion depth limit** (`QUICKJS_MAX_CONVERSION_DEPTH`, default 32):
  deeper nesting raises `RuntimeError: maximum conversion depth exceeded`
  instead of overflowing the C stack.
- **Circular reference detection** (active-stack based): self-referential
  lists/dicts/JS arrays/objects raise
  `RuntimeError: circular reference detected`. Shared (non-circular)
  references such as `[x, x]` are *not* misdetected.

## Tests

Run with the built unix `micropython` binary:

```sh
micropython tests/test_quickjs_phase0.py   # baseline conversions + API
micropython tests/test_quickjs_phase1.py   # Context lifecycle / GC
micropython tests/test_quickjs_phase2.py   # depth/cycles, binary, BigInt, errors
micropython tests/test_quickjs_phase3.py   # function bridge + timeout
```

## Layout

```
micropython.cmake     CMake-based ports (esp32, rp2)
micropython.mk        Make-based ports (unix, stm32, ...)
modquickjs.c          the module implementation
src/                  vendored QuickJS-NG (v0.16.1, unmodified)
tests/                Python test suites
```

## License

MIT (QuickJS-NG is MIT).
