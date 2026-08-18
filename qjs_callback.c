#include "modquickjs.h"

/* -------------------------------------------------------------------------- */
/* Python callable -> JS CClosure (callback registry)                         */
/* -------------------------------------------------------------------------- */

void quickjs_cb_finalize(void *opaque) {
  quickjs_callback_t *node = (quickjs_callback_t *)opaque;

  if (node != NULL) {

    node->opaque_active = false;
    node->dead = true;
  }
}

JSValue quickjs_callback(JSContext *qctx, JSValueConst this_val, int argc,
                         JSValueConst *argv, int magic, void *opaque) {
  (void)this_val;
  (void)magic;

  quickjs_callback_t *node = (quickjs_callback_t *)opaque;

  quickjs_ctx_t *state = (node != NULL) ? node->state : NULL;

  /* Defense: closure can only be called when context is open */
  if (state == NULL || state->closed || state->ctx == NULL ||
      state->rt == NULL || node->callable == MP_OBJ_NULL ||
      !node->opaque_active) {

    return JS_ThrowTypeError(qctx, "context closed");
  }

  /*
   * Phase 5: Count reentrancy depth during callback execution (defense in
   * depth). Outer window is already counted (eval/call/run_jobs all enter);
   * adding 1 here guarantees calling ctx.close() inside callback will be
   * rejected; both success and exception paths execute leave, strictly
   * restoring depth.
   */
  quickjs_ctx_enter(state);

  quickjs_convert_state_t st;
  memset(&st, 0, sizeof(st));

  mp_obj_t *mp_args = NULL;

  JSValue result = JS_UNDEFINED;

  nlr_buf_t nlr;

  if (nlr_push(&nlr) == 0) {

    if (argc > 0) {

      mp_args = m_new(mp_obj_t, argc);
    }

    /* JS -> MicroPython arguments */
    for (int i = 0; i < argc; i++) {

      mp_args[i] = quickjs_to_mp_obj(qctx, argv[i], &st);
    }

    /* Call Python callable */
    mp_obj_t ret =
        mp_call_function_n_kw(node->callable, (size_t)argc, 0, mp_args);

    if (mp_args != NULL) {

      m_del(mp_obj_t, mp_args, argc);

      mp_args = NULL;
    }

    /* Python return value -> JS (unsupported type sets JS exception and returns
     * JS_EXCEPTION) */
    result = mp_to_quickjs(qctx, ret, &st);

    nlr_pop();

    quickjs_ctx_leave(state);

  } else {

    /* Python exception -> JS exception */
    if (mp_args != NULL) {

      m_del(mp_obj_t, mp_args, argc);
    }

    quickjs_ctx_leave(state);

    mp_obj_t exc = (mp_obj_t)nlr.ret_val;

    const char *type_name = mp_obj_get_type_str(exc);

    /*
     * Extract exception message (str(exc)). Protected by second-level nlr
     * in case str() itself raises an exception.
     */
    const char *msg = NULL;
    mp_obj_t s = MP_OBJ_NULL;

    nlr_buf_t nlr2;

    if (nlr_push(&nlr2) == 0) {

      mp_obj_t args_arr[1];
      args_arr[0] = exc;

      s = mp_obj_str_make_new(&mp_type_str, 1, 0, args_arr);

      msg = mp_obj_str_get_str(s);

      nlr_pop();

    } else {

      msg = NULL;
    }

    /*
     * msg points into GC string s buffer.
     * Between here and JS_ThrowTypeError (allocates only on JS heap, no MP GC),
     * there are no MP allocations so msg remains valid; s stays alive during
     * call.
     */
    JS_ThrowTypeError(qctx, "%s: %s", type_name,
                      (msg != NULL) ? msg : "callback error");

    result = JS_EXCEPTION;
  }

  return result;
}

void quickjs_callback_free_node(quickjs_callback_t *node) {
  if (node == NULL) {
    return;
  }

  node->callable = MP_OBJ_NULL;
  node->name = MP_OBJ_NULL;
  node->state = NULL;
  node->js_func = JS_UNDEFINED;

  m_del(quickjs_callback_t, node, 1);
}

void quickjs_reap_dead_callbacks(quickjs_ctx_t *state) {
  quickjs_callback_t **pp = &state->callbacks;

  while (*pp != NULL) {

    quickjs_callback_t *node = *pp;

    if (node->dead) {

      *pp = node->next;

      quickjs_callback_free_node(node);

    } else {

      pp = &node->next;
    }
  }
}

/* -------------------------------------------------------------------------- */
/* ctx.add_callable() implementation                                          */
/* -------------------------------------------------------------------------- */

mp_obj_t mod_quickjs_ctx_add_callable_impl(quickjs_ctx_t *state,
                                           mp_obj_t name_obj,
                                           mp_obj_t callable_obj) {
  if (!mp_obj_is_callable(callable_obj)) {

    mp_raise_msg(&mp_type_TypeError,
                 MP_ERROR_TEXT("callable must be callable"));
  }

  if (!mp_obj_is_str(name_obj)) {

    mp_raise_msg(&mp_type_ValueError, MP_ERROR_TEXT("invalid function name"));
  }

  {
    size_t name_len = 0;

    mp_obj_str_get_data(name_obj, &name_len);

    if (name_len == 0) {

      mp_raise_msg(&mp_type_ValueError, MP_ERROR_TEXT("invalid function name"));
    }
  }

  const char *name = mp_obj_str_get_str(name_obj);

  /*
   * Create CClosure.
   * opaque = node (not yet allocated); allocate node before creating closure.
   */
  quickjs_callback_t *node = m_new_obj(quickjs_callback_t);

  memset(node, 0, sizeof(*node));

  node->state = state;
  node->callable = callable_obj;
  node->name = name_obj;
  node->opaque_active = true;
  node->js_func = JS_UNDEFINED;

  JSValue func = JS_NewCClosure(state->ctx, quickjs_callback, name,
                                quickjs_cb_finalize, 0, 0, node);

  if (JS_IsException(func)) {

    /* Creation failed: free node (JSValue already cleaned up internally by
     * JS_NewCClosure) */
    quickjs_callback_free_node(node);

    quickjs_raise_exception(state->ctx, func);

    return mp_const_none;
  }

  /*
   * Registry holds second reference for release on close / overwrite.
   */
  node->js_func = JS_DupValue(state->ctx, func);

  /*
   * Write to global object. JS_SetPropertyStr takes ownership of func on both
   * success and failure (v0.16.1 set_value moves value directly into property
   * slot).
   */
  JSValue global = JS_GetGlobalObject(state->ctx);

  int rc = JS_SetPropertyStr(state->ctx, global, name, func);

  JS_FreeValue(state->ctx, global);

  if (rc < 0) {

    /*
     * Failure: release registry reference, then free node.
     * At this point closure has no other references (not written to global), so
     * it finalizes -> opaque_finalize(node) runs first (node still alive), then
     * we free node.
     */
    JS_FreeValue(state->ctx, node->js_func);

    quickjs_callback_free_node(node);

    quickjs_raise_exception(state->ctx, JS_UNDEFINED);

    return mp_const_none;
  }

  /*
   * Overwrite semantics: if callback with same name exists, release old
   * callback.
   */
  quickjs_callback_t *prev = NULL;

  quickjs_callback_t *old = state->callbacks;

  while (old != NULL) {

    if (old == node) {

      break;
    }

    /*
     * Anonymous nodes (Promise handler nodes, name = MP_OBJ_NULL)
     * do not participate in add_callable name overwrite matching.
     * Must never call mp_obj_equal on MP_OBJ_NULL (would dereference null
     * pointer).
     */
    if (old->name != MP_OBJ_NULL && mp_obj_equal(old->name, name_obj)) {

      break;
    }

    prev = old;
    old = old->next;
  }

  if (old != NULL && old != node) {

    /* unlink */
    if (prev != NULL) {
      prev->next = old->next;
    } else {
      state->callbacks = old->next;
    }

    /* Release old closure reference (global slot already freed old value on
     * overwrite) */
    JS_FreeValue(state->ctx, old->js_func);

    quickjs_callback_free_node(old);
  }

  /* Prepend new node to list */
  node->next = state->callbacks;
  state->callbacks = node;

  return mp_const_none;
}

mp_obj_t mod_quickjs_ctx_add_callable(mp_obj_t self_in, mp_obj_t name_obj,
                                      mp_obj_t callable_obj) {
  quickjs_ctx_t *state = quickjs_ctx_check_open(self_in);

  quickjs_ctx_enter(state);

  nlr_buf_t nlr;

  if (nlr_push(&nlr) == 0) {

    mp_obj_t result =
        mod_quickjs_ctx_add_callable_impl(state, name_obj, callable_obj);

    nlr_pop();

    quickjs_ctx_leave(state);

    return result;

  } else {

    quickjs_ctx_leave(state);

    nlr_raise(nlr.ret_val);
  }

  return mp_const_none; /* unreachable */
}

MP_DEFINE_CONST_FUN_OBJ_3(mod_quickjs_ctx_add_callable_obj,
                          mod_quickjs_ctx_add_callable);
