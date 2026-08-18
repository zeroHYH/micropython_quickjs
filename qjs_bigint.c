#include "modquickjs.h"

/* -------------------------------------------------------------------------- */
/* JS BigInt -> MicroPython int                                               */
/* -------------------------------------------------------------------------- */

bool quickjs_bigint_str_to_i64(const char *s, int64_t *out) {
  bool neg = false;

  if (*s == '-') {
    neg = true;
    s++;
  } else if (*s == '+') {
    s++;
  }

  if (*s == '\0') {
    return false;
  }

  const uint64_t pos_limit = 9223372036854775807ULL;
  const uint64_t neg_limit = 9223372036854775808ULL;
  const uint64_t limit = neg ? neg_limit : pos_limit;

  uint64_t acc = 0;

  while (*s != '\0') {

    if (*s < '0' || *s > '9') {
      return false;
    }

    unsigned d = (unsigned)(*s - '0');

    if (acc > (limit - d) / 10) {
      return false; /* overflow */
    }

    acc = acc * 10 + d;

    s++;
  }

  if (neg) {
    *out = (int64_t)(0 - acc);
  } else {
    *out = (int64_t)acc;
  }

  return true;
}

mp_obj_t quickjs_bigint_to_mp(JSContext *ctx, JSValueConst val) {
  size_t len = 0;

  const char *str = JS_ToCStringLen(ctx, &len, val);

  if (str == NULL) {

    quickjs_clear_pending_exception(ctx);

    mp_raise_msg(&mp_type_RuntimeError,
                 MP_ERROR_TEXT("failed to convert JS BigInt"));
  }

  int64_t result = 0;

  if (!quickjs_bigint_str_to_i64(str, &result)) {

    JS_FreeCString(ctx, str);

    mp_raise_msg(&mp_type_TypeError, MP_ERROR_TEXT("BigInt out of range"));
  }

  JS_FreeCString(ctx, str);

  return mp_obj_new_int_from_ll(result);
}

/* -------------------------------------------------------------------------- */
/* quickjs.bigint() explicit BigInt marker object                             */
/* -------------------------------------------------------------------------- */

void quickjs_bigint_print(const mp_print_t *print, mp_obj_t self_in,
                          mp_print_kind_t kind) {
  mp_obj_quickjs_bigint_t *b = MP_OBJ_TO_PTR(self_in);

  /* Debug friendly: print underlying Python int's decimal value */
  mp_obj_print_helper(print, b->value, kind);
}

MP_DEFINE_CONST_OBJ_TYPE(quickjs_bigint_type, MP_QSTR_bigint, MP_TYPE_FLAG_NONE,
                         print, quickjs_bigint_print);

mp_obj_t mod_quickjs_bigint(mp_obj_t value_obj) {
  if (!mp_obj_is_int(value_obj)) {

    mp_raise_msg(&mp_type_ValueError,
                 MP_ERROR_TEXT("bigint() requires an integer"));
  }

  mp_obj_quickjs_bigint_t *b =
      mp_obj_malloc(mp_obj_quickjs_bigint_t, &quickjs_bigint_type);

  b->value = value_obj;

  return MP_OBJ_FROM_PTR(b);
}

MP_DEFINE_CONST_FUN_OBJ_1(mod_quickjs_bigint_obj, mod_quickjs_bigint);

JSValue quickjs_bigint_wrapper_to_js(JSContext *ctx, mp_obj_t obj) {
  mp_obj_quickjs_bigint_t *b = MP_OBJ_TO_PTR(obj);

  mp_obj_t s_obj = MP_OBJ_NULL;
  const char *s = NULL;

  nlr_buf_t nlr;

  if (nlr_push(&nlr) == 0) {

    s_obj = mp_obj_str_make_new(&mp_type_str, 1, 0, (mp_obj_t[]){b->value});

    s = mp_obj_str_get_str(s_obj);

    nlr_pop();

  } else {

    /* str() failed (OOM): swallow and return JS exception to avoid cross-stack
     * leak */
    JS_ThrowTypeError(ctx, "cannot stringify bigint value");

    return JS_EXCEPTION;
  }

  int64_t i64 = 0;

  if (quickjs_bigint_str_to_i64(s, &i64)) {

    return JS_NewBigInt64(ctx, i64);
  }

  const char *p = s;
  bool neg = false;

  if (*p == '-') {
    neg = true;
    p++;
  } else if (*p == '+') {
    p++;
  }

  if (*p == '\0') {

    JS_ThrowTypeError(ctx, "invalid bigint value");

    return JS_EXCEPTION;
  }

  for (; *p != '\0'; p++) {

    if (*p < '0' || *p > '9') {

      JS_ThrowTypeError(ctx, "invalid bigint value");

      return JS_EXCEPTION;
    }
  }

  const char *digits = s;

  if (neg) {
    digits++;
  }

  size_t dlen = strlen(digits);

  char *buf = m_new(char, dlen + (neg ? 2 : 1) + 1);

  size_t pos = 0;

  if (neg) {
    buf[pos++] = '-';
  }

  memcpy(buf + pos, digits, dlen);

  pos += dlen;

  buf[pos++] = 'n';
  buf[pos] = '\0';

  JSValue v = JS_Eval(ctx, buf, pos, "<bigint>", JS_EVAL_TYPE_GLOBAL);

  m_del(char, buf, dlen + (neg ? 2 : 1) + 1);

  return v;
}
