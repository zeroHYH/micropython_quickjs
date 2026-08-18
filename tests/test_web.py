# MicroPython QuickJS - Web Standard APIs & Fast JSON Tests
#
# Tests TextEncoder, TextDecoder, btoa, atob, performance.now, and eval_json().

import quickjs


def expect(cond, msg="Assertion failed"):
    if not cond:
        raise AssertionError(msg)


print("=== Running test_web.py ===")

ctx = quickjs.Context()

# ---------------------------------------------------------------------------
# Section 1: TextEncoder & TextDecoder
# ---------------------------------------------------------------------------
# Basic encoding / decoding
encoded = ctx.eval("new TextEncoder().encode('Hello World')")
expect(isinstance(encoded, bytes))
expect(encoded == b"Hello World", f"Got {encoded}")

decoded = ctx.eval("new TextDecoder().decode(new Uint8Array([72, 101, 108, 108, 111]))")
expect(decoded == "Hello", f"Got {decoded}")

# UTF-8 multibyte characters
utf8_str = "QuickJS in MicroPython 🚀"
ctx.set("input_str", utf8_str)
roundtrip = ctx.eval("""
const enc = new TextEncoder();
const dec = new TextDecoder();
const bytes = enc.encode(input_str);
dec.decode(bytes);
""")
expect(roundtrip == utf8_str, f"Got {roundtrip}")

print("TextEncoder & TextDecoder OK")

# ---------------------------------------------------------------------------
# Section 2: btoa & atob (Base64)
# ---------------------------------------------------------------------------
# Encoding
expect(ctx.eval("btoa('admin:secret')") == "YWRtaW46c2VjcmV0")
expect(ctx.eval("btoa('')") == "")
expect(ctx.eval("btoa('a')") == "YQ==")
expect(ctx.eval("btoa('ab')") == "YWI=")
expect(ctx.eval("btoa('abc')") == "YWJj")

# Decoding
expect(ctx.eval("atob('YWRtaW46c2VjcmV0')") == "admin:secret")
expect(ctx.eval("atob('YQ==')") == "a")
expect(ctx.eval("atob('YWI=')") == "ab")
expect(ctx.eval("atob('YWJj')") == "abc")

# Invalid base64 throws
try:
    ctx.eval("atob('invalid!@#')")
    raise AssertionError("should have thrown")
except RuntimeError as e:
    expect("Invalid base64" in str(e))

print("btoa & atob OK")

# ---------------------------------------------------------------------------
# Section 3: performance.now()
# ---------------------------------------------------------------------------
t0 = ctx.eval("performance.now()")
expect(isinstance(t0, float))
expect(t0 >= 0.0)

t1 = ctx.eval("""
for (let i = 0; i < 1000; i++) Math.sqrt(i);
performance.now();
""")
expect(t1 >= t0)

print("performance.now() OK")

# ---------------------------------------------------------------------------
# Section 4: Fast JSON Evaluation (eval_json)
# ---------------------------------------------------------------------------
# Context.eval_json
json_data = '{"sensor": "DHT22", "values": [23.5, 24.1, 23.8], "active": true}'
res_ctx = ctx.eval_json(json_data)
expect(isinstance(res_ctx, dict))
expect(res_ctx["sensor"] == "DHT22")
expect(res_ctx["values"] == [23.5, 24.1, 23.8])
expect(res_ctx["active"] is True)

# Module quickjs.eval_json
res_mod = quickjs.eval_json('{"status": 200, "message": "OK"}')
expect(res_mod == {"status": 200, "message": "OK"})

# Invalid JSON raises exception
try:
    ctx.eval_json("{ bad json }")
    raise AssertionError("should have thrown")
except RuntimeError:
    pass

ctx.close()
print("Fast JSON evaluation OK")

print("ALL WEB & JSON TESTS PASSED")
