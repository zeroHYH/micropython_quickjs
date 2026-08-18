#include "modquickjs.h"

/* -------------------------------------------------------------------------- */
/* Python exception -> JS Error object                                       */
/* -------------------------------------------------------------------------- */

JSValue quickjs_exception_to_js_error(JSContext *qctx, mp_obj_t exc) {
  const char *type_name = mp_obj_get_type_str(exc);

  const char *msg = NULL;

  nlr_buf_t nlr;

  if (nlr_push(&nlr) == 0) {

    mp_obj_t args_arr[1];
    args_arr[0] = exc;

    mp_obj_t s = (mp_obj_t)mp_obj_str_make_new(&mp_type_str, 1, 0, args_arr);

    msg = mp_obj_str_get_str(s);

    nlr_pop();

  } else {

    msg = NULL;
  }

  JS_ThrowTypeError(qctx, "%s: %s", type_name,
                    (msg != NULL) ? msg : "exception");

  return JS_GetException(qctx);
}

/* -------------------------------------------------------------------------- */
/* Helper: extract string property of JS error object                         */
/* -------------------------------------------------------------------------- */

static const char *quickjs_error_prop_str(JSContext *ctx, JSValueConst obj,
                                          const char *prop) {
  JSValue v = JS_GetPropertyStr(ctx, obj, prop);

  if (JS_IsException(v)) {

    /* Failed to access property (e.g. Proxy), clear pending exception. */
    quickjs_clear_pending_exception(ctx);

    return NULL;
  }

  int tag = JS_VALUE_GET_TAG(v);

  if (tag != JS_TAG_STRING && tag != JS_TAG_STRING_ROPE) {

    JS_FreeValue(ctx, v);

    return NULL;
  }

  const char *str = JS_ToCString(ctx, v);

  JS_FreeValue(ctx, v);

  if (str == NULL) {

    quickjs_clear_pending_exception(ctx);

    return NULL;
  }

  return str;
}

/* -------------------------------------------------------------------------- */
/* QuickJS exception -> MicroPython exception                                 */
/* -------------------------------------------------------------------------- */

void quickjs_raise_value(JSContext *ctx, JSValue exception_val) {
  /*
   * Format the JSValue exception (or promise rejection reason) held by caller
   * into a MicroPython exception and nlr_raise. Ownership of exception_val is
   * transferred to this function (responsible for freeing it).
   */
  const char *name = NULL;
  const char *message = NULL;
  const char *stack = NULL;

  if (JS_VALUE_GET_TAG(exception_val) == JS_TAG_OBJECT) {

    name = quickjs_error_prop_str(ctx, exception_val, "name");

    message = quickjs_error_prop_str(ctx, exception_val, "message");

    stack = quickjs_error_prop_str(ctx, exception_val, "stack");
  }

  const char *fallback = NULL;

  if (name == NULL && message == NULL) {

    fallback = JS_ToCString(ctx, exception_val);

    if (fallback == NULL) {
      quickjs_clear_pending_exception(ctx);
    }
  }

  vstr_t vstr;
  bool vstr_ok = false;
  mp_obj_t exc = MP_OBJ_NULL;
  nlr_buf_t nlr;

  if (nlr_push(&nlr) == 0) {

    vstr_init(&vstr, 64);
    vstr_ok = true;

    if (name != NULL && message != NULL) {

      vstr_add_str(&vstr, name);
      vstr_add_str(&vstr, ": ");
      vstr_add_str(&vstr, message);

    } else if (name != NULL) {

      vstr_add_str(&vstr, name);

    } else if (message != NULL) {

      vstr_add_str(&vstr, message);

    } else if (fallback != NULL) {

      vstr_add_str(&vstr, fallback);

    } else {

      vstr_add_str(&vstr, "QuickJS exception");
    }

    if (stack != NULL && stack[0] != '\0' && vstr_len(&vstr) > 0) {

      vstr_add_str(&vstr, "\nstack:\n");
      vstr_add_str(&vstr, stack);
    }

    vstr_null_terminated_str(&vstr);

    exc = mp_obj_new_exception_msg_varg(&mp_type_RuntimeError,
                                        MP_ERROR_TEXT("%s"), vstr.buf);

    nlr_pop();

  } else {

    /*
     * Failed to construct exception: free JS resources before rethrowing.
     */
    if (vstr_ok) {
      vstr_clear(&vstr);
    }

    if (name != NULL) {
      JS_FreeCString(ctx, name);
    }
    if (message != NULL) {
      JS_FreeCString(ctx, message);
    }
    if (stack != NULL) {
      JS_FreeCString(ctx, stack);
    }
    if (fallback != NULL) {
      JS_FreeCString(ctx, fallback);
    }

    JS_FreeValue(ctx, exception_val);

    nlr_raise(nlr.ret_val);
  }

  /*
   * Success path: free JS resources before raising.
   * The message has already been copied into the exception object by
   * mp_obj_new_exception_msg_varg.
   */
  vstr_clear(&vstr);

  if (name != NULL) {
    JS_FreeCString(ctx, name);
  }
  if (message != NULL) {
    JS_FreeCString(ctx, message);
  }
  if (stack != NULL) {
    JS_FreeCString(ctx, stack);
  }
  if (fallback != NULL) {
    JS_FreeCString(ctx, fallback);
  }

  JS_FreeValue(ctx, exception_val);

  nlr_raise(exc);
}

void quickjs_raise_exception(JSContext *ctx, JSValue val) {
  JSValue exception_val = JS_GetException(ctx);

  JS_FreeValue(ctx, val);

  if (JS_IsUninitialized(exception_val)) {

    mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("QuickJS exception"));
  }

  quickjs_raise_value(ctx, exception_val);
}

void quickjs_raise_exception_state(JSContext *ctx, JSValue val,
                                   bool timed_out) {
  if (timed_out) {

    JSValue exc = JS_GetException(ctx);

    if (!JS_IsUninitialized(exc)) {

      JS_FreeValue(ctx, exc);
    }

    JS_FreeValue(ctx, val);

    mp_raise_msg(&mp_type_RuntimeError,
                 MP_ERROR_TEXT("JavaScript execution timeout"));
  }

  quickjs_raise_exception(ctx, val);
}

/* -------------------------------------------------------------------------- */
/* Helper: clear pending exception                                            */
/* -------------------------------------------------------------------------- */

void quickjs_clear_pending_exception(JSContext *ctx) {
  JSValue exc = JS_GetException(ctx);

  if (!JS_IsUninitialized(exc)) {

    JS_FreeValue(ctx, exc);
  }
}
