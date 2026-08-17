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

> The default QuickJS heap limit is 128 KiB (`QUICKJS_DEFAULT_MEMORY_LIMIT` in
> `modquickjs.c`, no longer overridden at build time). On a 64-bit host
> QuickJS-NG needs more than 64 KiB just to create a JSContext, so Make-based
> builds of this module have always needed a host-side override; the default
> was raised to 128 KiB because that is the verified *device* budget needed
> for one full Context (see ESP32 notes below). Override on the make command
> line with `CFLAGS_USERMOD=-DQUICKJS_DEFAULT_MEMORY_LIMIT=...` if desired.
>
> **ESP32 limitation:** with a 128 KiB QuickJS heap limit the device can
> reliably hold **one** Context: quickjs.eval, Context() create/close/recreate,
> eval/call/get/set, Python callbacks, Function wrappers, timeouts,
> Promise/run_jobs and `ctx.promise()` all work. Creating a *second*
> Context concurrently fails with `failed to create JS Context` — this is the
> expected resource limit, not a bug, and this module intentionally does not
> try to fit multiple Contexts (it never alters QuickJS-NG memory policy).
> After `ctx.close()` a fresh Context (re)creation succeeds.

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

# Phase 7: create a promise from Python
p, resolve, reject = ctx.promise()   # -> (Promise wrapper, resolve, reject)
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

### Creating promises from Python: `ctx.promise()`

`ctx.promise()` creates a new pending `Promise` on the Python side and returns
the three-tuple `(p, resolve, reject)`:

```python
p, resolve, reject = ctx.promise()   # p: Promise wrapper (same as eval's)
resolve(42)                          # settle fulfilled
# reject(ValueError("failed"))       # settle rejected (see below)
assert p.result() == 42
```

- `p` is the ordinary Promise wrapper (`done`/`result`/`then`/`catch`/
  `finally_`), so everything above composes.
- `resolve`/`reject` are separate MicroPython callables created by the same
  callback registry as function wrappers (token + strong Context ref). They
  keep the promise alive: the promise stays reachable for the JS GC while
  either resolver exists, even after `del p`.
- **Settlement is first-wins, exactly like JS:** calling `resolve` after a
  resolution (or any `reject`) is ignored; same for `reject` after a
  rejection. The Promise state machine — including this rule — lives entirely
  inside QuickJS; the Python side never tracks settled state.
- **Thenable/promise assimilation is native:** `resolve(thenable)` or
  `resolve(existingPromiseWrapper)` runs the standard `Promise Resolve`
  algorithm (the internal `then` probe + thenable job). A thenable's `.then`
  may run arbitrary JS — or a Python callback — so `resolve` is a full
  guarded window: execution depth, timeout, and exception conversion all
  apply, and the context re-entrancy rules hold.
  A promise wrapper from another Context (or after its Context closed) is
  rejected by the conversion layer (`promise belongs to another context` /
  `context closed`) and the promise stays pending.
- `resolve(None)` -> `null` -> `None`; `resolve(dict/list/bytes/BigInt)`
  convert as usual (bytes -> `ArrayBuffer`, BigInt beyond int64 raises
  `BigInt out of range` while the promise stays pending).
- **Rejecting with a Python exception:** `reject(ValueError("failed"))` turns
  the exception into a JS `TypeError: ValueError: failed` for the rejection
  reason (same conversion the callback path uses); `p.result()` then raises
  the corresponding MicroPython `RuntimeError`. Non-exception reasons pass
  through the normal conversion (`reject("failed")` -> reason `"failed"`).
  When an `Error`-typed reason flows *into a Python handler* it converts to
  `{}` (documented JS Error -> MP limitation; `p.result()` still formats it
  properly).
- **Lifetime:** `resolve`/`reject` are attached to the Context exactly like
  other wrappers. Dropping them (`del`, GC order irrelevant) never affects
  pending work; their internal JS references are released by `ctx.close()`
  with everything else. After `close()`, calling `resolve()`/`reject()`
  raises `RuntimeError("context closed")` (and the returned promise's
  methods do too) — no dangling access, no reliance on finaliser order.
- Calling `resolve`/`reject` while the Context is executing elsewhere keeps
  the `context is busy` protection for nested JS execution.

### async / await (native QuickJS execution model)

QuickJS implements `async function` / `await` natively (an internal state
machine + the same promise reaction job queue this module already pumps),
so **no extra engine wiring was needed** — Phase 8 verified the whole model
on top of the existing `run_jobs()` / wrappers / callback registry /
`ctx.promise()` integration:

```python
ctx.eval("async function foo() { return 42; }")
p = ctx.get("foo")()       # p is the ordinary Promise wrapper
ctx.run_jobs()
assert p.result() == 42
```

Observed semantics (pinned by the tests):

- **Async functions return a Promise wrapper.** If the body completes with
  *no* suspension point, the result promise settles **synchronously** —
  `p.done() is True` right after the call and no job is queued. With any
  `await`, the promise stays pending until `ctx.run_jobs()`.
- **`await` composes with every promise source**: JS promises
  (`Promise.resolve`), Python-created promises (`ctx.promise()` — resolving
  from Python then draining settles the awaiting async function), and
  awaited Python callbacks.
- **Python callbacks may be awaited.** A callback that *returns a Promise
  wrapper* of the same Context is assimilated by the native `PromiseResolve`
  path (the `mp_to_quickjs` pass-through), so `await py_cb()` suspends until
  the returned promise settles. A Python exception raised inside an awaited
  callback rejects the async function's promise.
- **Microtask drain is complete**: one `ctx.run_jobs()` call executes *every*
  pending job, including jobs enqueued while draining (`.then` chains,
  nested awaits, callbacks resolving other promises mid-drain). Afterwards
  `has_pending_jobs()` is `False`. The return value is the job count
  (`0` when nothing was pending); call it repeatedly in a `while`-loop if you
  prefer being independent of a single-call guarantee.
- **Error model**: async `throw`, awaited rejections, callback exceptions,
  conversion failures, timeout and OOM all surface as rejection *state*
  (via `p.result()`) or as a MicroPython exception — never as a stale JS
  pending exception. The Context stays immediately usable
  (`ctx.eval("1 + 2") == 3` right after any of them).
- **Timeout covers the job pump** (`ctx.set_time_limit`): an infinite
  `await` loop or a synchronous loop inside a `.then` job is interrupted;
  `run_jobs()` raises `JavaScript execution timeout` and the Context remains
  reusable (remaining jobs stay queued and can be drained later).
- **`ctx.close()` inside a job is refused** (`context is busy`, same phase-6
  rule; visible as a rejection if uncaught, or catchable inside the
  callback), and after a successful `close()` every access path
  (`run_jobs` / `done` / `result`) reports `context closed`.
- **Lifetime**: dropping the async-function wrapper, the returned promise
  wrapper, or even the only Python reference to the Context (promise
  wrappers hold the Context alive) never strands a pending chain — drain
  still completes. GC of pending promises is safe (wrapper token model).
- Stress parity: 1000 async/await invocations and 1000-link promise chains
  drain fully with no persistent JS or MicroPython heap growth (see the
  Phase 8 tests).

### Unhandled promise rejection diagnostics

`ctx.set_unhandled_rejection_handler(callable_or_None)` bridges the native
QuickJS `JS_SetHostPromiseRejectionTracker()` (runtime level, one tracker
per Context) to a Python callback:

```python
ctx.set_unhandled_rejection_handler(handler)   # handler(reason, is_handled)
ctx.eval("Promise.reject('boom')")             # handler called with ("boom", False)
ctx.run_jobs()                                  # diagnostic: never raises
```

Guaranteed semantics (pinned by the Phase 9 tests, verified against the
vendored source):

- **Events are synchronous, not asynchronous**: there is no "unhandled
  rejection job". `is_handled == False` fires at the rejection instant
  (inside `eval`/`call`/`run_jobs`/`resolve`/`reject`), and
  `is_handled == True` fires at the instant a `.then`/`.catch` handler is
  attached to an already-rejected promise. A rejection that is promptly
  caught produces both events, in order; a never-caught rejection produces
  just the first.
- **Diagnostic, not execution error**: `run_jobs()` never raises for an
  unhandled rejection (the phase 8 job semantics are untouched).
- Only promises that are rejected and not yet handled produce events. A
  promise that is pending when a handler attaches is already "handled" and
  never reports.
- QuickJS idiosyncrasy (pinned, not "fixed"): attaching a fulfil-only
  `.then()` to a rejected promise also marks it handled and fires the
  `is_handled == True` event.
- `reason` is a **borrowed** JSValue converted immediately through the
  normal conversion layer (str/number/dict/list pass through; a JS `Error`
  converts to `{}` — the same documented limitation as everywhere else;
  `p.result()` still formats the Error properly).
- **Handler exceptions are swallowed and recorded** (kept on the internal
  handler node, GC-safe): a MicroPython exception can never cross the
  QuickJS C stack from inside the tracker (which may be called mid-
  `fulfill_or_reject_promise`). Later events keep being delivered; the
  Context stays fully usable.
- **Reentrancy**: the tracker runs inside an active execution window, so a
  handler may call `eval`/`run_jobs` (nested, order preserved) and calling
  `ctx.close()` inside it is refused with `RuntimeError("context is busy")`.
- **Lifetime**: the handler is rooted by a GC-scanned node (same model as
  the callback registry — no static MP GC root). Replacement frees the old
  node, `None` unregisters the tracker, and `ctx.close()` unregisters the
  tracker and frees the node so no callback ever fires after close.
- The default singleton (`quickjs.eval`) has no tracker.

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
micropython tests/test_quickjs_phase7.py   # python-side promise creation/control (ctx.promise)
micropython tests/test_quickjs_phase8.py   # async/await execution model (native, no engine changes)
micropython tests/test_quickjs_phase9.py   # unhandled rejection diagnostics (set_unhandled_rejection_handler)
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
