# MicroPython QuickJS - System & Web Standard API Tests
#
# Tests crypto (getRandomValues/randomUUID), std module, and URL/URLSearchParams.

import quickjs


def expect(cond, msg="Assertion failed"):
    if not cond:
        raise AssertionError(msg)


print("=== Running test_sys_web.py ===")

ctx = quickjs.Context()

# ---------------------------------------------------------------------------
# Section 1: crypto (getRandomValues & randomUUID)
# ---------------------------------------------------------------------------
# getRandomValues
res = ctx.eval("""
const buf = new Uint8Array(16);
crypto.getRandomValues(buf);
let nonZero = 0;
for (let i = 0; i < buf.length; i++) {
    if (buf[i] !== 0) nonZero++;
}
nonZero > 8;
""")
expect(res is True)

# randomUUID
uuid = ctx.eval("crypto.randomUUID()")
expect(isinstance(uuid, str))
expect(len(uuid) == 36)
expect(uuid[8] == "-" and uuid[13] == "-" and uuid[18] == "-" and uuid[23] == "-")
expect(uuid[14] == "4")  # v4 UUID

print("crypto.getRandomValues & crypto.randomUUID OK")

# ---------------------------------------------------------------------------
# Section 2: std Module (sprintf, printf, puts, gc, getenv, setenv)
# ---------------------------------------------------------------------------
# sprintf
s = ctx.eval("std.sprintf('Temp: %d C, Hex: 0x%x', 25, 255)")
expect(s == "Temp: 25 C, Hex: 0xff")

# puts and printf (execute without throwing)
ctx.eval("std.puts('Testing std.puts from JS');")
ctx.eval("std.printf('Testing std.printf: %s %d\\n', 'value', 42);")

# gc
ctx.eval("std.gc();")

# getenv & setenv
ctx.eval("std.setenv('QJS_TEST_VAR', 'hello_world');")
expect(ctx.eval("std.getenv('QJS_TEST_VAR')") == "hello_world")

print("std module utilities (sprintf, puts, gc, env) OK")

# ---------------------------------------------------------------------------
# Section 3: std File I/O (writeFile, loadFile, loadScript)
# ---------------------------------------------------------------------------
tmp_file = "/tmp/qjs_test_file.txt"
tmp_js = "/tmp/qjs_test_script.js"

# writeFile & loadFile
expect(ctx.eval(f"std.writeFile('{tmp_file}', 'quickjs-file-content');") is True)
loaded = ctx.eval(f"std.loadFile('{tmp_file}');")
expect(loaded == "quickjs-file-content")

# loadScript
expect(
    ctx.eval(f"std.writeFile('{tmp_js}', 'globalThis.loadedScriptVal = 42 * 2;');")
    is True
)
ctx.eval(f"std.loadScript('{tmp_js}');")
expect(ctx.eval("globalThis.loadedScriptVal") == 84)

print("std file I/O (loadFile, writeFile, loadScript) OK")

# ---------------------------------------------------------------------------
# Section 4: URL & URLSearchParams
# ---------------------------------------------------------------------------
res = ctx.eval("""
const u = new URL("https://example.com:8080/api/sensor?device=esp32&rate=100#section1");
({
    protocol: u.protocol,
    hostname: u.hostname,
    port: u.port,
    host: u.host,
    pathname: u.pathname,
    search: u.search,
    hash: u.hash,
    device: u.searchParams.get("device"),
    rate: u.searchParams.get("rate")
})
""")
expect(res["protocol"] == "https:")
expect(res["hostname"] == "example.com")
expect(res["port"] == "8080")
expect(res["host"] == "example.com:8080")
expect(res["pathname"] == "/api/sensor")
expect(res["search"] == "?device=esp32&rate=100")
expect(res["hash"] == "#section1")
expect(res["device"] == "esp32")
expect(res["rate"] == "100")

# URLSearchParams mutation
ctx.eval("""
const p = new URLSearchParams("a=1&b=2");
p.set("a", "10");
p.append("c", "3");
p.delete("b");
globalThis.paramStr = p.toString();
""")
expect(ctx.get("paramStr") == "a=10&c=3")

ctx.close()
print("URL & URLSearchParams OK")

print("ALL SYSTEM & WEB API TESTS PASSED")
