# MicroPython QuickJS - Comprehensive Test Runner
#
# Runs all functional test suites in sequence and reports results.

import gc

test_modules = [
    "test_basic",
    "test_convert",
    "test_function",
    "test_promise",
    "test_bytecode",
    "test_module",
    "test_console",
    "test_web",
    "test_asyncio",
    "test_lifecycle",
]

print("==============================================")
print("Running MicroPython QuickJS Full Test Suite")
print("==============================================")

for mod_name in test_modules:
    print(f"\n---> Running {mod_name}...")
    __import__(mod_name)
    gc.collect()

print("\n==============================================")
print("ALL TESTS PASSED SUCCESSFULLY!")
print("==============================================")
