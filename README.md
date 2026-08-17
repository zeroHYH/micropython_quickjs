# MicroPython QuickJS-NG binding

Portable MicroPython binding for the [QuickJS-NG](https://github.com/quickjs-ng/quickjs)
JavaScript engine (vendored under `src/`, v0.16.1).

- No ESP32/FreeRTOS/board-specific code — only public MicroPython + QuickJS C APIs.
- Builds on Unix, ESP32, RP2, STM32, ESP8266, nRF via standard `USER_C_MODULES`.

## Build

The QuickJS-NG engine is **not stored in this repository** — it is fetched
from GitHub on first build (see `get_quickjs.sh`).  The Make and CMake
build files fetch it automatically when `src/quickjs.h` is missing, so you
typically do not need to do anything; the first build needs network access.

```sh
./get_quickjs.sh        # optional: fetch manually
```

### Make-based ports (unix, stm32, esp8266, nrf, ...)

```sh
cd <micropython>/ports/unix
make -j$(nproc) USER_C_MODULES=/path/to/micropython_quickjs MICROPY_PY_FFI=0
```

(`MICROPY_PY_FFI=0` is only needed if the host lacks libffi.)

### CMake-based ports (esp32, rp2, ...)

Pass the module directory through `USER_C_MODULES` on the CMake command line
(the project already ships `micropython.cmake`; it fetches QuickJS at
configure time).

### Choosing the QuickJS snapshot

By default the **latest** QuickJS-NG (default branch) is used.  To pin to a
specific tag or commit, re-fetch with `QJS_REF`:

```sh
QJS_REF=v0.16.1 ./get_quickjs.sh
```

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
quickjs.run_jobs()        # execute pending JS jobs (promise microtasks)
quickjs.has_pending_jobs()  # -> bool
quickjs.bigint(10**30)    # explicit BigInt marker (see BigInt below)
quickjs.version()         # -> "0.16.1"
quickjs.help()
```

Note: the singleton has no `Context` opaque, so `eval()` of a JS `Function`
or `Promise` *value* raises `TypeError: unsupported QuickJS value type`
(consistent with how it has always handled functions); side effects and the
job queue still work normally.

### Context (isolated runtime)

```python
ctx = quickjs.Context()        # independent JSRuntime + JSContext

ctx.eval("function add(a,b){ return a+b; }")
ctx.call("add", 10, 20)        # -> 30
ctx.get("answer")              # None if missing
ctx.set("config", {"name": "MicroPython", "items": [1,2,3]})

ctx.gc()                       # run the JS garbage collector
ctx.run_jobs()                 # execute pending JS jobs; returns count
ctx.has_pending_jobs()         # -> bool
ctx.set_memory_limit(128*1024) # JS heap limit, 0 = unlimited
ctx.set_max_stack_size(256*1024) # JS stack limit, 0 = unlimited (default 1 MiB)
ctx._js_mem()                  # debug: JS heap usage in bytes

ctx.close()                    # free runtime; idempotent
```

Each `Context` owns an independent runtime, so global state is fully
isolated between contexts. `close()` is the primary release path; a
`__del__` finaliser also runs when `MICROPY_ENABLE_FINALISER` is enabled,
but never rely on it alone.

After `close()`, every Context method raises `RuntimeError("context closed")`.

While a JS call is executing (a Python callback is on the C stack, or a
microtask is running), calling `ctx.close()` raises
`RuntimeError("context is busy")` instead of tearing down a runtime that is
mid-execution. The rejected `close()` is a no-op: the context stays fully
usable. The busy window covers `eval`/`call`/`get`/`set`/`run_jobs`/
`add_callable`, function-wrapper calls, and Promise methods; the depth
counter is restored on *every* path (return, exception, timeout, job error).

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

### `this` binding (`wrapper.call`)

```python
ctx.eval("globalThis.obj = { value: 42, getValue: function(x){ return this.value + x; } };")
getValue = ctx.eval("obj.getValue")
obj = ctx.get("obj")

getValue.call(obj, 8)          # -> 50   (this = obj)
getValue(8)                    # -> NaN  (default: this is undefined -> sloppy-mode global)
```

- `wrapper.call(this_obj, *args)` binds `this` to the converted
  `this_obj` (dict/object/callable/Promise — anything convertible).
- `wrapper(*args)` keeps the original behaviour: `this` is passed as
  `JS_UNDEFINED` (JS sloppy mode coerces it to the global object).
- Cross-context `this` is rejected (`function belongs to another context`),
  as with any other wrapper pass-through.

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

The timeout covers `eval`, `call`, `run_jobs`, promise methods and
function-wrapper calls. It is implemented with `JS_SetInterruptHandler` +
`mp_hal_ticks_ms()`; the interrupt handler only reads the clock and sets a
flag (no MicroPython API calls, no GC). The timeout state is per-execution
and never poisons the Context.

JS execution can nest (an outer `eval`/`call` runs a Python callback,
which re-enters the Context with another `eval`). Nested windows **share
the outermost window's budget**: an inner `eval` never resets or cancels
the outer deadline, so an infinite outer loop cannot escape the timeout by
re-entering the Context from a callback. When the interrupt fires inside an
inner window, the whole call chain unwinds with a timeout error.

## Promises & the job queue

QuickJS runs promise microtasks in a host-pumped job queue. `eval()`/`call()`
are synchronous and never block on a promise; instead:

- `ctx.run_jobs()` drains the current runtime's pending jobs (microtasks from
  `.then`/`.catch`/`.finally`/async) and returns how many jobs ran
  (`0` when nothing is pending). A job that fails (e.g. the execution
  timeout interrupt) raises a MicroPython exception.
- `ctx.has_pending_jobs()` reports whether the queue is non-empty.

```python
ctx.eval("""
    Promise.resolve(1)
        .then(x => x + 1)
        .then(x => globalThis.result = x)
""")
ctx.run_jobs()
assert ctx.get("result") == 2
```

`ctx.eval()`/`ctx.get()` of a JS `Promise` returns a **Promise wrapper**
(the same lifetime model as the function wrapper — it holds a strong
reference to the Context, so it never dangles):

```python
p = ctx.eval("Promise.resolve(42)")
p.done()                 # -> bool: settled (fulfilled or rejected)?
p.result()               # -> 42  (raises the rejection, or RuntimeError if pending)
```
- `p.result()` on a fulfilled promise converts the value as usual
  (object/array/bytes/etc.).
- On a **rejected** promise it raises the rejection as a MicroPython
  `RuntimeError` (`name: message` + `stack` for `Error` reasons, like any
  other JS error). Non-Error reasons (`Promise.reject("oops")`) are
  stringified.
- On a still-**pending** promise it raises
  `RuntimeError("promise not settled; call ctx.run_jobs()")`.

```python
p = ctx.eval("Promise.resolve(1).then(() => { throw new Error('failure'); })")
ctx.run_jobs()           # the reaction job runs; the derived promise is rejected
p.result()               # raises RuntimeError: Error: failure
```

Notes:
- A rejection is *state*, not an execution error: `run_jobs()` only raises
  for real JS failures (uncatchable errors / timeout / OOM).
- The timeout covers the job queue too: a microtask that spins forever is
  interrupted and surfaces as `JavaScript execution timeout`; the Context
  stays reusable.
- After `ctx.close()`, `done()`/`result()` raise
  `RuntimeError("context closed")`; the wrapper never touches a dangling
  JS value.

### Bridging: `p.then` / `p.catch` / `p.finally_`

The wrapper also exposes the native promise combinators, letting Python
attach reaction callbacks to a JS promise and chain further promises on the
Python side:

```python
q = p.then(on_fulfilled=None, on_rejected=None)   # -> new Promise wrapper
q = p.catch(on_rejected=None)                      # -> p.then(None, r)
q = p.finally_(callback=None)                      # -> new Promise wrapper
```

```python
p = ctx.eval("Promise.resolve(10)")
q = p.then(lambda x: x * 2)          # Python callback runs in a microtask
ctx.run_jobs()                        # execute the reaction job
q.result()                            # -> 20
```

Semantics (implemented on the native QuickJS `JS_PromiseThen` and the
native `Promise.prototype.finally`):

- Handlers may be Python callables, JS function wrappers (same Context), or
  `None` (absent). Anything else raises `TypeError`.
- The callback runs as a **microtask**: drive it with `ctx.run_jobs()`
  (returns after each completed job). Rejections are *state* — call
  `q.result()` on the derived promise.
- **Return-value rules** (per the spec `PromiseResolve` path):
  - returns a **Promise wrapper** of the same Context -> assimilated
    (the derived promise settles with the inner promise's outcome);
  - returns a **JS Function wrapper** -> passes through as a JS value;
  - returns anything else -> converted as usual (`None` -> `null`);
  - a Python exception in the callback rejects the derived promise.
- `p.then()` / `p.catch()` with no handlers is a native pass-through.
- `finally_` runs its callback on settle and propagates the original
  outcome (like JS `Promise.prototype.finally`), including rejections.
- Cross-context handlers/returned promises are rejected
  (`promise belongs to another context`); a wrapper whose Context was
  closed raises `context closed`.
- Promise handler nodes are unnamed and carry no strong `js_func` ref, so
  they are released by the JS GC once the reaction is consumed — they never
  grow the JS heap, and are unlinked by `ctx.close()`.

## Function wrapper pass-through

A JS function wrapper obtained from one Context can be handed back into the
*same* Context — via `ctx.set()`, as a callback return value, or nested in a
`list`/`dict`:

```python
ctx.eval("function mul(a, b){ return a * b; }")
mul = ctx.get("mul")
ctx.set("mul2", mul)           # pass-through: same context
assert ctx.eval("mul2(6, 7)") == 42

def pick():
    return mul                 # callback returning a JS function wrapper
ctx.add_callable("pick", pick)
assert ctx.eval("pick()(3, 4)") == 12
```

Crossing contexts is rejected — a wrapper from `ctx1` passed into `ctx2`
raises `RuntimeError: TypeError: function belongs to another context` (the
underlying JSValue is never moved between runtimes). A wrapper whose Context
was closed raises `context closed`.

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
| `quickjs.bigint(n)` | `BigInt` (arbitrary precision; see below) |
| Python callable | `Function` (via `add_callable`) |
| JS Function wrapper | its `Function` (same Context only; cross-Context → error) |
| JS Promise wrapper | its `Promise` (same Context only; cross-Context → error) |

| JS | MicroPython |
|---|---|
| `null` / `undefined` | `None` |
| `boolean` | `bool` |
| `number` | `int` (integral) or `float` |
| `string` | `str` |
| `Array` | `list` |
| `Object` | `dict` |
| `ArrayBuffer` | `bytes` |
| TypedArray (any kind) | `bytes` — raw byte representation (see below) |
| `BigInt` (int64 range) | `int` |
| `Function` | Python callable wrapper |
| `Promise` | Promise wrapper (`done()` / `result()` / `then()` / `catch()` / `finally_()`) |

`bytes` / `ArrayBuffer` / `bytearray` / TypedArray conversions are **copy
semantics** — no borrowed pointers in either direction, so the Python object
and the JS value never alias.

### BigInt

Python `int` -> JS stays a `number` (int32/float64, unchanged). For an
explicit JS `BigInt`, use `quickjs.bigint(value)`:

```python
ctx.set("x", quickjs.bigint(123456789012345678901234567890))   # arbitrary precision
ctx.eval("x.toString()")    # -> "123456789012345678901234567890"
ctx.set("y", quickjs.bigint(-42))                              # negative / 0 / small OK
```

- Values within int64 range use the official `JS_NewBigInt64` C API; values
  beyond int64 are parsed from a strictly-validated `[0-9]+` decimal string
  into a BigInt literal (no fake arbitrary precision, no truncation).
- `quickjs.bigint()` requires an integer (`ValueError` otherwise).
- Reading a BigInt back into Python is limited to int64 (larger values
  raise `TypeError: BigInt out of range` — read them via `x.toString()`).

### TypedArray -> bytes

Any JS TypedArray (`Int8Array`, `Uint8Array`, `Uint8ClampedArray`,
`Int16Array`, `Uint16Array`, `Int32Array`, `Uint32Array`, `Float32Array`,
`Float64Array`, plus BigInt64/Float16) converts to `bytes` as its **raw byte
representation**: the view slice (`subarray` offsets respected) in **host
byte order**, with no element interpretation. `Uint8Array` behaviour is
unchanged. Detached buffers raise an error.

### Not supported (yet)

- `Symbol` -> raises `TypeError`
- `BigInt` outside int64 range read back -> raises `TypeError: BigInt out of range`
  (never silently converted to float or truncated)
- Python-side *construction* of a new Promise with executor function
  (promises are created in JS; `then`/`catch`/`finally_` bridge the other way)
- `Map` / `Set` / `Date` / typed-array *element* semantics (raw bytes only)

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
- **Conversion failures never leak**: every failure during
  MicroPython → JS conversion (unsupported type, out-of-range integer,
  buffer error, …) surfaces as a JS-style exception (`RuntimeError:
  TypeError: <Python error>`, e.g. a raw `10**300` argument raises
  `RuntimeError: TypeError: OverflowError: overflow converting long int
  to machine word`) instead of a raw MicroPython exception jumping out in
  the middle of the conversion. Use `quickjs.bigint()` for huge integers.
  Partially built arguments/containers are released on all such paths, so
  the Context closes cleanly afterwards.

## Tests

Run with the built unix `micropython` binary:

```sh
micropython tests/test_quickjs_phase0.py   # baseline conversions + API
micropython tests/test_quickjs_phase1.py   # Context lifecycle / GC
micropython tests/test_quickjs_phase2.py   # depth/cycles, binary, BigInt, errors
micropython tests/test_quickjs_phase3.py   # function bridge + timeout
micropython tests/test_quickjs_phase4.py   # promise/job queue + pass-through + lifecycle
micropython tests/test_quickjs_phase5.py   # reentrancy, promise bridging, this, TypedArray, bigint, GC/lifetime
micropython tests/test_quickjs_phase6.py   # execution safety: nested timeout budget, conversion ownership, contamination
```

## Layout

```
micropython.cmake     CMake-based ports (esp32, rp2) — auto-fetches QuickJS
micropython.mk        Make-based ports (unix, stm32, ...) — auto-fetches QuickJS
get_quickjs.sh        fetch the QuickJS-NG engine into src/ (from GitHub)
modquickjs.c          the module implementation
tests/                Python test suites
src/                  generated: QuickJS-NG fetched from GitHub (git-ignored)
```

## License

MIT (QuickJS-NG is MIT).
