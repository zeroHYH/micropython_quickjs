#include "modquickjs.h"

/* -------------------------------------------------------------------------- */
/* Built-in console Object (log, info, debug, warn, error)                    */
/* -------------------------------------------------------------------------- */

static JSValue js_console_log(JSContext *ctx, JSValueConst this_val, int argc,
                              JSValueConst *argv, int magic) {
  (void)this_val;

  if (magic == 1) {
    mp_printf(&mp_plat_print, "[WARN] ");
  } else if (magic == 2) {
    mp_printf(&mp_plat_print, "[ERROR] ");
  }

  for (int i = 0; i < argc; i++) {
    if (i > 0) {
      mp_printf(&mp_plat_print, " ");
    }
    size_t len = 0;
    const char *str = JS_ToCStringLen(ctx, &len, argv[i]);
    if (str != NULL) {
      mp_printf(&mp_plat_print, "%.*s", (int)len, str);
      JS_FreeCString(ctx, str);
    } else {
      quickjs_clear_pending_exception(ctx);
      mp_printf(&mp_plat_print, "[object]");
    }
  }
  mp_printf(&mp_plat_print, "\n");
  return JS_UNDEFINED;
}

void quickjs_init_console(JSContext *ctx) {
  if (ctx == NULL) {
    return;
  }

  JSValue global = JS_GetGlobalObject(ctx);
  JSValue console = JS_NewObject(ctx);

  /* magic: 0 = log/info/debug, 1 = warn, 2 = error */
  JS_SetPropertyStr(ctx, console, "log",
                    JS_NewCFunctionMagic(ctx,
                                         (JSCFunctionMagic *)js_console_log,
                                         "log", 1, JS_CFUNC_generic_magic, 0));
  JS_SetPropertyStr(ctx, console, "info",
                    JS_NewCFunctionMagic(ctx,
                                         (JSCFunctionMagic *)js_console_log,
                                         "info", 1, JS_CFUNC_generic_magic, 0));
  JS_SetPropertyStr(
      ctx, console, "debug",
      JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)js_console_log, "debug", 1,
                           JS_CFUNC_generic_magic, 0));
  JS_SetPropertyStr(ctx, console, "warn",
                    JS_NewCFunctionMagic(ctx,
                                         (JSCFunctionMagic *)js_console_log,
                                         "warn", 1, JS_CFUNC_generic_magic, 1));
  JS_SetPropertyStr(
      ctx, console, "error",
      JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)js_console_log, "error", 1,
                           JS_CFUNC_generic_magic, 2));

  JS_SetPropertyStr(ctx, global, "console", console);
  JS_FreeValue(ctx, global);
}
