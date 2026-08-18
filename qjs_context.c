#include "modquickjs.h"

/* -------------------------------------------------------------------------- */
/* Context creation                                                           */
/* -------------------------------------------------------------------------- */

mp_obj_t quickjs_context_make_new(const mp_obj_type_t *type, size_t n_args,
                                  size_t n_kw, const mp_obj_t *args) {
  (void)args;
  mp_arg_check_num(n_args, n_kw, 0, 0, false);

  JSRuntime *qrt = JS_NewRuntime();

  if (qrt == NULL) {

    mp_raise_msg(&mp_type_RuntimeError,
                 MP_ERROR_TEXT("failed to create JS Runtime"));
  }

  JS_SetMemoryLimit(qrt, QUICKJS_DEFAULT_MEMORY_LIMIT);

  JSContext *qctx = JS_NewContext(qrt);

  if (qctx == NULL) {

    JS_FreeRuntime(qrt);

    mp_raise_msg(&mp_type_RuntimeError,
                 MP_ERROR_TEXT("failed to create JS Context"));
  }

  quickjs_ctx_t *state = NULL;
  mp_obj_t self = MP_OBJ_NULL;

  nlr_buf_t nlr;

  if (nlr_push(&nlr) == 0) {

    state = m_new_obj(quickjs_ctx_t);

    memset(state, 0, sizeof(quickjs_ctx_t));

    mp_obj_quickjs_context_t *obj =
        mp_obj_malloc_with_finaliser(mp_obj_quickjs_context_t, type);

    obj->state = state;

    state->rt = qrt;
    state->ctx = qctx;
    state->closed = false;
    state->self_obj = MP_OBJ_FROM_PTR(obj);

    JS_SetContextOpaque(qctx, state);

    JS_SetInterruptHandler(qrt, quickjs_interrupt_handler, state);

    self = MP_OBJ_FROM_PTR(obj);

    nlr_pop();

  } else {

    JS_FreeContext(qctx);
    JS_FreeRuntime(qrt);

    nlr_raise(nlr.ret_val);
  }

  return self;
}

/* -------------------------------------------------------------------------- */
/* Context state check                                                        */
/* -------------------------------------------------------------------------- */

quickjs_ctx_t *quickjs_ctx_check_open(mp_obj_t self_in) {
  mp_obj_quickjs_context_t *self = MP_OBJ_TO_PTR(self_in);

  quickjs_ctx_t *state = self->state;

  if (state == NULL || state->closed || state->ctx == NULL ||
      state->rt == NULL) {

    mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("context closed"));
  }

  return state;
}

/* -------------------------------------------------------------------------- */
/* Context.close()                                                            */
/* -------------------------------------------------------------------------- */

mp_obj_t mod_quickjs_ctx_close(mp_obj_t self_in) {
  mp_obj_quickjs_context_t *self = MP_OBJ_TO_PTR(self_in);

  quickjs_ctx_t *state = self->state;

  if (state == NULL || state->closed) {

    return mp_const_none;
  }

  if (state->executing_depth > 0) {

    mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("context is busy"));
  }

  state->closed = true;

  if (state->rt != NULL) {

    JS_SetHostPromiseRejectionTracker(state->rt, NULL, NULL);
  }

  if (state->ctx != NULL) {

    quickjs_ctx_release_entries(state);

    quickjs_callback_t *cb = state->callbacks;

    while (cb != NULL) {

      JS_FreeValue(state->ctx, cb->js_func);

      cb->js_func = JS_UNDEFINED;

      cb = cb->next;
    }
  }

  if (state->ctx != NULL) {

    JS_FreeContext(state->ctx);

    state->ctx = NULL;
  }

  if (state->rt != NULL) {

    JS_FreeRuntime(state->rt);

    state->rt = NULL;
  }

  quickjs_callback_t *cb = state->callbacks;

  while (cb != NULL) {

    quickjs_callback_t *next = cb->next;

    quickjs_callback_free_node(cb);

    cb = next;
  }

  state->callbacks = NULL;

  if (state->rejection_handler != NULL) {

    quickjs_rejection_handler_t *rh = state->rejection_handler;

    state->rejection_handler = NULL;

    rh->state = NULL;
    rh->callable = MP_OBJ_NULL;
    rh->last_error = MP_OBJ_NULL;

    m_del(quickjs_rejection_handler_t, rh, 1);
  }

  return mp_const_none;
}

/* -------------------------------------------------------------------------- */
/* Context.eval()                                                             */
/* -------------------------------------------------------------------------- */

mp_obj_t mod_quickjs_ctx_eval(mp_obj_t self_in, mp_obj_t js_code_obj) {
  quickjs_ctx_t *state = quickjs_ctx_check_open(self_in);

  quickjs_ctx_enter(state);

  nlr_buf_t nlr;

  if (nlr_push(&nlr) == 0) {

    const char *js_code = mp_obj_str_get_str(js_code_obj);

    mp_obj_t result = quickjs_eval_helper(state, js_code);

    nlr_pop();

    quickjs_ctx_leave(state);

    return result;

  } else {

    quickjs_ctx_leave(state);

    nlr_raise(nlr.ret_val);
  }

  return mp_const_none; /* unreachable */
}

static MP_DEFINE_CONST_FUN_OBJ_2(mod_quickjs_ctx_eval_obj,
                                 mod_quickjs_ctx_eval);

/* -------------------------------------------------------------------------- */
/* Context.call()                                                             */
/* -------------------------------------------------------------------------- */

mp_obj_t mod_quickjs_ctx_call(size_t n_args, const mp_obj_t *args) {
  quickjs_ctx_t *state = quickjs_ctx_check_open(args[0]);

  quickjs_ctx_enter(state);

  nlr_buf_t nlr;

  if (nlr_push(&nlr) == 0) {

    const char *function_name = mp_obj_str_get_str(args[1]);

    mp_obj_t result =
        quickjs_call_helper(state, function_name, n_args - 2, args + 2);

    nlr_pop();

    quickjs_ctx_leave(state);

    return result;

  } else {

    quickjs_ctx_leave(state);

    nlr_raise(nlr.ret_val);
  }

  return mp_const_none; /* unreachable */
}

static MP_DEFINE_CONST_FUN_OBJ_VAR(mod_quickjs_ctx_call_obj, 2,
                                   mod_quickjs_ctx_call);

/* -------------------------------------------------------------------------- */
/* Context.get()                                                              */
/* -------------------------------------------------------------------------- */

static mp_obj_t mod_quickjs_ctx_get_impl(quickjs_ctx_t *state,
                                         mp_obj_t name_obj) {
  quickjs_convert_state_t st;
  memset(&st, 0, sizeof(st));

  const char *name = mp_obj_str_get_str(name_obj);

  JSValue global = JS_GetGlobalObject(state->ctx);

  JSValue val = JS_GetPropertyStr(state->ctx, global, name);

  JS_FreeValue(state->ctx, global);

  if (JS_IsException(val)) {

    quickjs_raise_exception(state->ctx, val);

    return mp_const_none;
  }

  if (JS_IsUndefined(val) || JS_IsNull(val)) {

    JS_FreeValue(state->ctx, val);

    return mp_const_none;
  }

  mp_obj_t result = quickjs_to_mp_owned(state->ctx, val, val, &st);

  JS_FreeValue(state->ctx, val);

  return result;
}

mp_obj_t mod_quickjs_ctx_get(mp_obj_t self_in, mp_obj_t name_obj) {
  quickjs_ctx_t *state = quickjs_ctx_check_open(self_in);

  quickjs_ctx_enter(state);

  nlr_buf_t nlr;

  if (nlr_push(&nlr) == 0) {

    mp_obj_t result = mod_quickjs_ctx_get_impl(state, name_obj);

    nlr_pop();

    quickjs_ctx_leave(state);

    return result;

  } else {

    quickjs_ctx_leave(state);

    nlr_raise(nlr.ret_val);
  }

  return mp_const_none; /* unreachable */
}

static MP_DEFINE_CONST_FUN_OBJ_2(mod_quickjs_ctx_get_obj, mod_quickjs_ctx_get);

/* -------------------------------------------------------------------------- */
/* Context.set()                                                              */
/* -------------------------------------------------------------------------- */

static mp_obj_t mod_quickjs_ctx_set_impl(quickjs_ctx_t *state,
                                         mp_obj_t name_obj,
                                         mp_obj_t value_obj) {
  quickjs_convert_state_t st;
  memset(&st, 0, sizeof(st));

  const char *name = mp_obj_str_get_str(name_obj);

  JSValue global = JS_GetGlobalObject(state->ctx);

  JSValue value = mp_to_quickjs(state->ctx, value_obj, &st);

  if (JS_IsException(value)) {

    JS_FreeValue(state->ctx, global);

    quickjs_raise_exception(state->ctx, value);

    return mp_const_none;
  }

  if (JS_SetPropertyStr(state->ctx, global, name, value) < 0) {

    JS_FreeValue(state->ctx, global);

    quickjs_raise_exception(state->ctx, JS_UNDEFINED);

    return mp_const_none;
  }

  JS_FreeValue(state->ctx, global);

  return mp_const_none;
}

mp_obj_t mod_quickjs_ctx_set(mp_obj_t self_in, mp_obj_t name_obj,
                             mp_obj_t value_obj) {
  quickjs_ctx_t *state = quickjs_ctx_check_open(self_in);

  quickjs_ctx_enter(state);

  nlr_buf_t nlr;

  if (nlr_push(&nlr) == 0) {

    mp_obj_t result = mod_quickjs_ctx_set_impl(state, name_obj, value_obj);

    nlr_pop();

    quickjs_ctx_leave(state);

    return result;

  } else {

    quickjs_ctx_leave(state);

    nlr_raise(nlr.ret_val);
  }

  return mp_const_none; /* unreachable */
}

static MP_DEFINE_CONST_FUN_OBJ_3(mod_quickjs_ctx_set_obj, mod_quickjs_ctx_set);

/* -------------------------------------------------------------------------- */
/* Context.promise()                                                          */
/* -------------------------------------------------------------------------- */

mp_obj_t mod_quickjs_ctx_promise(mp_obj_t self_in) {
  quickjs_ctx_t *state = quickjs_ctx_check_open(self_in);

  quickjs_ctx_enter(state);

  nlr_buf_t nlr;

  if (nlr_push(&nlr) == 0) {

    JSContext *qctx = state->ctx;

    quickjs_ctx_arm_timeout(state);

    JSValue resolving_funcs[2] = {JS_UNDEFINED, JS_UNDEFINED};

    JSValue promise = JS_NewPromiseCapability(qctx, resolving_funcs);

    bool timed_out = quickjs_ctx_finish_timeout(state);

    if (JS_IsException(promise)) {

      JS_FreeValue(qctx, resolving_funcs[0]);

      JS_FreeValue(qctx, resolving_funcs[1]);

      quickjs_raise_exception_state(qctx, promise, timed_out);
    }

    nlr_buf_t nlr2;

    if (nlr_push(&nlr2) == 0) {

      mp_obj_t pw = quickjs_promise_to_mp(qctx, promise);

      mp_obj_t rw = quickjs_resolver_to_mp(qctx, resolving_funcs[0], false);

      mp_obj_t jw = quickjs_resolver_to_mp(qctx, resolving_funcs[1], true);

      JS_FreeValue(qctx, promise);

      JS_FreeValue(qctx, resolving_funcs[0]);

      JS_FreeValue(qctx, resolving_funcs[1]);

      mp_obj_t items[3];
      items[0] = pw;
      items[1] = rw;
      items[2] = jw;

      mp_obj_t result = mp_obj_new_tuple(3, items);

      nlr_pop();
      nlr_pop();

      quickjs_ctx_leave(state);

      return result;

    } else {

      JS_FreeValue(qctx, promise);

      JS_FreeValue(qctx, resolving_funcs[0]);

      JS_FreeValue(qctx, resolving_funcs[1]);

      nlr_raise(nlr2.ret_val);
    }

  } else {

    quickjs_ctx_leave(state);

    nlr_raise(nlr.ret_val);
  }

  return mp_const_none; /* unreachable */
}

static MP_DEFINE_CONST_FUN_OBJ_1(mod_quickjs_ctx_promise_obj,
                                 mod_quickjs_ctx_promise);

/* -------------------------------------------------------------------------- */
/* Context.gc()                                                               */
/* -------------------------------------------------------------------------- */

mp_obj_t mod_quickjs_ctx_gc(mp_obj_t self_in) {
  quickjs_ctx_t *state = quickjs_ctx_check_open(self_in);

  JS_RunGC(state->rt);

  return mp_const_none;
}

static MP_DEFINE_CONST_FUN_OBJ_1(mod_quickjs_ctx_gc_obj, mod_quickjs_ctx_gc);

/* -------------------------------------------------------------------------- */
/* Context._js_mem()                                                          */
/* -------------------------------------------------------------------------- */

static mp_obj_t mod_quickjs_ctx_js_mem(mp_obj_t self_in) {
  quickjs_ctx_t *state = quickjs_ctx_check_open(self_in);

  JSMemoryUsage usage;
  memset(&usage, 0, sizeof(usage));

  JS_ComputeMemoryUsage(state->rt, &usage);

  return mp_obj_new_int_from_ll((long long)usage.memory_used_size);
}

static MP_DEFINE_CONST_FUN_OBJ_1(mod_quickjs_ctx_js_mem_obj,
                                 mod_quickjs_ctx_js_mem);

/* -------------------------------------------------------------------------- */
/* Context.run_jobs() / has_pending_jobs()                                    */
/* -------------------------------------------------------------------------- */

mp_obj_t mod_quickjs_ctx_run_jobs(mp_obj_t self_in) {
  quickjs_ctx_t *state = quickjs_ctx_check_open(self_in);

  quickjs_ctx_enter(state);

  nlr_buf_t nlr;

  if (nlr_push(&nlr) == 0) {

    mp_obj_t result = quickjs_run_jobs_helper(state);

    nlr_pop();

    quickjs_ctx_leave(state);

    return result;

  } else {

    quickjs_ctx_leave(state);

    nlr_raise(nlr.ret_val);
  }

  return mp_const_none; /* unreachable */
}

static MP_DEFINE_CONST_FUN_OBJ_1(mod_quickjs_ctx_run_jobs_obj,
                                 mod_quickjs_ctx_run_jobs);

mp_obj_t mod_quickjs_ctx_has_pending_jobs(mp_obj_t self_in) {
  quickjs_ctx_t *state = quickjs_ctx_check_open(self_in);

  return mp_obj_new_bool(JS_IsJobPending(state->rt));
}

static MP_DEFINE_CONST_FUN_OBJ_1(mod_quickjs_ctx_has_pending_jobs_obj,
                                 mod_quickjs_ctx_has_pending_jobs);

/* -------------------------------------------------------------------------- */
/* Context.set_memory_limit() / set_max_stack_size()                          */
/* -------------------------------------------------------------------------- */

mp_obj_t mod_quickjs_ctx_set_memory_limit(mp_obj_t self_in,
                                          mp_obj_t limit_obj) {
  quickjs_ctx_t *state = quickjs_ctx_check_open(self_in);

  mp_int_t limit = mp_obj_get_int(limit_obj);

  if (limit < 0) {

    mp_raise_msg(&mp_type_ValueError,
                 MP_ERROR_TEXT("memory limit must be >= 0"));
  }

  JS_SetMemoryLimit(state->rt, (size_t)limit);

  return mp_const_none;
}

static MP_DEFINE_CONST_FUN_OBJ_2(mod_quickjs_ctx_set_memory_limit_obj,
                                 mod_quickjs_ctx_set_memory_limit);

mp_obj_t mod_quickjs_ctx_set_max_stack_size(mp_obj_t self_in,
                                            mp_obj_t limit_obj) {
  quickjs_ctx_t *state = quickjs_ctx_check_open(self_in);

  mp_int_t limit = mp_obj_get_int(limit_obj);

  if (limit < 0) {

    mp_raise_msg(&mp_type_ValueError,
                 MP_ERROR_TEXT("max stack size must be >= 0"));
  }

  JS_SetMaxStackSize(state->rt, (size_t)limit);

  return mp_const_none;
}

static MP_DEFINE_CONST_FUN_OBJ_2(mod_quickjs_ctx_set_max_stack_size_obj,
                                 mod_quickjs_ctx_set_max_stack_size);

/* -------------------------------------------------------------------------- */
/* Context type definition                                                    */
/* -------------------------------------------------------------------------- */

static MP_DEFINE_CONST_FUN_OBJ_1(mod_quickjs_ctx_close_obj,
                                 mod_quickjs_ctx_close);

static const mp_rom_map_elem_t quickjs_context_locals_dict_table[] = {

    {MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&mod_quickjs_ctx_close_obj)},

    {MP_ROM_QSTR(MP_QSTR_eval), MP_ROM_PTR(&mod_quickjs_ctx_eval_obj)},

    {MP_ROM_QSTR(MP_QSTR_call), MP_ROM_PTR(&mod_quickjs_ctx_call_obj)},

    {MP_ROM_QSTR(MP_QSTR_get), MP_ROM_PTR(&mod_quickjs_ctx_get_obj)},

    {MP_ROM_QSTR(MP_QSTR_set), MP_ROM_PTR(&mod_quickjs_ctx_set_obj)},

    {MP_ROM_QSTR(MP_QSTR_promise), MP_ROM_PTR(&mod_quickjs_ctx_promise_obj)},

    {MP_ROM_QSTR(MP_QSTR_add_callable),
     MP_ROM_PTR(&mod_quickjs_ctx_add_callable_obj)},

    {MP_ROM_QSTR(MP_QSTR_set_time_limit),
     MP_ROM_PTR(&mod_quickjs_ctx_set_time_limit_obj)},

    {MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&mod_quickjs_ctx_close_obj)},

    {MP_ROM_QSTR(MP_QSTR_gc), MP_ROM_PTR(&mod_quickjs_ctx_gc_obj)},

    {MP_ROM_QSTR(MP_QSTR__js_mem), MP_ROM_PTR(&mod_quickjs_ctx_js_mem_obj)},

    {MP_ROM_QSTR(MP_QSTR_run_jobs), MP_ROM_PTR(&mod_quickjs_ctx_run_jobs_obj)},

    {MP_ROM_QSTR(MP_QSTR_has_pending_jobs),
     MP_ROM_PTR(&mod_quickjs_ctx_has_pending_jobs_obj)},

    {MP_ROM_QSTR(MP_QSTR_set_unhandled_rejection_handler),
     MP_ROM_PTR(&mod_quickjs_ctx_set_unhandled_rejection_handler_obj)},

    {MP_ROM_QSTR(MP_QSTR_set_memory_limit),
     MP_ROM_PTR(&mod_quickjs_ctx_set_memory_limit_obj)},

    {MP_ROM_QSTR(MP_QSTR_set_max_stack_size),
     MP_ROM_PTR(&mod_quickjs_ctx_set_max_stack_size_obj)},
};

static MP_DEFINE_CONST_DICT(quickjs_context_locals_dict,
                            quickjs_context_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(quickjs_context_type, MP_QSTR_Context,
                         MP_TYPE_FLAG_NONE, make_new, quickjs_context_make_new,
                         locals_dict, &quickjs_context_locals_dict);
