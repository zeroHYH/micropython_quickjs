#include "modquickjs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------- */
/* Base64 btoa / atob Implementation                                          */
/* -------------------------------------------------------------------------- */

static const char b64_chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static JSValue js_btoa(JSContext *ctx, JSValueConst this_val, int argc,
                       JSValueConst *argv) {
  (void)this_val;
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "btoa requires at least 1 argument");
  }
  size_t len = 0;
  const char *src = JS_ToCStringLen(ctx, &len, argv[0]);
  if (src == NULL) {
    return JS_EXCEPTION;
  }

  size_t out_len = 4 * ((len + 2) / 3);
  char *out = (char *)js_malloc(ctx, out_len + 1);
  if (out == NULL) {
    JS_FreeCString(ctx, src);
    return JS_ThrowOutOfMemory(ctx);
  }

  size_t i = 0, j = 0;
  for (i = 0; i < len; i += 3) {
    uint32_t a = (unsigned char)src[i];
    uint32_t b = (i + 1 < len) ? (unsigned char)src[i + 1] : 0;
    uint32_t c = (i + 2 < len) ? (unsigned char)src[i + 2] : 0;

    uint32_t triple = (a << 16) | (b << 8) | c;

    out[j++] = b64_chars[(triple >> 18) & 0x3F];
    out[j++] = b64_chars[(triple >> 12) & 0x3F];
    out[j++] = (i + 1 < len) ? b64_chars[(triple >> 6) & 0x3F] : '=';
    out[j++] = (i + 2 < len) ? b64_chars[triple & 0x3F] : '=';
  }
  out[j] = '\0';
  JS_FreeCString(ctx, src);

  JSValue res = JS_NewStringLen(ctx, out, j);
  js_free(ctx, out);
  return res;
}

static int b64_decode_char(char c) {
  if (c >= 'A' && c <= 'Z') {
    return c - 'A';
  }
  if (c >= 'a' && c <= 'z') {
    return c - 'a' + 26;
  }
  if (c >= '0' && c <= '9') {
    return c - '0' + 52;
  }
  if (c == '+') {
    return 62;
  }
  if (c == '/') {
    return 63;
  }
  return -1;
}

static JSValue js_atob(JSContext *ctx, JSValueConst this_val, int argc,
                       JSValueConst *argv) {
  (void)this_val;
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "atob requires at least 1 argument");
  }
  size_t len = 0;
  const char *src = JS_ToCStringLen(ctx, &len, argv[0]);
  if (src == NULL) {
    return JS_EXCEPTION;
  }

  while (len > 0 && src[len - 1] == '=') {
    len--;
  }

  size_t out_len = (len * 3) / 4;
  char *out = (char *)js_malloc(ctx, out_len + 1);
  if (out == NULL) {
    JS_FreeCString(ctx, src);
    return JS_ThrowOutOfMemory(ctx);
  }

  size_t i = 0, j = 0;
  uint32_t buf = 0;
  int bits = 0;

  for (i = 0; i < len; i++) {
    char c = src[i];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      continue;
    }
    int val = b64_decode_char(c);
    if (val < 0) {
      js_free(ctx, out);
      JS_FreeCString(ctx, src);
      return JS_ThrowTypeError(ctx, "Invalid base64 character");
    }
    buf = (buf << 6) | (uint32_t)val;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out[j++] = (char)((buf >> bits) & 0xFF);
    }
  }

  out[j] = '\0';
  JS_FreeCString(ctx, src);

  JSValue res = JS_NewStringLen(ctx, out, j);
  js_free(ctx, out);
  return res;
}

/* -------------------------------------------------------------------------- */
/* TextEncoder / TextDecoder                                                  */
/* -------------------------------------------------------------------------- */

static JSValue js_text_encoder_encode(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv) {
  (void)this_val;
  size_t len = 0;
  const char *str = (argc > 0 && !JS_IsUndefined(argv[0]))
                        ? JS_ToCStringLen(ctx, &len, argv[0])
                        : "";
  if (str == NULL) {
    return JS_EXCEPTION;
  }
  JSValue arr = JS_NewUint8ArrayCopy(ctx, (const uint8_t *)str, len);
  if (argc > 0 && !JS_IsUndefined(argv[0])) {
    JS_FreeCString(ctx, str);
  }
  return arr;
}

static JSValue js_text_encoder_ctor(JSContext *ctx, JSValueConst new_target,
                                    int argc, JSValueConst *argv) {
  (void)argc;
  (void)argv;
  JSValue proto = JS_GetPropertyStr(ctx, new_target, "prototype");
  if (JS_IsException(proto)) {
    return proto;
  }
  JSValue obj = JS_NewObjectProto(ctx, proto);
  JS_FreeValue(ctx, proto);
  return obj;
}

static JSValue js_text_decoder_decode(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv) {
  (void)this_val;
  if (argc < 1 || JS_IsUndefined(argv[0])) {
    return JS_NewString(ctx, "");
  }
  size_t size = 0;
  size_t off = 0, len = 0, bpe = 0;
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
    return JS_ThrowTypeError(ctx,
                             "Expected BufferSource for TextDecoder.decode");
  }

  return JS_NewStringLen(ctx, (const char *)(data + off), len);
}

static JSValue js_text_decoder_ctor(JSContext *ctx, JSValueConst new_target,
                                    int argc, JSValueConst *argv) {
  (void)argc;
  (void)argv;
  JSValue proto = JS_GetPropertyStr(ctx, new_target, "prototype");
  if (JS_IsException(proto)) {
    return proto;
  }
  JSValue obj = JS_NewObjectProto(ctx, proto);
  JS_FreeValue(ctx, proto);
  return obj;
}

/* -------------------------------------------------------------------------- */
/* performance.now()                                                          */
/* -------------------------------------------------------------------------- */

static JSValue js_performance_now(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv) {
  (void)this_val;
  (void)argc;
  (void)argv;
  mp_uint_t us = mp_hal_ticks_us();
  double ms = (double)us / 1000.0;
  return JS_NewFloat64(ctx, ms);
}

/* -------------------------------------------------------------------------- */
/* crypto (getRandomValues & randomUUID)                                      */
/* -------------------------------------------------------------------------- */

static JSValue js_crypto_get_random_values(JSContext *ctx,
                                           JSValueConst this_val, int argc,
                                           JSValueConst *argv) {
  (void)this_val;
  if (argc < 1) {
    return JS_ThrowTypeError(ctx, "getRandomValues requires TypedArray");
  }
  size_t size = 0, off = 0, len = 0, bpe = 0;
  uint8_t *data = NULL;
  JSValue abuf = JS_GetTypedArrayBuffer(ctx, argv[0], &off, &len, &bpe);
  if (!JS_IsException(abuf)) {
    data = JS_GetArrayBuffer(ctx, &size, abuf);
    JS_FreeValue(ctx, abuf);
  }
  if (data == NULL) {
    return JS_ThrowTypeError(ctx, "Expected TypedArray for getRandomValues");
  }
  size_t byte_len = len * bpe;
  uint8_t *ptr = data + off;
  uint32_t seed = (uint32_t)mp_hal_ticks_us();
  for (size_t i = 0; i < byte_len; i++) {
    seed = seed * 1664525u + 1013904223u;
    ptr[i] = (uint8_t)((seed >> 16) ^ (seed & 0xFF));
  }
  return JS_DupValue(ctx, argv[0]);
}

static JSValue js_crypto_random_uuid(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv) {
  (void)this_val;
  (void)argc;
  (void)argv;
  uint8_t bytes[16];
  uint32_t seed = (uint32_t)mp_hal_ticks_us();
  for (int i = 0; i < 16; i++) {
    seed = seed * 1664525u + 1013904223u;
    bytes[i] = (uint8_t)((seed >> 16) ^ (seed & 0xFF));
  }
  bytes[6] = (bytes[6] & 0x0F) | 0x40; /* version 4 */
  bytes[8] = (bytes[8] & 0x3F) | 0x80; /* variant 1 */

  char uuid[37];
  snprintf(uuid, sizeof(uuid),
           "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%"
           "02x",
           bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6],
           bytes[7], bytes[8], bytes[9], bytes[10], bytes[11], bytes[12],
           bytes[13], bytes[14], bytes[15]);
  return JS_NewStringLen(ctx, uuid, 36);
}

/* -------------------------------------------------------------------------- */
/* Web API Initialization                                                     */
/* -------------------------------------------------------------------------- */

void quickjs_init_web_apis(JSContext *ctx) {
  if (ctx == NULL) {
    return;
  }

  JSValue global = JS_GetGlobalObject(ctx);

  /* btoa and atob */
  JS_SetPropertyStr(ctx, global, "btoa",
                    JS_NewCFunction(ctx, js_btoa, "btoa", 1));
  JS_SetPropertyStr(ctx, global, "atob",
                    JS_NewCFunction(ctx, js_atob, "atob", 1));

  /* TextEncoder */
  JSValue enc_proto = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, enc_proto, "encode",
                    JS_NewCFunction(ctx, js_text_encoder_encode, "encode", 1));
  JSValue enc_ctor =
      JS_NewCFunction2(ctx, (JSCFunction *)js_text_encoder_ctor, "TextEncoder",
                       0, JS_CFUNC_constructor_or_func, 0);
  JS_SetConstructor(ctx, enc_ctor, enc_proto);
  JS_SetPropertyStr(ctx, global, "TextEncoder", enc_ctor);
  JS_FreeValue(ctx, enc_proto);

  /* TextDecoder */
  JSValue dec_proto = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, dec_proto, "decode",
                    JS_NewCFunction(ctx, js_text_decoder_decode, "decode", 1));
  JSValue dec_ctor =
      JS_NewCFunction2(ctx, (JSCFunction *)js_text_decoder_ctor, "TextDecoder",
                       0, JS_CFUNC_constructor_or_func, 0);
  JS_SetConstructor(ctx, dec_ctor, dec_proto);
  JS_SetPropertyStr(ctx, global, "TextDecoder", dec_ctor);
  JS_FreeValue(ctx, dec_proto);

  /* performance */
  JSValue perf = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, perf, "now",
                    JS_NewCFunction(ctx, js_performance_now, "now", 0));
  JS_SetPropertyStr(ctx, global, "performance", perf);

  /* crypto */
  JSValue crypto = JS_NewObject(ctx);
  JS_SetPropertyStr(
      ctx, crypto, "getRandomValues",
      JS_NewCFunction(ctx, js_crypto_get_random_values, "getRandomValues", 1));
  JS_SetPropertyStr(
      ctx, crypto, "randomUUID",
      JS_NewCFunction(ctx, js_crypto_random_uuid, "randomUUID", 0));
  JS_SetPropertyStr(ctx, global, "crypto", crypto);

  /* URL & URLSearchParams lightweight bootstrap */
  const char *url_bootstrap =
      "(function(global){"
      "class URLSearchParams {"
      "  constructor(init='') {"
      "    this._p = [];"
      "    if(typeof init==='string') {"
      "      if(init.startsWith('?')) init = init.slice(1);"
      "      if(init) {"
      "        for(const pair of init.split('&')) {"
      "          const idx = pair.indexOf('=');"
      "          const k = idx>=0 ? pair.slice(0,idx) : pair;"
      "          const v = idx>=0 ? pair.slice(idx+1) : '';"
      "          this._p.push([decodeURIComponent(k), decodeURIComponent(v)]);"
      "        }"
      "      }"
      "    } else if(init && typeof init==='object') {"
      "      for(const [k,v] of Object.entries(init)) {"
      "        this._p.push([String(k), String(v)]);"
      "      }"
      "    }"
      "  }"
      "  get(n){ const p=this._p.find(([k])=>k===n); return p?p[1]:null; }"
      "  set(n,v){ const i=this._p.findIndex(([k])=>k===n); if(i>=0) "
      "this._p[i]=[n,String(v)]; else this._p.push([n,String(v)]); }"
      "  append(n,v){ this._p.push([n,String(v)]); }"
      "  has(n){ return this._p.some(([k])=>k===n); }"
      "  delete(n){ this._p=this._p.filter(([k])=>k!==n); }"
      "  toString(){ return "
      "this._p.map(([k,v])=>`${encodeURIComponent(k)}=${encodeURIComponent(v)}`"
      ")"
      ".join('&'); }"
      "}"
      "class URL {"
      "  constructor(url, base) {"
      "    let f = String(url);"
      "    if(base && !f.includes('://')) {"
      "      f = base.replace(/\\/+$/, '') + '/' + f.replace(/^\\/+/, '');"
      "    }"
      "    const m = "
      "f.match(/^(?:([a-zA-Z]+:))?(?:\\/\\/([^\\/?#:]+)(?::(\\d+))?)?([^?#]*)(?"
      ":\\?([^#]*))?(?:#(.*))?$/);"
      "    if(!m) throw new TypeError('Invalid URL');"
      "    this.protocol = m[1] || '';"
      "    this.hostname = m[2] || '';"
      "    this.port = m[3] || '';"
      "    this.host = this.port ? `${this.hostname}:${this.port}` : "
      "this.hostname;"
      "    this.pathname = m[4] || '/';"
      "    this.search = m[5] ? `?${m[5]}` : '';"
      "    this.hash = m[6] ? `#${m[6]}` : '';"
      "    this.href = f;"
      "    this.searchParams = new URLSearchParams(this.search);"
      "  }"
      "}"
      "global.URLSearchParams = URLSearchParams;"
      "global.URL = URL;"
      "})(globalThis);";

  JSValue bres = JS_Eval(ctx, url_bootstrap, strlen(url_bootstrap), "<web>",
                         JS_EVAL_TYPE_GLOBAL);
  JS_FreeValue(ctx, bres);

  JS_FreeValue(ctx, global);
}
