#include "modquickjs.h"
#include "py/reader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------- */
/* bjson (Binary JSON) Implementation                                         */
/* -------------------------------------------------------------------------- */

static JSValue js_bjson_write(JSContext *ctx, JSValueConst this_val, int argc,
                              JSValueConst *argv) {
  (void)this_val;
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "write requires 1 argument");
  }
  int flags = 0;
  if (argc > 1) {
    JS_ToInt32(ctx, &flags, argv[1]);
  }
  size_t len = 0;
  uint8_t *buf = JS_WriteObject(ctx, &len, argv[0], flags);
  if (buf == NULL) {
    return JS_EXCEPTION;
  }
  JSValue arr = JS_NewUint8ArrayCopy(ctx, buf, len);
  js_free(ctx, buf);
  return arr;
}

static JSValue js_bjson_read(JSContext *ctx, JSValueConst this_val, int argc,
                             JSValueConst *argv) {
  (void)this_val;
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "read requires 1 argument");
  }
  int flags = 0;
  if (argc > 1) {
    JS_ToInt32(ctx, &flags, argv[1]);
  }
  size_t size = 0, off = 0, len = 0, bpe = 0;
  uint8_t *data = NULL;
  if (JS_IsArrayBuffer(argv[0])) {
    data = JS_GetArrayBuffer(ctx, &size, argv[0]);
    len = size;
  } else {
    JSValue abuf = JS_GetTypedArrayBuffer(ctx, argv[0], &off, &len, &bpe);
    if (!JS_IsException(abuf)) {
      data = JS_GetArrayBuffer(ctx, &size, abuf);
      JS_FreeValue(ctx, abuf);
    }
  }
  if (data == NULL) {
    return JS_ThrowTypeError(ctx, "Expected Uint8Array or ArrayBuffer");
  }
  return JS_ReadObject(ctx, data + off, len, flags);
}

/* -------------------------------------------------------------------------- */
/* std Module (loadFile, writeFile, loadScript, sprintf, printf, puts, etc.)  */
/* -------------------------------------------------------------------------- */

static JSValue js_std_load_file(JSContext *ctx, JSValueConst this_val, int argc,
                                JSValueConst *argv) {
  (void)this_val;
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "loadFile requires filename");
  }
  const char *filename = JS_ToCString(ctx, argv[0]);
  if (filename == NULL) {
    return JS_EXCEPTION;
  }

  mp_reader_t reader;
  nlr_buf_t nlr_file;
  if (nlr_push(&nlr_file) == 0) {
    qstr q_name = qstr_from_str(filename);
    mp_reader_new_file(&reader, q_name);
    nlr_pop();

    vstr_t fbuf;
    vstr_init(&fbuf, 256);
    while (reader.readbyte != NULL) {
      int c = reader.readbyte(reader.data);
      if (c < 0) {
        break;
      }
      vstr_add_byte(&fbuf, c);
    }
    reader.close(reader.data);
    JS_FreeCString(ctx, filename);

    JSValue res = JS_NewStringLen(ctx, vstr_str(&fbuf), vstr_len(&fbuf));
    vstr_clear(&fbuf);
    return res;
  } else {
    JS_FreeCString(ctx, filename);
    return JS_NULL;
  }
}

static JSValue js_std_write_file(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv) {
  (void)this_val;
  if (argc < 2) {
    return JS_ThrowTypeError(ctx, "writeFile requires (filename, data)");
  }
  const char *filename = JS_ToCString(ctx, argv[0]);
  if (filename == NULL) {
    return JS_EXCEPTION;
  }

  size_t data_len = 0;
  const uint8_t *data_ptr = NULL;
  bool is_str = false;

  if (JS_IsString(argv[1])) {
    data_ptr = (const uint8_t *)JS_ToCStringLen(ctx, &data_len, argv[1]);
    is_str = true;
  } else {
    size_t size = 0, off = 0, len = 0, bpe = 0;
    if (JS_IsArrayBuffer(argv[1])) {
      data_ptr = JS_GetArrayBuffer(ctx, &data_len, argv[1]);
    } else {
      JSValue abuf = JS_GetTypedArrayBuffer(ctx, argv[1], &off, &len, &bpe);
      if (!JS_IsException(abuf)) {
        data_ptr = JS_GetArrayBuffer(ctx, &size, abuf);
        if (data_ptr != NULL) {
          data_ptr += off;
          data_len = len;
        }
        JS_FreeValue(ctx, abuf);
      }
    }
  }

  if (data_ptr == NULL) {
    JS_FreeCString(ctx, filename);
    return JS_ThrowTypeError(ctx, "Expected string or BufferSource for data");
  }

  /* Write via fopen */
  FILE *f = fopen(filename, "wb");
  bool success = false;
  if (f != NULL) {
    size_t written = fwrite(data_ptr, 1, data_len, f);
    fclose(f);
    success = (written == data_len);
  }

  if (is_str) {
    JS_FreeCString(ctx, (const char *)data_ptr);
  }
  JS_FreeCString(ctx, filename);

  return JS_NewBool(ctx, success);
}

static JSValue js_std_load_script(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv) {
  (void)this_val;
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "loadScript requires filename");
  }
  const char *filename = JS_ToCString(ctx, argv[0]);
  if (filename == NULL) {
    return JS_EXCEPTION;
  }

  mp_reader_t reader;
  nlr_buf_t nlr_file;
  if (nlr_push(&nlr_file) == 0) {
    qstr q_name = qstr_from_str(filename);
    mp_reader_new_file(&reader, q_name);
    nlr_pop();

    vstr_t fbuf;
    vstr_init(&fbuf, 256);
    while (reader.readbyte != NULL) {
      int c = reader.readbyte(reader.data);
      if (c < 0) {
        break;
      }
      vstr_add_byte(&fbuf, c);
    }
    reader.close(reader.data);

    JSValue val = JS_Eval(ctx, vstr_str(&fbuf), vstr_len(&fbuf), filename,
                          JS_EVAL_TYPE_GLOBAL);
    vstr_clear(&fbuf);
    JS_FreeCString(ctx, filename);
    return val;
  } else {
    JS_FreeCString(ctx, filename);
    return JS_ThrowReferenceError(ctx, "Could not load script '%s'", filename);
  }
}

static JSValue js_std_sprintf(JSContext *ctx, JSValueConst this_val, int argc,
                              JSValueConst *argv) {
  (void)this_val;
  if (argc < 1) {
    return JS_NewString(ctx, "");
  }
  const char *fmt = JS_ToCString(ctx, argv[0]);
  if (fmt == NULL) {
    return JS_EXCEPTION;
  }

  vstr_t out;
  vstr_init(&out, 64);
  int arg_idx = 1;

  for (const char *p = fmt; *p != '\0'; p++) {
    if (*p == '%' && *(p + 1) != '\0') {
      p++;
      if (*p == '%') {
        vstr_add_byte(&out, '%');
      } else if (*p == 's') {
        if (arg_idx < argc) {
          const char *s = JS_ToCString(ctx, argv[arg_idx++]);
          if (s != NULL) {
            vstr_add_str(&out, s);
            JS_FreeCString(ctx, s);
          }
        }
      } else if (*p == 'd' || *p == 'i') {
        if (arg_idx < argc) {
          int32_t val = 0;
          JS_ToInt32(ctx, &val, argv[arg_idx++]);
          char numbuf[32];
          snprintf(numbuf, sizeof(numbuf), "%d", (int)val);
          vstr_add_str(&out, numbuf);
        }
      } else if (*p == 'x' || *p == 'X') {
        if (arg_idx < argc) {
          uint32_t val = 0;
          JS_ToUint32(ctx, &val, argv[arg_idx++]);
          char numbuf[32];
          snprintf(numbuf, sizeof(numbuf), (*p == 'x') ? "%x" : "%X",
                   (unsigned int)val);
          vstr_add_str(&out, numbuf);
        }
      } else if (*p == 'f' || *p == 'g') {
        if (arg_idx < argc) {
          double val = 0.0;
          JS_ToFloat64(ctx, &val, argv[arg_idx++]);
          char numbuf[32];
          snprintf(numbuf, sizeof(numbuf), "%g", val);
          vstr_add_str(&out, numbuf);
        }
      } else {
        vstr_add_byte(&out, '%');
        vstr_add_byte(&out, *p);
      }
    } else {
      vstr_add_byte(&out, *p);
    }
  }

  JS_FreeCString(ctx, fmt);
  JSValue res = JS_NewStringLen(ctx, vstr_str(&out), vstr_len(&out));
  vstr_clear(&out);
  return res;
}

static JSValue js_std_printf(JSContext *ctx, JSValueConst this_val, int argc,
                             JSValueConst *argv) {
  JSValue s = js_std_sprintf(ctx, this_val, argc, argv);
  if (JS_IsException(s)) {
    return s;
  }
  const char *str = JS_ToCString(ctx, s);
  if (str != NULL) {
    mp_printf(&mp_plat_print, "%s", str);
    JS_FreeCString(ctx, str);
  }
  JS_FreeValue(ctx, s);
  return JS_UNDEFINED;
}

static JSValue js_std_puts(JSContext *ctx, JSValueConst this_val, int argc,
                           JSValueConst *argv) {
  (void)this_val;
  if (argc > 0) {
    const char *str = JS_ToCString(ctx, argv[0]);
    if (str != NULL) {
      mp_printf(&mp_plat_print, "%s\n", str);
      JS_FreeCString(ctx, str);
    }
  }
  return JS_UNDEFINED;
}

static JSValue js_std_gc(JSContext *ctx, JSValueConst this_val, int argc,
                         JSValueConst *argv) {
  (void)this_val;
  (void)argc;
  (void)argv;
  JSRuntime *rt = JS_GetRuntime(ctx);
  if (rt != NULL) {
    JS_RunGC(rt);
  }
  return JS_UNDEFINED;
}

static JSValue js_std_exit(JSContext *ctx, JSValueConst this_val, int argc,
                           JSValueConst *argv) {
  (void)this_val;
  int code = 0;
  if (argc > 0) {
    JS_ToInt32(ctx, &code, argv[0]);
  }
  mp_raise_type_arg(&mp_type_SystemExit, mp_obj_new_int(code));
  return JS_UNDEFINED;
}

static JSValue js_std_getenv(JSContext *ctx, JSValueConst this_val, int argc,
                             JSValueConst *argv) {
  (void)this_val;
  if (argc < 1) {
    return JS_NULL;
  }
  const char *name = JS_ToCString(ctx, argv[0]);
  if (name == NULL) {
    return JS_NULL;
  }
  const char *val = getenv(name);
  JS_FreeCString(ctx, name);
  if (val != NULL) {
    return JS_NewString(ctx, val);
  }
  return JS_NULL;
}

static JSValue js_std_setenv(JSContext *ctx, JSValueConst this_val, int argc,
                             JSValueConst *argv) {
  (void)this_val;
  if (argc < 2) {
    return JS_ThrowTypeError(ctx, "setenv requires (name, value)");
  }
  const char *name = JS_ToCString(ctx, argv[0]);
  const char *val = JS_ToCString(ctx, argv[1]);
  if (name == NULL || val == NULL) {
    if (name) {
      JS_FreeCString(ctx, name);
    }
    if (val) {
      JS_FreeCString(ctx, val);
    }
    return JS_EXCEPTION;
  }
  setenv(name, val, 1);
  JS_FreeCString(ctx, name);
  JS_FreeCString(ctx, val);
  return JS_UNDEFINED;
}

/* -------------------------------------------------------------------------- */
/* Standard Modules Initialization                                            */
/* -------------------------------------------------------------------------- */

void quickjs_init_std_modules(JSContext *ctx) {
  if (ctx == NULL) {
    return;
  }

  JSValue global = JS_GetGlobalObject(ctx);

  /* bjson */
  JSValue bjson = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, bjson, "write",
                    JS_NewCFunction(ctx, js_bjson_write, "write", 1));
  JS_SetPropertyStr(ctx, bjson, "read",
                    JS_NewCFunction(ctx, js_bjson_read, "read", 1));
  JS_SetPropertyStr(ctx, bjson, "READ_OBJ_BYTECODE", JS_NewInt32(ctx, 1));
  JS_SetPropertyStr(ctx, bjson, "WRITE_OBJ_BYTECODE", JS_NewInt32(ctx, 1));
  JS_SetPropertyStr(ctx, bjson, "READ_OBJ_REFERENCE", JS_NewInt32(ctx, 4));
  JS_SetPropertyStr(ctx, bjson, "WRITE_OBJ_REFERENCE", JS_NewInt32(ctx, 4));
  JS_SetPropertyStr(ctx, global, "bjson", bjson);

  /* std */
  JSValue std = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, std, "loadFile",
                    JS_NewCFunction(ctx, js_std_load_file, "loadFile", 1));
  JS_SetPropertyStr(ctx, std, "writeFile",
                    JS_NewCFunction(ctx, js_std_write_file, "writeFile", 2));
  JS_SetPropertyStr(ctx, std, "loadScript",
                    JS_NewCFunction(ctx, js_std_load_script, "loadScript", 1));
  JS_SetPropertyStr(ctx, std, "sprintf",
                    JS_NewCFunction(ctx, js_std_sprintf, "sprintf", 1));
  JS_SetPropertyStr(ctx, std, "printf",
                    JS_NewCFunction(ctx, js_std_printf, "printf", 1));
  JS_SetPropertyStr(ctx, std, "puts",
                    JS_NewCFunction(ctx, js_std_puts, "puts", 1));
  JS_SetPropertyStr(ctx, std, "gc", JS_NewCFunction(ctx, js_std_gc, "gc", 0));
  JS_SetPropertyStr(ctx, std, "exit",
                    JS_NewCFunction(ctx, js_std_exit, "exit", 1));
  JS_SetPropertyStr(ctx, std, "getenv",
                    JS_NewCFunction(ctx, js_std_getenv, "getenv", 1));
  JS_SetPropertyStr(ctx, std, "setenv",
                    JS_NewCFunction(ctx, js_std_setenv, "setenv", 2));
  JS_SetPropertyStr(ctx, global, "std", std);

  JS_FreeValue(ctx, global);
}

/* -------------------------------------------------------------------------- */
/* Fast JSON, BJSON, and Memory Stats Helpers                                 */
/* -------------------------------------------------------------------------- */

mp_obj_t quickjs_eval_json_helper(quickjs_ctx_t *state, const char *json_str,
                                  size_t len) {
  JSContext *qctx = (state != NULL) ? state->ctx : ctx;
  if (qctx == NULL) {
    mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("context closed"));
  }

  JSValue val = JS_ParseJSON(qctx, json_str, len, "<json>");
  if (JS_IsException(val)) {
    quickjs_raise_exception(qctx, val);
    return mp_const_none;
  }

  quickjs_convert_state_t st;
  memset(&st, 0, sizeof(st));
  mp_obj_t res = quickjs_to_mp_owned(qctx, val, val, &st);
  JS_FreeValue(qctx, val);
  return res;
}

mp_obj_t quickjs_bjson_encode_helper(quickjs_ctx_t *state, mp_obj_t obj) {
  JSContext *qctx = (state != NULL) ? state->ctx : ctx;
  if (qctx == NULL) {
    mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("context closed"));
  }

  quickjs_convert_state_t st;
  memset(&st, 0, sizeof(st));
  JSValue val = mp_to_quickjs(qctx, obj, &st);
  if (JS_IsException(val)) {
    quickjs_raise_exception(qctx, val);
    return mp_const_none;
  }

  size_t len = 0;
  uint8_t *buf = JS_WriteObject(qctx, &len, val, 0);
  JS_FreeValue(qctx, val);
  if (buf == NULL) {
    mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("bjson encode failed"));
  }

  mp_obj_t res = mp_obj_new_bytes(buf, len);
  js_free(qctx, buf);
  return res;
}

mp_obj_t quickjs_bjson_decode_helper(quickjs_ctx_t *state, const uint8_t *buf,
                                     size_t len) {
  JSContext *qctx = (state != NULL) ? state->ctx : ctx;
  if (qctx == NULL) {
    mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("context closed"));
  }

  JSValue val = JS_ReadObject(qctx, buf, len, 0);
  if (JS_IsException(val)) {
    quickjs_raise_exception(qctx, val);
    return mp_const_none;
  }

  quickjs_convert_state_t st;
  memset(&st, 0, sizeof(st));
  mp_obj_t res = quickjs_to_mp_owned(qctx, val, val, &st);
  JS_FreeValue(qctx, val);
  return res;
}

mp_obj_t quickjs_memory_stats_helper(JSRuntime *qrt) {
  if (qrt == NULL) {
    mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("context closed"));
  }

  JSMemoryUsage stats;
  JS_ComputeMemoryUsage(qrt, &stats);

  mp_obj_t dict = mp_obj_new_dict(16);
  mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_memory_used_size),
                    mp_obj_new_int_from_ull(stats.memory_used_size));
  mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_malloc_size),
                    mp_obj_new_int_from_ull(stats.malloc_size));
  mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_malloc_limit),
                    mp_obj_new_int_from_ull(stats.malloc_limit));
  mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_malloc_count),
                    mp_obj_new_int_from_ull(stats.malloc_count));
  mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_atoms_count),
                    mp_obj_new_int_from_ull(stats.atom_count));
  mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_atoms_size),
                    mp_obj_new_int_from_ull(stats.atom_size));
  mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_objects_count),
                    mp_obj_new_int_from_ull(stats.obj_count));
  mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_objects_size),
                    mp_obj_new_int_from_ull(stats.obj_size));
  mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_strings_count),
                    mp_obj_new_int_from_ull(stats.str_count));
  mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_strings_size),
                    mp_obj_new_int_from_ull(stats.str_size));
  mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_js_func_count),
                    mp_obj_new_int_from_ull(stats.js_func_count));
  mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_js_func_size),
                    mp_obj_new_int_from_ull(stats.js_func_size));
  mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_js_func_code_size),
                    mp_obj_new_int_from_ull(stats.js_func_code_size));
  mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_c_func_count),
                    mp_obj_new_int_from_ull(stats.c_func_count));
  mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_array_count),
                    mp_obj_new_int_from_ull(stats.array_count));
  mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_fast_array_count),
                    mp_obj_new_int_from_ull(stats.fast_array_count));

  return dict;
}
