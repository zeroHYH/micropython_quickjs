#include "modquickjs.h"

/* -------------------------------------------------------------------------- */
/* Promise wrapper resource cleanup                                           */
/* -------------------------------------------------------------------------- */

mp_obj_t mod_quickjs_promise_del(mp_obj_t self_in) {
  mp_obj_quickjs_promise_t *p = MP_OBJ_TO_PTR(self_in);

  if (p->state != NULL && p->state->ctx != NULL) {

    quickjs_value_entry_t **pp = &p->state->promise_entries;

    while (*pp != NULL) {

      if ((*pp)->token == p->token) {

        quickjs_value_entry_t *e = *pp;

        *pp = e->next;

        JS_FreeValue(p->state->ctx, e->val);

        m_del(quickjs_value_entry_t, e, 1);

        break;
      }

      pp = &(*pp)->next;
    }
  }

  return mp_const_none;
}

static MP_DEFINE_CONST_FUN_OBJ_1(mod_quickjs_promise_del_obj,
                                 mod_quickjs_promise_del);

/*
 * Phase 5 (GC/lifecycle audit fix): release all wrapper JS values held by
 * state.
 */
void quickjs_ctx_release_entries(quickjs_ctx_t *state) {
  if (state->ctx == NULL) {

    return;
  }

  /* Function wrapper entries */
  quickjs_value_entry_t *e = state->function_entries;

  while (e != NULL) {

    quickjs_value_entry_t *next = e->next;

    JS_FreeValue(state->ctx, e->val);

    m_del(quickjs_value_entry_t, e, 1);

    e = next;
  }

  state->function_entries = NULL;

  /* Promise wrapper entries */
  e = state->promise_entries;

  while (e != NULL) {

    quickjs_value_entry_t *next = e->next;

    JS_FreeValue(state->ctx, e->val);

    m_del(quickjs_value_entry_t, e, 1);

    e = next;
  }

  state->promise_entries = NULL;

  /* Phase 7: resolve/reject wrapper entries (ctx.promise()) */
  e = state->resolver_entries;

  while (e != NULL) {

    quickjs_value_entry_t *next = e->next;

    JS_FreeValue(state->ctx, e->val);

    m_del(quickjs_value_entry_t, e, 1);

    e = next;
  }

  state->resolver_entries = NULL;
}

/* -------------------------------------------------------------------------- */
/* Token lookup and Pass-through                                              */
/* -------------------------------------------------------------------------- */

JSValue quickjs_promise_lookup(mp_obj_quickjs_promise_t *p) {
  quickjs_ctx_t *state = p->state;

  if (state == NULL || state->closed || state->ctx == NULL ||
      state->rt == NULL) {

    mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("context closed"));
  }

  for (quickjs_value_entry_t *e = state->promise_entries; e != NULL;
       e = e->next) {

    if (e->token == p->token) {

      return e->val;
    }
  }

  mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("context closed"));

  return JS_UNDEFINED; /* unreachable */
}

JSValue quickjs_promise_lookup_jserr(JSContext *ctx,
                                     mp_obj_quickjs_promise_t *p) {
  quickjs_ctx_t *state = p->state;

  if (state == NULL || state->closed || state->ctx == NULL ||
      state->rt == NULL) {

    return JS_ThrowTypeError(ctx, "context closed");
  }

  if (state->ctx != ctx) {

    return JS_ThrowTypeError(ctx, "promise belongs to another context");
  }

  for (quickjs_value_entry_t *e = state->promise_entries; e != NULL;
       e = e->next) {

    if (e->token == p->token) {

      return e->val;
    }
  }

  return JS_ThrowTypeError(ctx, "context closed");
}

JSValue quickjs_promise_pass_through(JSContext *ctx, mp_obj_t obj) {
  mp_obj_quickjs_promise_t *p = (mp_obj_quickjs_promise_t *)MP_OBJ_TO_PTR(obj);

  JSValue v = quickjs_promise_lookup_jserr(ctx, p);

  if (JS_IsException(v)) {

    return v;
  }

  return JS_DupValue(ctx, v);
}

/* -------------------------------------------------------------------------- */
/* Promise methods: done / result                                             */
/* -------------------------------------------------------------------------- */

mp_obj_t mod_quickjs_promise_done(mp_obj_t self_in) {
  mp_obj_quickjs_promise_t *p = MP_OBJ_TO_PTR(self_in);

  quickjs_ctx_t *state = p->state;

  if (state == NULL || state->closed || state->ctx == NULL ||
      state->rt == NULL) {

    mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("context closed"));
  }

  JSPromiseStateEnum s = JS_PromiseState(state->ctx, quickjs_promise_lookup(p));

  if (s == JS_PROMISE_NOT_A_PROMISE) {

    mp_raise_msg(&mp_type_TypeError, MP_ERROR_TEXT("not a promise"));
  }

  return mp_obj_new_bool(s != JS_PROMISE_PENDING);
}

static MP_DEFINE_CONST_FUN_OBJ_1(mod_quickjs_promise_done_obj,
                                 mod_quickjs_promise_done);

static mp_obj_t mod_quickjs_promise_result_impl(quickjs_ctx_t *state,
                                                mp_obj_quickjs_promise_t *p) {
  JSValue pval = quickjs_promise_lookup(p);

  JSPromiseStateEnum s = JS_PromiseState(state->ctx, pval);

  if (s == JS_PROMISE_NOT_A_PROMISE) {

    mp_raise_msg(&mp_type_TypeError, MP_ERROR_TEXT("not a promise"));
  }

  if (s == JS_PROMISE_PENDING) {

    mp_raise_msg(&mp_type_RuntimeError,
                 MP_ERROR_TEXT("promise not settled; call ctx.run_jobs()"));
  }

  JSValue r = JS_PromiseResult(state->ctx, pval);

  if (s == JS_PROMISE_FULFILLED) {

    quickjs_convert_state_t st;
    memset(&st, 0, sizeof(st));

    mp_obj_t result = quickjs_to_mp_owned(state->ctx, r, r, &st);

    JS_FreeValue(state->ctx, r);

    return result;
  }

  quickjs_raise_value(state->ctx, r);

  return mp_const_none; /* unreachable */
}

mp_obj_t mod_quickjs_promise_result(mp_obj_t self_in) {
  mp_obj_quickjs_promise_t *p = MP_OBJ_TO_PTR(self_in);

  quickjs_ctx_t *state = p->state;

  if (state == NULL || state->closed || state->ctx == NULL ||
      state->rt == NULL) {

    mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("context closed"));
  }

  quickjs_ctx_enter(state);

  nlr_buf_t nlr;

  if (nlr_push(&nlr) == 0) {

    mp_obj_t result = mod_quickjs_promise_result_impl(state, p);

    nlr_pop();

    quickjs_ctx_leave(state);

    return result;

  } else {

    quickjs_ctx_leave(state);

    nlr_raise(nlr.ret_val);
  }

  return mp_const_none; /* unreachable */
}

static MP_DEFINE_CONST_FUN_OBJ_1(mod_quickjs_promise_result_obj,
                                 mod_quickjs_promise_result);

/* -------------------------------------------------------------------------- */
/* Promise bidirectional bridging: handler construction and p.then / catch /
 * finally_ */
/* -------------------------------------------------------------------------- */

JSValue quickjs_new_handler_closure(quickjs_ctx_t *state, mp_obj_t callable) {
  quickjs_callback_t *node = m_new_obj(quickjs_callback_t);

  memset(node, 0, sizeof(*node));

  node->state = state;
  node->callable = callable;
  node->name = MP_OBJ_NULL; /* Anonymous: does not participate in add_callable
                               overwrite matching */
  node->opaque_active = true;
  node->js_func =
      JS_UNDEFINED; /* Registry does not strongly reference closure */

  JSValue func =
      JS_NewCClosure(state->ctx, quickjs_callback, "<promise-handler>",
                     quickjs_cb_finalize, 0, 0, node);

  if (JS_IsException(func)) {

    quickjs_callback_free_node(node);

    return func;
  }

  node->next = state->callbacks;
  state->callbacks = node;

  return func;
}

JSValue quickjs_promise_handler_to_js(quickjs_ctx_t *state, JSContext *qctx,
                                      mp_obj_t handler) {
  if (handler == mp_const_none) {

    return JS_UNDEFINED;
  }

  if (mp_obj_is_type(handler, &quickjs_function_type)) {

    return quickjs_function_pass_through(qctx, handler);
  }

  if (mp_obj_is_callable(handler)) {

    return quickjs_new_handler_closure(state, handler);
  }

  return JS_ThrowTypeError(qctx, "handler must be callable or None");
}

bool quickjs_promise_handler_check(mp_obj_t handler) {
  return (handler == mp_const_none ||
          mp_obj_is_type(handler, &quickjs_function_type) ||
          mp_obj_is_callable(handler));
}

mp_obj_t mod_quickjs_promise_then(size_t n_args, const mp_obj_t *args) {
  mp_obj_quickjs_promise_t *p = MP_OBJ_TO_PTR(args[0]);

  quickjs_ctx_t *state = p->state;

  if (state == NULL || state->closed || state->ctx == NULL ||
      state->rt == NULL) {

    mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("context closed"));
  }

  mp_obj_t on_fulfilled = (n_args > 1) ? args[1] : mp_const_none;
  mp_obj_t on_rejected = (n_args > 2) ? args[2] : mp_const_none;

  if (!quickjs_promise_handler_check(on_fulfilled) ||
      !quickjs_promise_handler_check(on_rejected)) {

    mp_raise_msg(&mp_type_TypeError,
                 MP_ERROR_TEXT("handler must be callable or None"));
  }

  quickjs_ctx_enter(state);

  nlr_buf_t nlr;

  if (nlr_push(&nlr) == 0) {

    JSContext *qctx = state->ctx;

    JSValue pval = quickjs_promise_lookup(p);

    JSValue f_js = quickjs_promise_handler_to_js(state, qctx, on_fulfilled);

    if (JS_IsException(f_js)) {

      quickjs_raise_exception(qctx, f_js);
    }

    JSValue r_js = quickjs_promise_handler_to_js(state, qctx, on_rejected);

    if (JS_IsException(r_js)) {

      JS_FreeValue(qctx, f_js);

      quickjs_raise_exception(qctx, r_js);
    }

    JSValue derived = JS_PromiseThen(qctx, pval, f_js, r_js);

    JS_FreeValue(qctx, f_js);
    JS_FreeValue(qctx, r_js);

    if (JS_IsException(derived)) {

      quickjs_raise_exception(qctx, derived);
    }

    mp_obj_t result = quickjs_promise_wrap_owned(qctx, derived);

    nlr_pop();

    quickjs_ctx_leave(state);

    return result;

  } else {

    quickjs_ctx_leave(state);

    nlr_raise(nlr.ret_val);
  }

  return mp_const_none; /* unreachable */
}

MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mod_quickjs_promise_then_obj, 1, 3,
                                    mod_quickjs_promise_then);

mp_obj_t mod_quickjs_promise_catch(size_t n_args, const mp_obj_t *args) {
  mp_obj_t new_args[3];
  new_args[0] = args[0];
  new_args[1] = mp_const_none;
  new_args[2] = (n_args > 1) ? args[1] : mp_const_none;

  return mod_quickjs_promise_then(3, new_args);
}

MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mod_quickjs_promise_catch_obj, 1, 2,
                                    mod_quickjs_promise_catch);

mp_obj_t mod_quickjs_promise_finally(size_t n_args, const mp_obj_t *args) {
  mp_obj_quickjs_promise_t *p = MP_OBJ_TO_PTR(args[0]);

  quickjs_ctx_t *state = p->state;

  if (state == NULL || state->closed || state->ctx == NULL ||
      state->rt == NULL) {

    mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("context closed"));
  }

  mp_obj_t on_finally = (n_args > 1) ? args[1] : mp_const_none;

  if (!quickjs_promise_handler_check(on_finally)) {

    mp_raise_msg(&mp_type_TypeError,
                 MP_ERROR_TEXT("handler must be callable or None"));
  }

  quickjs_ctx_enter(state);

  nlr_buf_t nlr;

  if (nlr_push(&nlr) == 0) {

    JSContext *qctx = state->ctx;

    JSValue pval = quickjs_promise_lookup(p);

    JSValue f_js = quickjs_promise_handler_to_js(state, qctx, on_finally);

    if (JS_IsException(f_js)) {

      quickjs_raise_exception(qctx, f_js);
    }

    JSValue global = JS_GetGlobalObject(qctx);

    JSValue prom_ctor = JS_GetPropertyStr(qctx, global, "Promise");

    JS_FreeValue(qctx, global);

    if (JS_IsException(prom_ctor)) {

      JS_FreeValue(qctx, f_js);

      quickjs_raise_exception(qctx, prom_ctor);
    }

    JSValue proto = JS_GetPropertyStr(qctx, prom_ctor, "prototype");

    JS_FreeValue(qctx, prom_ctor);

    if (JS_IsException(proto)) {

      JS_FreeValue(qctx, f_js);

      quickjs_raise_exception(qctx, proto);
    }

    JSValue finally_fn = JS_GetPropertyStr(qctx, proto, "finally");

    JS_FreeValue(qctx, proto);

    if (JS_IsException(finally_fn)) {

      JS_FreeValue(qctx, f_js);

      quickjs_raise_exception(qctx, finally_fn);
    }

    JSValue arg_arr[1];
    arg_arr[0] = f_js;

    JSValue derived = JS_Call(qctx, finally_fn, pval, 1, arg_arr);

    JS_FreeValue(qctx, finally_fn);
    JS_FreeValue(qctx, f_js);

    if (JS_IsException(derived)) {

      quickjs_raise_exception(qctx, derived);
    }

    mp_obj_t result = quickjs_promise_wrap_owned(qctx, derived);

    nlr_pop();

    quickjs_ctx_leave(state);

    return result;

  } else {

    quickjs_ctx_leave(state);

    nlr_raise(nlr.ret_val);
  }

  return mp_const_none; /* unreachable */
}

MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mod_quickjs_promise_finally_obj, 1, 2,
                                    mod_quickjs_promise_finally);

/* -------------------------------------------------------------------------- */
/* Promise type definition                                                    */
/* -------------------------------------------------------------------------- */

static const mp_rom_map_elem_t quickjs_promise_locals_dict_table[] = {

    {MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&mod_quickjs_promise_del_obj)},

    {MP_ROM_QSTR(MP_QSTR_done), MP_ROM_PTR(&mod_quickjs_promise_done_obj)},

    {MP_ROM_QSTR(MP_QSTR_result), MP_ROM_PTR(&mod_quickjs_promise_result_obj)},

    {MP_ROM_QSTR(MP_QSTR_then), MP_ROM_PTR(&mod_quickjs_promise_then_obj)},

    {MP_ROM_QSTR(MP_QSTR_catch), MP_ROM_PTR(&mod_quickjs_promise_catch_obj)},

    {MP_ROM_QSTR(MP_QSTR_finally_),
     MP_ROM_PTR(&mod_quickjs_promise_finally_obj)},
};

static MP_DEFINE_CONST_DICT(quickjs_promise_locals_dict,
                            quickjs_promise_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(quickjs_promise_type, MP_QSTR_Promise,
                         MP_TYPE_FLAG_NONE, locals_dict,
                         &quickjs_promise_locals_dict);

mp_obj_t quickjs_promise_to_mp(JSContext *ctx, JSValueConst val) {
  quickjs_ctx_t *state = (quickjs_ctx_t *)JS_GetContextOpaque(ctx);

  if (state == NULL || state->closed) {

    mp_raise_msg(&mp_type_TypeError,
                 MP_ERROR_TEXT("unsupported QuickJS value type"));
  }

  mp_obj_quickjs_promise_t *p = mp_obj_malloc_with_finaliser(
      mp_obj_quickjs_promise_t, &quickjs_promise_type);

  p->state = state;
  p->ctx_obj = state->self_obj;
  p->token = ++state->next_token;

  quickjs_value_entry_t *pe = m_new(quickjs_value_entry_t, 1);

  pe->token = p->token;
  pe->val = JS_DupValue(ctx, val);
  pe->next = state->promise_entries;
  state->promise_entries = pe;

  return MP_OBJ_FROM_PTR(p);
}

mp_obj_t quickjs_promise_wrap_owned(JSContext *ctx, JSValue q) {
  nlr_buf_t nlr;

  if (nlr_push(&nlr) == 0) {

    mp_obj_t w = quickjs_promise_to_mp(ctx, q);

    nlr_pop();

    JS_FreeValue(ctx, q);

    return w;
  }

  JS_FreeValue(ctx, q);

  nlr_raise(nlr.ret_val);

  return mp_const_none; /* unreachable */
}

/* -------------------------------------------------------------------------- */
/* Resolver wrapper and type definition                                       */
/* -------------------------------------------------------------------------- */

mp_obj_t mod_quickjs_resolver_del(mp_obj_t self_in) {
  mp_obj_quickjs_resolver_t *r = MP_OBJ_TO_PTR(self_in);

  if (r->state != NULL && r->state->ctx != NULL) {

    quickjs_value_entry_t **pp = &r->state->resolver_entries;

    while (*pp != NULL) {

      if ((*pp)->token == r->token) {

        quickjs_value_entry_t *e = *pp;

        *pp = e->next;

        JS_FreeValue(r->state->ctx, e->val);

        m_del(quickjs_value_entry_t, e, 1);

        break;
      }

      pp = &(*pp)->next;
    }
  }

  return mp_const_none;
}

static MP_DEFINE_CONST_FUN_OBJ_1(mod_quickjs_resolver_del_obj,
                                 mod_quickjs_resolver_del);

JSValue quickjs_resolver_lookup(mp_obj_quickjs_resolver_t *r) {
  quickjs_ctx_t *state = r->state;

  if (state == NULL || state->closed || state->ctx == NULL ||
      state->rt == NULL) {

    mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("context closed"));
  }

  for (quickjs_value_entry_t *e = state->resolver_entries; e != NULL;
       e = e->next) {

    if (e->token == r->token) {

      return e->val;
    }
  }

  mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("context closed"));

  return JS_UNDEFINED; /* unreachable */
}

mp_obj_t mod_quickjs_resolver_call(mp_obj_t self_in, size_t n_args, size_t n_kw,
                                   const mp_obj_t *args) {
  mp_obj_quickjs_resolver_t *r = MP_OBJ_TO_PTR(self_in);

  quickjs_ctx_t *state = r->state;

  if (n_kw > 0) {

    mp_raise_msg(&mp_type_TypeError,
                 MP_ERROR_TEXT("resolver does not support keyword arguments"));
  }

  if (n_args > 1) {

    mp_raise_msg(&mp_type_TypeError,
                 MP_ERROR_TEXT("resolver takes at most 1 value argument"));
  }

  if (state == NULL || state->closed || state->ctx == NULL ||
      state->rt == NULL) {

    mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("context closed"));
  }

  quickjs_ctx_enter(state);

  nlr_buf_t nlr;

  if (nlr_push(&nlr) == 0) {

    JSContext *qctx = state->ctx;

    JSValue func = quickjs_resolver_lookup(r);

    quickjs_convert_state_t st;
    memset(&st, 0, sizeof(st));

    JSValue v = JS_UNDEFINED;
    bool have_v = false;

    if (n_args >= 1) {

      mp_obj_t value = args[0];

      if (r->is_reject && mp_obj_is_exception_instance(value)) {

        v = quickjs_exception_to_js_error(qctx, value);

        if (JS_IsException(v)) {

          quickjs_raise_exception(qctx, v);
        }

      } else {

        v = mp_to_quickjs(qctx, value, &st);

        if (JS_IsException(v)) {

          quickjs_raise_exception(qctx, v);
        }
      }

      have_v = true;
    }

    quickjs_ctx_arm_timeout(state);

    JSValue result = (have_v) ? JS_Call(qctx, func, JS_UNDEFINED, 1, &v)
                              : JS_Call(qctx, func, JS_UNDEFINED, 0, NULL);

    bool timed_out = quickjs_ctx_finish_timeout(state);

    if (have_v) {

      JS_FreeValue(qctx, v);
    }

    if (JS_IsException(result)) {

      quickjs_raise_exception_state(qctx, result, timed_out);
    }

    JS_FreeValue(qctx, result);

    nlr_pop();

    quickjs_ctx_leave(state);

    return mp_const_none;

  } else {

    quickjs_ctx_leave(state);

    nlr_raise(nlr.ret_val);
  }

  return mp_const_none; /* unreachable */
}

static const mp_rom_map_elem_t quickjs_resolver_locals_dict_table[] = {

    {MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&mod_quickjs_resolver_del_obj)},
};

static MP_DEFINE_CONST_DICT(quickjs_resolver_locals_dict,
                            quickjs_resolver_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(quickjs_resolver_type, MP_QSTR_PromiseResolver,
                         MP_TYPE_FLAG_NONE, call, mod_quickjs_resolver_call,
                         locals_dict, &quickjs_resolver_locals_dict);

mp_obj_t quickjs_resolver_to_mp(JSContext *ctx, JSValueConst val,
                                bool is_reject) {
  quickjs_ctx_t *state = (quickjs_ctx_t *)JS_GetContextOpaque(ctx);

  mp_obj_quickjs_resolver_t *r = mp_obj_malloc_with_finaliser(
      mp_obj_quickjs_resolver_t, &quickjs_resolver_type);

  r->state = state;
  r->ctx_obj = state->self_obj;
  r->token = ++state->next_token;
  r->is_reject = is_reject;

  quickjs_value_entry_t *re = m_new(quickjs_value_entry_t, 1);

  re->token = r->token;
  re->val = JS_DupValue(ctx, val);
  re->next = state->resolver_entries;
  state->resolver_entries = re;

  return MP_OBJ_FROM_PTR(r);
}

/* -------------------------------------------------------------------------- */
/* Unhandled Promise Rejection Tracker                                        */
/* -------------------------------------------------------------------------- */

void quickjs_promise_rejection_tracker(JSContext *qctx, JSValueConst promise,
                                       JSValueConst reason, bool is_handled,
                                       void *opaque) {
  (void)promise;
  quickjs_ctx_t *state = (quickjs_ctx_t *)opaque;

  if (state == NULL || state->closed || state->ctx == NULL ||
      state->rt == NULL || qctx != state->ctx) {

    return;
  }

  quickjs_rejection_handler_t *node = state->rejection_handler;

  if (node == NULL || node->callable == MP_OBJ_NULL) {

    return;
  }

  quickjs_ctx_enter(state);

  quickjs_convert_state_t st;
  memset(&st, 0, sizeof(st));

  nlr_buf_t nlr;

  if (nlr_push(&nlr) == 0) {

    mp_obj_t args[2];

    args[0] = quickjs_to_mp_obj(qctx, reason, &st);

    args[1] = mp_obj_new_bool(is_handled);

    mp_call_function_n_kw(node->callable, 2, 0, args);

    nlr_pop();

    quickjs_ctx_leave(state);

    return;

  } else {

    node->last_error = nlr.ret_val;

    quickjs_ctx_leave(state);

    return;
  }
}

mp_obj_t mod_quickjs_ctx_set_unhandled_rejection_handler(mp_obj_t self_in,
                                                         mp_obj_t callable) {
  quickjs_ctx_t *state = quickjs_ctx_check_open(self_in);

  if (callable != mp_const_none && !mp_obj_is_callable(callable)) {

    mp_raise_TypeError(MP_ERROR_TEXT("handler must be callable or None"));
  }

  if (state->rejection_handler != NULL) {

    quickjs_rejection_handler_t *old = state->rejection_handler;

    state->rejection_handler = NULL;

    old->state = NULL;
    old->callable = MP_OBJ_NULL;
    old->last_error = MP_OBJ_NULL;

    m_del(quickjs_rejection_handler_t, old, 1);
  }

  if (callable == mp_const_none) {

    JS_SetHostPromiseRejectionTracker(state->rt, NULL, NULL);

    return mp_const_none;
  }

  quickjs_rejection_handler_t *node = m_new_obj(quickjs_rejection_handler_t);

  node->state = state;
  node->callable = callable;
  node->last_error = MP_OBJ_NULL;

  state->rejection_handler = node;

  JS_SetHostPromiseRejectionTracker(state->rt,
                                    quickjs_promise_rejection_tracker, state);

  return mp_const_none;
}

MP_DEFINE_CONST_FUN_OBJ_2(mod_quickjs_ctx_set_unhandled_rejection_handler_obj,
                          mod_quickjs_ctx_set_unhandled_rejection_handler);
