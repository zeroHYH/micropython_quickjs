#include "modquickjs.h"

/* -------------------------------------------------------------------------- */
/* Conversion depth limit and circular reference detection                    */
/* -------------------------------------------------------------------------- */

quickjs_convert_err_t quickjs_convert_push(quickjs_convert_state_t *st,
                                           const void *id) {
  if (st->depth >= QUICKJS_MAX_CONVERSION_DEPTH) {
    return QUICKJS_CONV_TOO_DEEP;
  }

  for (size_t i = 0; i < st->depth; i++) {
    if (st->active[i] == id) {
      return QUICKJS_CONV_CYCLE;
    }
  }

  st->active[st->depth++] = id;

  return QUICKJS_CONV_OK;
}

void quickjs_convert_pop(quickjs_convert_state_t *st) { st->depth--; }

void quickjs_convert_push_mp(quickjs_convert_state_t *st, const void *id) {
  quickjs_convert_err_t err = quickjs_convert_push(st, id);

  if (err == QUICKJS_CONV_TOO_DEEP) {

    mp_raise_msg(&mp_type_RuntimeError,
                 MP_ERROR_TEXT("maximum conversion depth exceeded"));
  }

  if (err == QUICKJS_CONV_CYCLE) {

    mp_raise_msg(&mp_type_RuntimeError,
                 MP_ERROR_TEXT("circular reference detected"));
  }
}

bool quickjs_convert_push_js(JSContext *qctx, quickjs_convert_state_t *st,
                             const void *id) {
  quickjs_convert_err_t err = quickjs_convert_push(st, id);

  if (err == QUICKJS_CONV_TOO_DEEP) {

    JS_ThrowTypeError(qctx, "maximum conversion depth exceeded");

    return false;
  }

  if (err == QUICKJS_CONV_CYCLE) {

    JS_ThrowTypeError(qctx, "circular reference detected");

    return false;
  }

  return true;
}

/* -------------------------------------------------------------------------- */
/* JS -> MicroPython conversion helpers                                       */
/* -------------------------------------------------------------------------- */

mp_obj_t quickjs_to_mp_owned(JSContext *ctx, JSValueConst val, JSValue owned,
                             quickjs_convert_state_t *st) {
  nlr_buf_t nlr;

  if (nlr_push(&nlr) == 0) {

    mp_obj_t result = quickjs_to_mp_obj(ctx, val, st);

    nlr_pop();

    return result;
  }

  JS_FreeValue(ctx, owned);

  nlr_raise(nlr.ret_val);

  return mp_const_none; /* unreachable */
}

mp_obj_t quickjs_string_to_mp(JSContext *ctx, JSValueConst val) {
  size_t len = 0;

  const char *str = JS_ToCStringLen(ctx, &len, val);

  if (str == NULL) {

    quickjs_clear_pending_exception(ctx);

    mp_raise_msg(&mp_type_RuntimeError,
                 MP_ERROR_TEXT("failed to convert JS string"));
  }

  nlr_buf_t nlr;

  if (nlr_push(&nlr) == 0) {

    mp_obj_t result = mp_obj_new_str(str, len);

    nlr_pop();

    JS_FreeCString(ctx, str);

    return result;

  } else {

    JS_FreeCString(ctx, str);

    nlr_raise(nlr.ret_val);
  }

  return mp_const_none; /* unreachable */
}

mp_obj_t quickjs_array_to_mp(JSContext *ctx, JSValueConst val,
                             quickjs_convert_state_t *st) {
  uint32_t len = 0;

  JSValue length_val = JS_GetPropertyStr(ctx, val, "length");

  if (JS_IsException(length_val)) {

    quickjs_raise_exception(ctx, length_val);

    return mp_const_none;
  }

  if (JS_ToUint32(ctx, &len, length_val) < 0) {

    JS_FreeValue(ctx, length_val);

    quickjs_raise_exception(ctx, JS_UNDEFINED);

    return mp_const_none;
  }

  JS_FreeValue(ctx, length_val);

  mp_obj_t *items = NULL;
  bool pushed = false;

  nlr_buf_t nlr;

  if (nlr_push(&nlr) == 0) {

    quickjs_convert_push_mp(st, JS_VALUE_GET_PTR(val));

    pushed = true;

    if (len > 0) {
      items = m_new(mp_obj_t, len);
    }

    for (uint32_t i = 0; i < len; i++) {

      JSValue item = JS_GetPropertyUint32(ctx, val, i);

      if (JS_IsException(item)) {

        quickjs_raise_exception(ctx, item);
      }

      items[i] = quickjs_to_mp_owned(ctx, item, item, st);

      JS_FreeValue(ctx, item);
    }

    nlr_pop();

  } else {

    if (pushed) {
      quickjs_convert_pop(st);
    }

    if (items != NULL) {
      m_del(mp_obj_t, items, len);
    }

    nlr_raise(nlr.ret_val);
  }

  if (pushed) {
    quickjs_convert_pop(st);
  }

  mp_obj_t result = mp_obj_new_list(len, items);

  if (items != NULL) {
    m_del(mp_obj_t, items, len);
  }

  return result;
}

mp_obj_t quickjs_object_to_mp(JSContext *ctx, JSValueConst val,
                              quickjs_convert_state_t *st) {
  JSPropertyEnum *props = NULL;
  uint32_t prop_count = 0;

  int ret = JS_GetOwnPropertyNames(ctx, &props, &prop_count, val,
                                   JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY);

  if (ret < 0) {

    quickjs_raise_exception(ctx, JS_UNDEFINED);

    return mp_const_none;
  }

  mp_obj_t result = MP_OBJ_NULL;
  bool pushed = false;

  nlr_buf_t outer;

  if (nlr_push(&outer) == 0) {

    result = mp_obj_new_dict(prop_count);

    quickjs_convert_push_mp(st, JS_VALUE_GET_PTR(val));

    pushed = true;

    for (uint32_t i = 0; i < prop_count; i++) {

      JSAtom atom = props[i].atom;

      JSValue key_val = JS_AtomToString(ctx, atom);

      if (JS_IsException(key_val)) {

        quickjs_clear_pending_exception(ctx);

        continue;
      }

      size_t key_len = 0;

      const char *key = JS_ToCStringLen(ctx, &key_len, key_val);

      if (key == NULL) {

        quickjs_clear_pending_exception(ctx);

        JS_FreeValue(ctx, key_val);

        continue;
      }

      JSValue value = JS_GetProperty(ctx, val, atom);

      if (JS_IsException(value)) {

        quickjs_clear_pending_exception(ctx);

        JS_FreeCString(ctx, key);

        JS_FreeValue(ctx, key_val);

        continue;
      }

      {
        nlr_buf_t nlr;

        if (nlr_push(&nlr) == 0) {

          mp_obj_t mp_key = mp_obj_new_str(key, key_len);

          mp_obj_t mp_value = quickjs_to_mp_obj(ctx, value, st);

          mp_obj_dict_store(MP_OBJ_TO_PTR(result), mp_key, mp_value);

          nlr_pop();

        } else {

          JS_FreeValue(ctx, value);

          JS_FreeCString(ctx, key);

          JS_FreeValue(ctx, key_val);

          nlr_raise(nlr.ret_val);
        }
      }

      JS_FreeValue(ctx, value);

      JS_FreeCString(ctx, key);

      JS_FreeValue(ctx, key_val);
    }

    nlr_pop();

  } else {

    if (pushed) {
      quickjs_convert_pop(st);
    }

    JS_FreePropertyEnum(ctx, props, prop_count);

    nlr_raise(outer.ret_val);
  }

  if (pushed) {
    quickjs_convert_pop(st);
  }

  JS_FreePropertyEnum(ctx, props, prop_count);

  return result;
}

mp_obj_t quickjs_arraybuffer_to_mp(JSContext *ctx, JSValueConst val) {
  size_t size = 0;

  uint8_t *data = JS_GetArrayBuffer(ctx, &size, val);

  if (data == NULL) {

    quickjs_raise_exception(ctx, JS_UNDEFINED);

    return mp_const_none;
  }

  return mp_obj_new_bytes(data, size);
}

mp_obj_t quickjs_typedarray_to_mp(JSContext *ctx, JSValueConst val) {
  size_t off = 0;
  size_t len = 0;
  size_t bpe = 0;

  JSValue abuf = JS_GetTypedArrayBuffer(ctx, val, &off, &len, &bpe);

  if (JS_IsException(abuf)) {

    quickjs_raise_exception(ctx, abuf);

    return mp_const_none;
  }

  size_t size = 0;

  uint8_t *data = JS_GetArrayBuffer(ctx, &size, abuf);

  if (data == NULL) {

    JS_FreeValue(ctx, abuf);

    quickjs_raise_exception(ctx, JS_UNDEFINED);

    return mp_const_none;
  }

  nlr_buf_t nlr;

  if (nlr_push(&nlr) == 0) {

    mp_obj_t result = mp_obj_new_bytes(data + off, len);

    nlr_pop();

    JS_FreeValue(ctx, abuf);

    return result;

  } else {

    JS_FreeValue(ctx, abuf);

    nlr_raise(nlr.ret_val);
  }

  return mp_const_none; /* unreachable */
}

mp_obj_t quickjs_map_to_mp(JSContext *ctx, JSValueConst val,
                           quickjs_convert_state_t *st) {
  JSValue entries_fn = JS_GetPropertyStr(ctx, val, "entries");
  if (JS_IsException(entries_fn) || !JS_IsFunction(ctx, entries_fn)) {
    JS_FreeValue(ctx, entries_fn);
    quickjs_clear_pending_exception(ctx);
    return mp_obj_new_dict(0);
  }

  JSValue iter = JS_Call(ctx, entries_fn, val, 0, NULL);
  JS_FreeValue(ctx, entries_fn);
  if (JS_IsException(iter)) {
    quickjs_clear_pending_exception(ctx);
    return mp_obj_new_dict(0);
  }

  JSValue next_fn = JS_GetPropertyStr(ctx, iter, "next");
  if (JS_IsException(next_fn) || !JS_IsFunction(ctx, next_fn)) {
    JS_FreeValue(ctx, next_fn);
    JS_FreeValue(ctx, iter);
    quickjs_clear_pending_exception(ctx);
    return mp_obj_new_dict(0);
  }

  mp_obj_t result = mp_obj_new_dict(0);
  bool pushed = false;
  nlr_buf_t nlr;

  if (nlr_push(&nlr) == 0) {
    quickjs_convert_push_mp(st, JS_VALUE_GET_PTR(val));
    pushed = true;

    for (;;) {
      JSValue step = JS_Call(ctx, next_fn, iter, 0, NULL);
      if (JS_IsException(step)) {
        quickjs_clear_pending_exception(ctx);
        break;
      }
      JSValue done_val = JS_GetPropertyStr(ctx, step, "done");
      bool done = JS_ToBool(ctx, done_val);
      JS_FreeValue(ctx, done_val);
      if (done) {
        JS_FreeValue(ctx, step);
        break;
      }
      JSValue entry = JS_GetPropertyStr(ctx, step, "value");
      JS_FreeValue(ctx, step);

      if (!JS_IsException(entry)) {
        JSValue k = JS_GetPropertyUint32(ctx, entry, 0);
        JSValue v = JS_GetPropertyUint32(ctx, entry, 1);
        mp_obj_t mp_k = quickjs_to_mp_obj(ctx, k, st);
        mp_obj_t mp_v = quickjs_to_mp_obj(ctx, v, st);
        mp_obj_dict_store(result, mp_k, mp_v);
        JS_FreeValue(ctx, k);
        JS_FreeValue(ctx, v);
        JS_FreeValue(ctx, entry);
      }
    }
    nlr_pop();
  } else {
    if (pushed) {
      quickjs_convert_pop(st);
    }
    JS_FreeValue(ctx, next_fn);
    JS_FreeValue(ctx, iter);
    nlr_raise(nlr.ret_val);
  }

  if (pushed) {
    quickjs_convert_pop(st);
  }
  JS_FreeValue(ctx, next_fn);
  JS_FreeValue(ctx, iter);
  return result;
}

mp_obj_t quickjs_set_to_mp(JSContext *ctx, JSValueConst val,
                           quickjs_convert_state_t *st) {
  JSValue values_fn = JS_GetPropertyStr(ctx, val, "values");
  if (JS_IsException(values_fn) || !JS_IsFunction(ctx, values_fn)) {
    JS_FreeValue(ctx, values_fn);
    quickjs_clear_pending_exception(ctx);
    return mp_obj_new_set(0, NULL);
  }

  JSValue iter = JS_Call(ctx, values_fn, val, 0, NULL);
  JS_FreeValue(ctx, values_fn);
  if (JS_IsException(iter)) {
    quickjs_clear_pending_exception(ctx);
    return mp_obj_new_set(0, NULL);
  }

  JSValue next_fn = JS_GetPropertyStr(ctx, iter, "next");
  if (JS_IsException(next_fn) || !JS_IsFunction(ctx, next_fn)) {
    JS_FreeValue(ctx, next_fn);
    JS_FreeValue(ctx, iter);
    quickjs_clear_pending_exception(ctx);
    return mp_obj_new_set(0, NULL);
  }

  mp_obj_t result = mp_obj_new_set(0, NULL);
  bool pushed = false;
  nlr_buf_t nlr;

  if (nlr_push(&nlr) == 0) {
    quickjs_convert_push_mp(st, JS_VALUE_GET_PTR(val));
    pushed = true;

    for (;;) {
      JSValue step = JS_Call(ctx, next_fn, iter, 0, NULL);
      if (JS_IsException(step)) {
        quickjs_clear_pending_exception(ctx);
        break;
      }
      JSValue done_val = JS_GetPropertyStr(ctx, step, "done");
      bool done = JS_ToBool(ctx, done_val);
      JS_FreeValue(ctx, done_val);
      if (done) {
        JS_FreeValue(ctx, step);
        break;
      }
      JSValue item = JS_GetPropertyStr(ctx, step, "value");
      JS_FreeValue(ctx, step);

      if (!JS_IsException(item)) {
        mp_obj_t mp_item = quickjs_to_mp_obj(ctx, item, st);
        mp_obj_set_store(result, mp_item);
        JS_FreeValue(ctx, item);
      }
    }
    nlr_pop();
  } else {
    if (pushed) {
      quickjs_convert_pop(st);
    }
    JS_FreeValue(ctx, next_fn);
    JS_FreeValue(ctx, iter);
    nlr_raise(nlr.ret_val);
  }

  if (pushed) {
    quickjs_convert_pop(st);
  }
  JS_FreeValue(ctx, next_fn);
  JS_FreeValue(ctx, iter);
  return result;
}

mp_obj_t quickjs_to_mp_obj(JSContext *ctx, JSValueConst val,
                           quickjs_convert_state_t *st) {
  int tag = JS_VALUE_GET_TAG(val);

  switch (tag) {

  case JS_TAG_UNDEFINED:
    return mp_const_none;

  case JS_TAG_NULL:
    return mp_const_none;

  case JS_TAG_BOOL: {
    int b = JS_ToBool(ctx, val);
    return mp_obj_new_bool(b);
  }

  case JS_TAG_INT: {
    int32_t value = JS_VALUE_GET_INT(val);
    return mp_obj_new_int(value);
  }

  case JS_TAG_STRING:
  case JS_TAG_STRING_ROPE:
    return quickjs_string_to_mp(ctx, val);

  case JS_TAG_SYMBOL: {
    JSAtom atom = JS_ValueToAtom(ctx, val);
    const char *str = JS_AtomToCString(ctx, atom);
    JS_FreeAtom(ctx, atom);
    if (str == NULL || str[0] == '\0') {
      if (str != NULL) {
        JS_FreeCString(ctx, str);
      }
      return mp_obj_new_str("Symbol()", 8);
    }
    vstr_t sv;
    vstr_init(&sv, 32);
    vstr_add_str(&sv, "Symbol(");
    vstr_add_str(&sv, str);
    vstr_add_byte(&sv, ')');
    JS_FreeCString(ctx, str);
    return mp_obj_new_str_from_vstr(&sv);
  }

  case JS_TAG_BIG_INT:
  case JS_TAG_SHORT_BIG_INT:
    return quickjs_bigint_to_mp(ctx, val);

  case JS_TAG_OBJECT: {
    if (JS_IsPromise(val)) {
      return quickjs_promise_to_mp(ctx, val);
    }

    if (JS_IsFunction(ctx, val)) {
      return quickjs_function_to_mp(ctx, val);
    }

    if (JS_IsDate(val)) {
      JSValue iso_fn = JS_GetPropertyStr(ctx, val, "toISOString");
      if (!JS_IsException(iso_fn) && JS_IsFunction(ctx, iso_fn)) {
        JSValue iso_val = JS_Call(ctx, iso_fn, val, 0, NULL);
        JS_FreeValue(ctx, iso_fn);
        if (!JS_IsException(iso_val)) {
          size_t len = 0;
          const char *iso_str = JS_ToCStringLen(ctx, &len, iso_val);
          mp_obj_t result =
              (iso_str != NULL) ? mp_obj_new_str(iso_str, len) : mp_const_none;
          if (iso_str != NULL) {
            JS_FreeCString(ctx, iso_str);
          }
          JS_FreeValue(ctx, iso_val);
          return result;
        }
      }
      JS_FreeValue(ctx, iso_fn);
      quickjs_clear_pending_exception(ctx);
    }

    if (JS_IsRegExp(val)) {
      size_t len = 0;
      const char *re_str = JS_ToCStringLen(ctx, &len, val);
      if (re_str != NULL) {
        mp_obj_t result = mp_obj_new_str(re_str, len);
        JS_FreeCString(ctx, re_str);
        return result;
      }
      quickjs_clear_pending_exception(ctx);
    }

    if (JS_IsMap(val)) {
      return quickjs_map_to_mp(ctx, val, st);
    }

    if (JS_IsSet(val)) {
      return quickjs_set_to_mp(ctx, val, st);
    }

    if (JS_IsArray(val)) {
      return quickjs_array_to_mp(ctx, val, st);
    }

    if (JS_IsArrayBuffer(val)) {
      return quickjs_arraybuffer_to_mp(ctx, val);
    }

    {
      int ta = JS_GetTypedArrayType(val);

      if (ta >= 0) {
        return quickjs_typedarray_to_mp(ctx, val);
      }
    }

    return quickjs_object_to_mp(ctx, val, st);
  }

  default:
    break;
  }

  if (JS_IsNumber(val)) {

    double value;

    if (JS_ToFloat64(ctx, &value, val) < 0) {

      quickjs_clear_pending_exception(ctx);

      mp_raise_msg(&mp_type_TypeError,
                   MP_ERROR_TEXT("failed to convert JS number"));
    }

    return mp_obj_new_float(value);
  }

  mp_raise_msg(&mp_type_TypeError,
               MP_ERROR_TEXT("unsupported QuickJS value type"));

  return mp_const_none;
}

/* -------------------------------------------------------------------------- */
/* MicroPython -> JS                                                          */
/* -------------------------------------------------------------------------- */

JSValue mp_str_to_quickjs(JSContext *ctx, mp_obj_t obj) {
  size_t len = 0;

  const char *str = mp_obj_str_get_data(obj, &len);

  return JS_NewStringLen(ctx, str, len);
}

JSValue mp_list_to_quickjs(JSContext *ctx, mp_obj_t obj,
                           quickjs_convert_state_t *st) {
  size_t len;
  mp_obj_t *items;

  mp_obj_get_array(obj, &len, &items);

  JSValue array = JS_NewArray(ctx);

  if (JS_IsException(array)) {
    return array;
  }

  if (!quickjs_convert_push_js(ctx, st, MP_OBJ_TO_PTR(obj))) {

    JS_FreeValue(ctx, array);

    return JS_EXCEPTION;
  }

  for (size_t i = 0; i < len; i++) {

    JSValue value = mp_to_quickjs(ctx, items[i], st);

    if (JS_IsException(value)) {

      JS_FreeValue(ctx, array);
      quickjs_convert_pop(st);

      return JS_EXCEPTION;
    }

    if (JS_SetPropertyUint32(ctx, array, i, value) < 0) {

      JS_FreeValue(ctx, array);
      quickjs_convert_pop(st);

      return JS_EXCEPTION;
    }
  }

  quickjs_convert_pop(st);

  return array;
}

JSValue mp_tuple_to_quickjs(JSContext *ctx, mp_obj_t obj,
                            quickjs_convert_state_t *st) {
  size_t len;
  mp_obj_t *items;

  mp_obj_get_array(obj, &len, &items);

  JSValue array = JS_NewArray(ctx);

  if (JS_IsException(array)) {
    return array;
  }

  if (!quickjs_convert_push_js(ctx, st, MP_OBJ_TO_PTR(obj))) {

    JS_FreeValue(ctx, array);

    return JS_EXCEPTION;
  }

  for (size_t i = 0; i < len; i++) {

    JSValue value = mp_to_quickjs(ctx, items[i], st);

    if (JS_IsException(value)) {

      JS_FreeValue(ctx, array);
      quickjs_convert_pop(st);

      return JS_EXCEPTION;
    }

    if (JS_SetPropertyUint32(ctx, array, i, value) < 0) {

      JS_FreeValue(ctx, array);
      quickjs_convert_pop(st);

      return JS_EXCEPTION;
    }
  }

  quickjs_convert_pop(st);

  return array;
}

JSValue mp_dict_to_quickjs(JSContext *ctx, mp_obj_t obj,
                           quickjs_convert_state_t *st) {
  mp_map_t *map = mp_obj_dict_get_map(obj);

  JSValue object = JS_NewObject(ctx);

  if (JS_IsException(object)) {
    return object;
  }

  if (!quickjs_convert_push_js(ctx, st, MP_OBJ_TO_PTR(obj))) {

    JS_FreeValue(ctx, object);

    return JS_EXCEPTION;
  }

  for (size_t i = 0; i < map->alloc; i++) {

    if (!mp_map_slot_is_filled(map, i)) {

      continue;
    }

    mp_map_elem_t *elem = &map->table[i];

    if (elem->value == MP_OBJ_NULL) {

      continue;
    }

    mp_obj_t key = elem->key;

    mp_obj_t value_obj = elem->value;

    if (!mp_obj_is_str(key)) {

      JS_FreeValue(ctx, object);
      quickjs_convert_pop(st);

      return JS_ThrowTypeError(ctx, "JS object keys must be str");
    }

    size_t key_len;

    const char *key_str = mp_obj_str_get_data(key, &key_len);

    JSValue value = mp_to_quickjs(ctx, value_obj, st);

    if (JS_IsException(value)) {

      JS_FreeValue(ctx, object);
      quickjs_convert_pop(st);

      return JS_EXCEPTION;
    }

    if (JS_SetPropertyStr(ctx, object, key_str, value) < 0) {

      JS_FreeValue(ctx, object);
      quickjs_convert_pop(st);

      return JS_EXCEPTION;
    }
  }

  quickjs_convert_pop(st);

  return object;
}

JSValue mp_to_quickjs_impl(JSContext *ctx, mp_obj_t obj,
                           quickjs_convert_state_t *st) {
  if (obj == mp_const_none) {

    return JS_NULL;
  }

  if (mp_obj_is_bool(obj)) {

    bool value = mp_obj_is_true(obj);

    return JS_NewBool(ctx, value);
  }

  if (mp_obj_is_int(obj)) {

    mp_int_t value = mp_obj_get_int(obj);

    if (value >= INT32_MIN && value <= INT32_MAX) {

      return JS_NewInt32(ctx, (int32_t)value);
    }

    return JS_NewFloat64(ctx, (double)value);
  }

  if (mp_obj_is_float(obj)) {

    mp_float_t value = mp_obj_get_float(obj);

    return JS_NewFloat64(ctx, (double)value);
  }

  if (mp_obj_is_str(obj)) {

    return mp_str_to_quickjs(ctx, obj);
  }

  if (mp_obj_is_type(obj, &mp_type_list)) {

    return mp_list_to_quickjs(ctx, obj, st);
  }

  if (mp_obj_is_type(obj, &mp_type_tuple)) {

    return mp_tuple_to_quickjs(ctx, obj, st);
  }

  if (mp_obj_is_type(obj, &mp_type_dict)) {

    return mp_dict_to_quickjs(ctx, obj, st);
  }

  if (mp_obj_is_type(obj, &mp_type_bytes)) {

    mp_buffer_info_t bufinfo;

    if (!mp_get_buffer(obj, &bufinfo, MP_BUFFER_READ)) {

      return JS_ThrowTypeError(ctx, "cannot read bytes buffer");
    }

    return JS_NewArrayBufferCopy(ctx, (const uint8_t *)bufinfo.buf,
                                 bufinfo.len);
  }

  if (mp_obj_is_type(obj, &mp_type_bytearray)) {

    mp_buffer_info_t bufinfo;

    if (!mp_get_buffer(obj, &bufinfo, MP_BUFFER_READ)) {

      return JS_ThrowTypeError(ctx, "cannot read bytearray buffer");
    }

    return JS_NewUint8ArrayCopy(ctx, (const uint8_t *)bufinfo.buf, bufinfo.len);
  }

  if (mp_obj_is_type(obj, &quickjs_function_type)) {

    return quickjs_function_pass_through(ctx, obj);
  }

  if (mp_obj_is_type(obj, &quickjs_promise_type)) {

    return quickjs_promise_pass_through(ctx, obj);
  }

  if (mp_obj_is_type(obj, &quickjs_bigint_type)) {

    return quickjs_bigint_wrapper_to_js(ctx, obj);
  }

  return JS_ThrowTypeError(ctx, "unsupported MicroPython type");
}

JSValue mp_to_quickjs(JSContext *qctx, mp_obj_t obj,
                      quickjs_convert_state_t *st) {
  nlr_buf_t nlr;

  if (nlr_push(&nlr) == 0) {

    JSValue result = mp_to_quickjs_impl(qctx, obj, st);

    nlr_pop();

    return result;
  }

  mp_obj_t exc = (mp_obj_t)nlr.ret_val;

  const char *type_name = mp_obj_get_type_str(exc);

  const char *msg = NULL;

  nlr_buf_t nlr2;

  if (nlr_push(&nlr2) == 0) {

    mp_obj_t args_arr[1];
    args_arr[0] = exc;

    mp_obj_t s = (mp_obj_t)mp_obj_str_make_new(&mp_type_str, 1, 0, args_arr);

    msg = mp_obj_str_get_str(s);

    nlr_pop();

  } else {

    msg = NULL;
  }

  JS_ThrowTypeError(qctx, "%s: %s", type_name,
                    (msg != NULL) ? msg : "conversion error");

  return JS_EXCEPTION;
}
