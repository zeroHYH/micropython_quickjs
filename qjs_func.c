#include "modquickjs.h"

/* -------------------------------------------------------------------------- */
/* JS Function -> MicroPython callable wrapper                                */
/* -------------------------------------------------------------------------- */

mp_obj_t mod_quickjs_function_del(mp_obj_t self_in) {
  mp_obj_quickjs_function_t *f = MP_OBJ_TO_PTR(self_in);

  if (f->state != NULL && f->state->ctx != NULL) {

    /*
     * JS heap is still valid: remove entry by token and free dup.
     * When close() has freed the table entirely (token not found), nothing to
     * do.
     */
    quickjs_value_entry_t **pp = &f->state->function_entries;

    while (*pp != NULL) {

      if ((*pp)->token == f->token) {

        quickjs_value_entry_t *e = *pp;

        *pp = e->next;

        JS_FreeValue(f->state->ctx, e->val);

        m_del(quickjs_value_entry_t, e, 1);

        break;
      }

      pp = &(*pp)->next;
    }
  }

  return mp_const_none;
}

static MP_DEFINE_CONST_FUN_OBJ_1(mod_quickjs_function_del_obj,
                                 mod_quickjs_function_del);

/* -------------------------------------------------------------------------- */
/* Function wrapper __call__ and .call(this_obj, *args)                       */
/* -------------------------------------------------------------------------- */

mp_obj_t mod_quickjs_function_call(mp_obj_t self_in, size_t n_args, size_t n_kw,
                                   const mp_obj_t *args) {
  mp_obj_quickjs_function_t *f = MP_OBJ_TO_PTR(self_in);

  quickjs_ctx_t *state = f->state;

  if (n_kw > 0) {

    mp_raise_msg(
        &mp_type_TypeError,
        MP_ERROR_TEXT("QuickJS function does not support keyword arguments"));
  }

  if (state == NULL || state->closed || state->ctx == NULL ||
      state->rt == NULL) {

    mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("context closed"));
  }

  quickjs_ctx_enter(state);

  nlr_buf_t nlr;

  if (nlr_push(&nlr) == 0) {

    JSValue fval = quickjs_function_lookup(f);

    mp_obj_t result = quickjs_call_value_helper(state, fval, n_args, args);

    nlr_pop();

    quickjs_ctx_leave(state);

    return result;

  } else {

    /* Exception path: must leave, otherwise depth stays nonzero and close() is
     * rejected */
    quickjs_ctx_leave(state);

    nlr_raise(nlr.ret_val);
  }

  return mp_const_none; /* unreachable */
}

mp_obj_t mod_quickjs_function_call_this(size_t n_args, const mp_obj_t *args) {
  mp_obj_quickjs_function_t *f = MP_OBJ_TO_PTR(args[0]);

  quickjs_ctx_t *state = f->state;

  if (state == NULL || state->closed || state->ctx == NULL ||
      state->rt == NULL) {

    mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("context closed"));
  }

  quickjs_ctx_enter(state);

  nlr_buf_t nlr;

  if (nlr_push(&nlr) == 0) {

    quickjs_convert_state_t st;
    memset(&st, 0, sizeof(st));

    JSValue this_js = mp_to_quickjs(state->ctx, args[1], &st);

    if (JS_IsException(this_js)) {

      /*
       * quickjs_raise_exception frees this_js sentinel and raises.
       * The exception is caught by outer nlr -> leave -> rethrown.
       */
      quickjs_raise_exception(state->ctx, this_js);
    }

    mp_obj_t result = quickjs_call_value_helper_this(
        state, quickjs_function_lookup(f), this_js, /* owned: freed by helper */
        n_args - 2, args + 2);

    nlr_pop();

    quickjs_ctx_leave(state);

    return result;

  } else {

    quickjs_ctx_leave(state);

    nlr_raise(nlr.ret_val);
  }

  return mp_const_none; /* unreachable */
}

MP_DEFINE_CONST_FUN_OBJ_VAR(mod_quickjs_function_call_this_obj, 2,
                            mod_quickjs_function_call_this);

/* -------------------------------------------------------------------------- */
/* Function wrapper type definition                                           */
/* -------------------------------------------------------------------------- */

static const mp_rom_map_elem_t quickjs_function_locals_dict_table[] = {

    {MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&mod_quickjs_function_del_obj)},

    {MP_ROM_QSTR(MP_QSTR_call),
     MP_ROM_PTR(&mod_quickjs_function_call_this_obj)},
};

static MP_DEFINE_CONST_DICT(quickjs_function_locals_dict,
                            quickjs_function_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(quickjs_function_type, MP_QSTR_Function,
                         MP_TYPE_FLAG_NONE, call, mod_quickjs_function_call,
                         locals_dict, &quickjs_function_locals_dict);

/* -------------------------------------------------------------------------- */
/* JS Function -> Python conversion implementation                            */
/* -------------------------------------------------------------------------- */

mp_obj_t quickjs_function_to_mp(JSContext *ctx, JSValueConst val) {
  /*
   * Only Context runtime has state (associated via JS_GetContextOpaque).
   * Default singleton ctx has no opaque -> maintain phase 2 behavior
   * (throw "unsupported QuickJS value type").
   */
  quickjs_ctx_t *state = (quickjs_ctx_t *)JS_GetContextOpaque(ctx);

  if (state == NULL || state->closed) {

    mp_raise_msg(&mp_type_TypeError,
                 MP_ERROR_TEXT("unsupported QuickJS value type"));
  }

  mp_obj_quickjs_function_t *f = mp_obj_malloc_with_finaliser(
      mp_obj_quickjs_function_t, &quickjs_function_type);

  f->state = state;
  f->ctx_obj = state->self_obj;
  f->token = ++state->next_token;

  /*
   * Phase 5: dup into entry table (owned by state), wrapper holds token only.
   * close() frees table as a whole; __del__ removes by token.
   */
  quickjs_value_entry_t *e = m_new(quickjs_value_entry_t, 1);

  e->token = f->token;
  e->val = JS_DupValue(ctx, val);
  e->next = state->function_entries;
  state->function_entries = e;

  return MP_OBJ_FROM_PTR(f);
}

/* -------------------------------------------------------------------------- */
/* Token lookup and Pass-through                                              */
/* -------------------------------------------------------------------------- */

JSValue quickjs_function_lookup(mp_obj_quickjs_function_t *f) {
  quickjs_ctx_t *state = f->state;

  if (state == NULL || state->closed || state->ctx == NULL ||
      state->rt == NULL) {

    mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("context closed"));
  }

  for (quickjs_value_entry_t *e = state->function_entries; e != NULL;
       e = e->next) {

    if (e->token == f->token) {

      return e->val;
    }
  }

  mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("context closed"));

  return JS_UNDEFINED; /* unreachable */
}

JSValue quickjs_function_lookup_jserr(JSContext *ctx,
                                      mp_obj_quickjs_function_t *f) {
  quickjs_ctx_t *state = f->state;

  if (state == NULL || state->closed || state->ctx == NULL ||
      state->rt == NULL) {

    return JS_ThrowTypeError(ctx, "context closed");
  }

  if (state->ctx != ctx) {

    return JS_ThrowTypeError(ctx, "function belongs to another context");
  }

  for (quickjs_value_entry_t *e = state->function_entries; e != NULL;
       e = e->next) {

    if (e->token == f->token) {

      return JS_DupValue(ctx, e->val);
    }
  }

  return JS_ThrowTypeError(ctx, "context closed");
}

JSValue quickjs_function_pass_through(JSContext *ctx, mp_obj_t obj) {
  mp_obj_quickjs_function_t *f = MP_OBJ_TO_PTR(obj);

  return quickjs_function_lookup_jserr(ctx, f);
}
