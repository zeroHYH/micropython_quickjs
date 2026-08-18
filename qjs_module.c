#include "modquickjs.h"
#include "py/reader.h"

/* -------------------------------------------------------------------------- */
/* ES Module Evaluation Helper                                                */
/* -------------------------------------------------------------------------- */

mp_obj_t quickjs_eval_module_helper(quickjs_ctx_t *state, const char *js_code,
                                    size_t len, const char *filename) {
  JSContext *qctx = (state != NULL) ? state->ctx : ctx;

  if (qctx == NULL) {
    mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("context closed"));
  }

  quickjs_ctx_arm_timeout(state);

  JSValue fun_obj =
      JS_Eval(qctx, js_code, len, (filename != NULL) ? filename : "<module>",
              JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);

  if (JS_IsException(fun_obj)) {
    bool timed_out = quickjs_ctx_finish_timeout(state);
    quickjs_raise_exception_state(qctx, fun_obj, timed_out);
    return mp_const_none;
  }

  if (JS_ResolveModule(qctx, fun_obj) < 0) {
    JS_FreeValue(qctx, fun_obj);
    bool timed_out = quickjs_ctx_finish_timeout(state);
    quickjs_raise_exception_state(qctx, JS_UNDEFINED, timed_out);
    return mp_const_none;
  }

  /* JS_EvalFunction takes ownership of fun_obj and frees it on all paths */
  JSValue result = JS_EvalFunction(qctx, fun_obj);
  bool timed_out = quickjs_ctx_finish_timeout(state);

  if (JS_IsException(result)) {
    quickjs_raise_exception_state(qctx, result, timed_out);
    return mp_const_none;
  }

  quickjs_convert_state_t st;
  memset(&st, 0, sizeof(st));

  mp_obj_t mp_result = quickjs_to_mp_owned(qctx, result, result, &st);
  JS_FreeValue(qctx, result);
  return mp_result;
}

/* -------------------------------------------------------------------------- */
/* ES Module Loader Callback (Custom Python Loader & VFS Fallback)            */
/* -------------------------------------------------------------------------- */

JSModuleDef *quickjs_module_loader_cb(JSContext *qctx, const char *module_name,
                                      void *opaque) {
  quickjs_ctx_t *state = (quickjs_ctx_t *)opaque;

  if (state == NULL || state->closed || state->ctx == NULL) {
    JS_ThrowReferenceError(qctx, "context closed");
    return NULL;
  }

  /* 1. Custom Python Module Loader */
  if (state->module_loader != MP_OBJ_NULL &&
      mp_obj_is_callable(state->module_loader)) {
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
      mp_obj_t name_obj = mp_obj_new_str(module_name, strlen(module_name));
      mp_obj_t ret = mp_call_function_1(state->module_loader, name_obj);
      nlr_pop();

      if (mp_obj_is_str(ret)) {
        size_t len = 0;
        const char *src = mp_obj_str_get_data(ret, &len);
        JSValue val = JS_Eval(qctx, src, len, module_name,
                              JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
        if (JS_IsException(val)) {
          return NULL;
        }
        JSModuleDef *m = (JSModuleDef *)JS_VALUE_GET_PTR(val);
        JS_FreeValue(qctx, val);
        return m;
      } else if (mp_obj_is_type(ret, &mp_type_bytes) ||
                 mp_obj_is_type(ret, &mp_type_bytearray)) {
        mp_buffer_info_t bufinfo;
        if (mp_get_buffer(ret, &bufinfo, MP_BUFFER_READ)) {
          JSValue val = JS_ReadObject(qctx, (const uint8_t *)bufinfo.buf,
                                      bufinfo.len, JS_READ_OBJ_BYTECODE);
          if (JS_IsException(val)) {
            return NULL;
          }
          JSModuleDef *m = (JSModuleDef *)JS_VALUE_GET_PTR(val);
          JS_FreeValue(qctx, val);
          return m;
        }
      }
    } else {
      /* Python exception during loader invocation */
      mp_obj_t exc = (mp_obj_t)nlr.ret_val;
      const char *type_name = mp_obj_get_type_str(exc);
      JS_ThrowReferenceError(qctx, "module loader failed for '%s' (%s)",
                             module_name, type_name);
      return NULL;
    }
  }

  /* 2. MicroPython VFS File Loader Fallback */
  mp_reader_t reader;
  nlr_buf_t nlr_file;
  if (nlr_push(&nlr_file) == 0) {
    qstr q_name = qstr_from_str(module_name);
    mp_reader_new_file(&reader, q_name);
    nlr_pop();
  } else {
    JS_ThrowReferenceError(qctx, "could not load module '%s'", module_name);
    return NULL;
  }

  vstr_t vstr;
  vstr_init(&vstr, 256);
  while (reader.readbyte != NULL) {
    int c = reader.readbyte(reader.data);
    if (c < 0) {
      break;
    }
    vstr_add_byte(&vstr, c);
  }
  reader.close(reader.data);

  JSValue val = JS_Eval(qctx, vstr_str(&vstr), vstr_len(&vstr), module_name,
                        JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
  vstr_clear(&vstr);

  if (JS_IsException(val)) {
    return NULL;
  }

  JSModuleDef *m = (JSModuleDef *)JS_VALUE_GET_PTR(val);
  JS_FreeValue(qctx, val);
  return m;
}
