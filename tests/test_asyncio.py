# MicroPython QuickJS - asyncio / uasyncio Coroutine Integration Tests
#
# Tests cooperative non-blocking scheduling between MicroPython's asyncio event loop
# and QuickJS Promise microtasks.

import asyncio

import quickjs


def expect(cond, msg="Assertion failed"):
    if not cond:
        raise AssertionError(msg)


async def async_wait(promise, ctx=None, poll_ms=1):
    """Cooperative async waiter for QuickJS promises in asyncio tasks."""
    while not promise.done():
        if ctx is not None:
            ctx.run_jobs()
        else:
            quickjs.run_jobs()
        if promise.done():
            break
        await asyncio.sleep_ms(poll_ms)
    return promise.result()


print("=== Running test_asyncio.py ===")


async def test_single_promise():
    ctx = quickjs.Context()
    p = ctx.eval("""
    async function fetchValue() {
        return await Promise.resolve(42);
    }
    fetchValue();
    """)

    res = await async_wait(p, ctx)
    expect(res == 42, f"Expected 42, got {res}")
    ctx.close()
    print("Single promise async_wait OK")


async def test_concurrent_promises():
    ctx = quickjs.Context()

    p1 = ctx.eval("Promise.resolve('task 1')")
    p2 = ctx.eval("Promise.resolve('task 2')")
    p3 = ctx.eval("Promise.resolve('task 3')")

    async def worker(p, expected):
        val = await async_wait(p, ctx)
        expect(val == expected, f"Expected {expected}, got {val}")
        return val

    results = await asyncio.gather(
        worker(p1, "task 1"),
        worker(p2, "task 2"),
        worker(p3, "task 3"),
    )
    expect(results == ["task 1", "task 2", "task 3"])

    ctx.close()
    print("Concurrent asyncio tasks OK")


async def test_rejected_promise():
    ctx = quickjs.Context()
    p_err = ctx.eval("Promise.reject(new Error('async failure'))")

    caught = False
    try:
        await async_wait(p_err, ctx)
    except RuntimeError as e:
        caught = True
        expect("async failure" in str(e))

    expect(caught is True, "Expected RuntimeError from rejected promise")
    ctx.close()
    print("Rejected promise async_wait OK")


async def main():
    await test_single_promise()
    await test_concurrent_promises()
    await test_rejected_promise()


asyncio.run(main())

print("ALL ASYNCIO TESTS PASSED")
