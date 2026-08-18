#include "modquickjs.h"

/* -------------------------------------------------------------------------- */
/* Global singleton runtime and context                                       */
/* -------------------------------------------------------------------------- */

JSRuntime *rt = NULL;
JSContext *ctx = NULL;

/* -------------------------------------------------------------------------- */
/* quickjs.init()                                                             */
/* -------------------------------------------------------------------------- */

static mp_obj_t mod_quickjs_init(void) {

  if (rt != NULL && ctx != NULL) {

    return mp_const_none;
  }

  rt = JS_NewRuntime();

  if (rt == NULL) {

    mp_raise_msg(&mp_type_RuntimeError,
                 MP_ERROR_TEXT("failed to create JS Runtime"));
  }

  /*
   * Default QuickJS heap limit (can be overridden via compile-time macro, see
   * modquickjs.h)
   */
  JS_SetMemoryLimit(rt, QUICKJS_DEFAULT_MEMORY_LIMIT);

  ctx = JS_NewContext(rt);

  if (ctx == NULL) {

    JS_FreeRuntime(rt);

    rt = NULL;

    mp_raise_msg(&mp_type_RuntimeError,
                 MP_ERROR_TEXT("failed to create JS Context"));
  }

  return mp_const_none;
}

static MP_DEFINE_CONST_FUN_OBJ_0(mod_quickjs_init_obj, mod_quickjs_init);

/* -------------------------------------------------------------------------- */
/* quickjs.version()                                                          */
/* -------------------------------------------------------------------------- */

static mp_obj_t mod_quickjs_version(void) {

  const char *ver = JS_GetVersion();

  if (ver == NULL) {
    return mp_const_none;
  }

  return mp_obj_new_str(ver, strlen(ver));
}

static MP_DEFINE_CONST_FUN_OBJ_0(mod_quickjs_version_obj, mod_quickjs_version);

/* -------------------------------------------------------------------------- */
/* quickjs.eval()                                                             */
/* -------------------------------------------------------------------------- */

static mp_obj_t mod_quickjs_eval(mp_obj_t js_code_obj) {
  if (ctx == NULL) {
    mod_quickjs_init();
  }

  const char *js_code = mp_obj_str_get_str(js_code_obj);

  return quickjs_eval_helper(NULL, js_code);
}

static MP_DEFINE_CONST_FUN_OBJ_1(mod_quickjs_eval_obj, mod_quickjs_eval);

/* -------------------------------------------------------------------------- */
/* quickjs.call()                                                             */
/* -------------------------------------------------------------------------- */

static mp_obj_t mod_quickjs_call(size_t n_args, const mp_obj_t *args) {
  if (ctx == NULL) {
    mod_quickjs_init();
  }

  const char *function_name = mp_obj_str_get_str(args[0]);

  return quickjs_call_helper(NULL, function_name, n_args - 1, args + 1);
}

static MP_DEFINE_CONST_FUN_OBJ_VAR(mod_quickjs_call_obj, 1, mod_quickjs_call);

/* -------------------------------------------------------------------------- */
/* quickjs.run_jobs() / has_pending_jobs()                                    */
/* -------------------------------------------------------------------------- */

static mp_obj_t mod_quickjs_run_jobs(void) {

  if (rt == NULL || ctx == NULL) {

    mod_quickjs_init();
  }

  return quickjs_run_jobs_helper(NULL);
}

static MP_DEFINE_CONST_FUN_OBJ_0(mod_quickjs_run_jobs_obj,
                                 mod_quickjs_run_jobs);

static mp_obj_t mod_quickjs_has_pending_jobs(void) {

  if (rt == NULL || ctx == NULL) {

    mod_quickjs_init();
  }

  return mp_obj_new_bool(JS_IsJobPending(rt));
}

static MP_DEFINE_CONST_FUN_OBJ_0(mod_quickjs_has_pending_jobs_obj,
                                 mod_quickjs_has_pending_jobs);

/* -------------------------------------------------------------------------- */
/* quickjs.help()                                                             */
/* -------------------------------------------------------------------------- */

static mp_obj_t mod_quickjs_help(void) {

  mp_printf(
      &mp_plat_print,

      "QuickJS-NG JavaScript Engine for MicroPython\n"
      "============================================\n"
      "Functions:\n"
      "  quickjs.init()\n"
      "      Initialize JS Runtime & Context\n"
      "  quickjs.eval(code)\n"
      "      Execute JavaScript code\n"
      "  quickjs.call(name, *args)\n"
      "      Call a global JavaScript function\n"
      "  quickjs.run_jobs()\n"
      "      Execute pending JS jobs (promise microtasks); returns count\n"
      "  quickjs.has_pending_jobs()\n"
      "      True if there are unexecuted JS jobs\n"
      "  quickjs.version()\n"
      "      Return QuickJS-NG engine version\n"
      "  quickjs.help()\n"
      "      Show this help documentation\n"
      "\n"
      "Context (isolated runtime):\n"
      "  quickjs.Context()\n"
      "      Create an independent JS runtime + context\n"
      "  ctx.eval(code)\n"
      "      Execute JavaScript code\n"
      "  ctx.call(name, *args)\n"
      "      Call a global JavaScript function\n"
      "  ctx.get(name)\n"
      "      Get a global variable (None if missing)\n"
      "  ctx.set(name, value)\n"
      "      Set a global variable\n"
      "  ctx.add_callable(name, callable)\n"
      "      Register a Python callable as a JS function\n"
      "  ctx.gc()\n"
      "      Run the QuickJS garbage collector\n"
      "  ctx._js_mem()\n"
      "      Debug: return QuickJS heap usage in bytes\n"
      "  ctx.run_jobs()\n"
      "      Execute pending JS jobs (promise microtasks); returns count\n"
      "  ctx.has_pending_jobs()\n"
      "      True if there are unexecuted JS jobs\n"
      "  ctx.set_unhandled_rejection_handler(cb_or_None)\n"
      "      Diagnose unhandled promise rejections: cb(reason, is_handled)\n"
      "  ctx.set_memory_limit(bytes)\n"
      "      Set the JS heap limit (0 = unlimited)\n"
      "  ctx.set_max_stack_size(bytes)\n"
      "      Set the JS stack limit (0 = unlimited)\n"
      "  ctx.set_time_limit(ms)\n"
      "      Set the JS execution timeout (0 = disabled)\n"
      "  ctx.close()\n"
      "      Free the runtime (idempotent; also runs on GC)\n"
      "  ctx.promise()\n"
      "      -> (p, resolve, reject): create a pending Promise from Python\n"
      "      (settle with resolve(value) / reject(reason); first call wins,\n"
      "       thenables/other promises are assimilated natively)\n"
      "  ctx.get('func') -> callable wrapper\n"
      "      JS functions convert to Python callables\n"
      "      wrapper.call(this_obj, *args): bind `this`\n"
      "\n"
      "Promise wrapper:\n"
      "  p.done()   -> settled (fulfilled or rejected)?\n"
      "  p.result() -> value, or raises the rejection / 'not settled'\n"
      "  p.then(on_fulfilled=None, on_rejected=None) -> new wrapper\n"
      "  p.catch(on_rejected=None)                    -> new wrapper\n"
      "  p.finally_(callback=None)                    -> new wrapper\n"
      "      (callbacks run via ctx.run_jobs(); return values "
      "resolve/assimilate)\n"
      "\n"
      "BigInt:\n"
      "  quickjs.bigint(value) -> marker object -> JS BigInt on conversion\n"
      "      (arbitrary precision; int64-range uses JS_NewBigInt64)\n"
      "\n"
      "JS -> MicroPython:\n"
      "  undefined -> None\n"
      "  null      -> None\n"
      "  boolean   -> bool\n"
      "  integer   -> int\n"
      "  number    -> float\n"
      "  string    -> str\n"
      "  Array     -> list\n"
      "  Object    -> dict\n"
      "  TypedArray-> bytes (raw byte representation)\n"
      "  Function  -> Python callable (Context)\n"
      "  Promise   -> Promise wrapper (Context): "
      "done/result/then/catch/finally_\n"
      "\n"
      "MicroPython -> JS:\n"
      "  None      -> null\n"
      "  bool      -> boolean\n"
      "  int       -> number\n"
      "  float     -> number\n"
      "  str       -> string\n"
      "  list      -> Array\n"
      "  tuple     -> Array\n"
      "  dict      -> Object\n"
      "  callable  -> Function (via add_callable)\n"
      "  bigint()  -> BigInt\n"
      "\n"
      "Example:\n"
      "  >>> import quickjs\n"
      "  >>> quickjs.init()\n"
      "  >>> quickjs.eval('function add(a,b) { return a+b; }')\n"
      "  >>> quickjs.call('add', 10, 20)\n"
      "  30\n"
      "  >>> quickjs.eval('function f(x) { return {value:x*2}; }')\n"
      "  >>> quickjs.call('f', {'x': 10})\n"
      "\n");

  return mp_const_none;
}

static MP_DEFINE_CONST_FUN_OBJ_0(mod_quickjs_help_obj, mod_quickjs_help);

/* -------------------------------------------------------------------------- */
/* Module globals                                                             */
/* -------------------------------------------------------------------------- */

static const mp_rom_map_elem_t quickjs_module_globals_table[] = {

    {MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_quickjs)},

    {MP_ROM_QSTR(MP_QSTR_help), MP_ROM_PTR(&mod_quickjs_help_obj)},

    {MP_ROM_QSTR(MP_QSTR_init), MP_ROM_PTR(&mod_quickjs_init_obj)},

    {MP_ROM_QSTR(MP_QSTR_eval), MP_ROM_PTR(&mod_quickjs_eval_obj)},

    {MP_ROM_QSTR(MP_QSTR_call), MP_ROM_PTR(&mod_quickjs_call_obj)},

    {MP_ROM_QSTR(MP_QSTR_run_jobs), MP_ROM_PTR(&mod_quickjs_run_jobs_obj)},

    {MP_ROM_QSTR(MP_QSTR_has_pending_jobs),
     MP_ROM_PTR(&mod_quickjs_has_pending_jobs_obj)},

    {MP_ROM_QSTR(MP_QSTR_version), MP_ROM_PTR(&mod_quickjs_version_obj)},

    {MP_ROM_QSTR(MP_QSTR_bigint), MP_ROM_PTR(&mod_quickjs_bigint_obj)},

    {MP_ROM_QSTR(MP_QSTR_Context), MP_ROM_PTR(&quickjs_context_type)},
};

static MP_DEFINE_CONST_DICT(quickjs_module_globals,
                            quickjs_module_globals_table);

/* -------------------------------------------------------------------------- */
/* Module definition                                                          */
/* -------------------------------------------------------------------------- */

const mp_obj_module_t mp_module_quickjs = {

    .base = {&mp_type_module},

    .globals = (mp_obj_dict_t *)&quickjs_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_quickjs, mp_module_quickjs);
