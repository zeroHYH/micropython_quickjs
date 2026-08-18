#include "modquickjs.h"

/* -------------------------------------------------------------------------- */
/* Reentrancy protection: enter/leave executing_depth                         */
/* -------------------------------------------------------------------------- */

void quickjs_ctx_enter(quickjs_ctx_t *state) {
  if (state != NULL) {

    /*
     * Phase 10 (OOM/lifecycle audit fix):
     * Reclaim handler closure nodes destroyed by JS GC. The finalizer callback
     * only marks them dead, never frees (preserving UAF-free ordering for
     * close/overwrite paths). Unified unlink + m_del here prevents
     * high-frequency promise handler churn from accumulating callback nodes
     * until ctx.close() (measured 96 B per closure).
     */
    quickjs_reap_dead_callbacks(state);

    state->executing_depth++;
  }
}

void quickjs_ctx_leave(quickjs_ctx_t *state) {
  if (state != NULL) {
    if (state->executing_depth > 0) {
      state->executing_depth--;
    }
  }
}

/* -------------------------------------------------------------------------- */
/* Execution timeout (JS_SetInterruptHandler)                                 */
/* -------------------------------------------------------------------------- */

int quickjs_interrupt_handler(JSRuntime *rt, void *opaque) {
  (void)rt;
  quickjs_ctx_t *state = (quickjs_ctx_t *)opaque;

  if (state == NULL || !state->timeout_enabled || state->deadline_ms == 0) {

    return 0; /* continue */
  }

  mp_uint_t now = mp_hal_ticks_ms();

  /* wrap-safe: now >= deadline */
  if ((mp_uint_t)(now - state->deadline_ms) < ((mp_uint_t)-1) / 2) {

    state->timeout_triggered = true;

    return 1; /* interrupt */
  }

  return 0;
}

void quickjs_ctx_arm_timeout(quickjs_ctx_t *state) {
  if (state == NULL || !state->timeout_enabled) {

    return;
  }

  if (state->timeout_depth == 0) {

    /* Outermost window: set timeout budget for this invocation chain */
    state->deadline_ms = mp_hal_ticks_ms() + state->timeout_ms;

    state->timeout_triggered = false;
  }

  state->timeout_depth++;
}

bool quickjs_ctx_finish_timeout(quickjs_ctx_t *state) {
  if (state == NULL) {
    return false;
  }

  bool t = state->timeout_triggered;

  if (state->timeout_depth > 0) {
    state->timeout_depth--;
  }

  if (state->timeout_depth == 0) {

    state->timeout_triggered = false;
    state->deadline_ms = 0;
  }

  return t;
}

/* -------------------------------------------------------------------------- */
/* Python: ctx.set_time_limit(ms)                                             */
/* -------------------------------------------------------------------------- */

mp_obj_t mod_quickjs_ctx_set_time_limit(mp_obj_t self_in, mp_obj_t ms_obj) {
  quickjs_ctx_t *state = quickjs_ctx_check_open(self_in);

  mp_int_t ms = mp_obj_get_int(ms_obj);

  if (ms < 0) {

    mp_raise_msg(&mp_type_ValueError,
                 MP_ERROR_TEXT("time limit must be non-negative"));
  }

  if (ms == 0) {

    state->timeout_enabled = false;
    state->timeout_ms = 0;
    state->deadline_ms = 0;
    state->timeout_triggered = false;

  } else {

    state->timeout_enabled = true;
    state->timeout_ms = (mp_uint_t)ms;
    state->deadline_ms = 0;
    state->timeout_triggered = false;
  }

  return mp_const_none;
}

MP_DEFINE_CONST_FUN_OBJ_2(mod_quickjs_ctx_set_time_limit_obj,
                          mod_quickjs_ctx_set_time_limit);

/* -------------------------------------------------------------------------- */
/* JS job queue (microtask pump) helper function                              */
/* -------------------------------------------------------------------------- */

mp_obj_t quickjs_run_jobs_helper(quickjs_ctx_t *state) {
  JSContext *qctx = (state != NULL) ? state->ctx : ctx;

  JSRuntime *qrt = (state != NULL) ? state->rt : rt;

  int count = 0;
  JSContext *err_ctx = NULL;

  quickjs_ctx_arm_timeout(state);

  for (;;) {

    JSContext *jctx = NULL;

    int err = JS_ExecutePendingJob(qrt, &jctx);

    if (err <= 0) {

      if (err < 0) {

        err_ctx = (jctx != NULL) ? jctx : qctx;
      }

      break;
    }

    count++;
  }

  bool timed_out = quickjs_ctx_finish_timeout(state);

  if (err_ctx != NULL) {

    /*
     * Job execution failed: consume exception on *pctx and convert to Python
     * exception. Timeout interrupt converts to timeout exception. Runtime
     * remains usable after exception is raised; remaining jobs stay in queue
     * for subsequent run_jobs().
     */
    quickjs_raise_exception_state(err_ctx, JS_UNDEFINED, timed_out);
  }

  return mp_obj_new_int(count);
}

/* -------------------------------------------------------------------------- */
/* Eval execution core                                                        */
/* -------------------------------------------------------------------------- */

mp_obj_t quickjs_eval_helper(quickjs_ctx_t *state, const char *js_code) {
  JSContext *qctx = (state != NULL) ? state->ctx : ctx;

  quickjs_convert_state_t st;
  memset(&st, 0, sizeof(st));

  quickjs_ctx_arm_timeout(state);

  JSValue val =
      JS_Eval(qctx, js_code, strlen(js_code), "<eval>", JS_EVAL_TYPE_GLOBAL);

  bool timed_out = quickjs_ctx_finish_timeout(state);

  if (JS_IsException(val)) {

    quickjs_raise_exception_state(qctx, val, timed_out);

    return mp_const_none;
  }

  /*
   * When MicroPython exception is thrown during conversion,
   * quickjs_to_mp_owned is responsible for freeing val.
   */
  mp_obj_t result = quickjs_to_mp_owned(qctx, val, val, &st);

  JS_FreeValue(qctx, val);

  return result;
}

/* -------------------------------------------------------------------------- */
/* Call execution core                                                        */
/* -------------------------------------------------------------------------- */

mp_obj_t quickjs_call_value_helper_this(quickjs_ctx_t *state, JSValueConst func,
                                        JSValue this_val, size_t argc,
                                        const mp_obj_t *mp_args) {
  JSContext *qctx = (state != NULL) ? state->ctx : ctx;

  quickjs_convert_state_t st;
  memset(&st, 0, sizeof(st));

  int n = (int)argc;

  JSValue *argv = NULL;

  if (n > 0) {

    /*
     * Phase 10 (OOM audit fix): If argv (m_new allocation) fails, this_val
     * is still owned by this function and must be freed before rethrowing,
     * otherwise this_val leaks across JSRuntime lifetime (observable in DEBUG
     * assert/LSan).
     */
    nlr_buf_t nlr_argv;

    if (nlr_push(&nlr_argv) == 0) {

      argv = m_new(JSValue, n);

      nlr_pop();

    } else {

      JS_FreeValue(qctx, this_val);

      nlr_raise(nlr_argv.ret_val);
    }
  }

  /*
   * MicroPython -> JS
   */
  for (int i = 0; i < n; i++) {

    argv[i] = mp_to_quickjs(qctx, mp_args[i], &st);

    if (JS_IsException(argv[i])) {

      /*
       * Free arguments that have already been created/converted.
       */
      for (int j = 0; j < i; j++) {

        JS_FreeValue(qctx, argv[j]);
      }

      if (argv != NULL) {

        m_del(JSValue, argv, n);
      }

      /* Free this_val (owned) before raising exception */
      JS_FreeValue(qctx, this_val);

      quickjs_raise_exception(qctx, argv[i]);

      return mp_const_none;
    }
  }

  quickjs_ctx_arm_timeout(state);

  /*
   * JS Function call
   */
  JSValue result = JS_Call(qctx, func, this_val, n, argv);

  bool timed_out = quickjs_ctx_finish_timeout(state);

  /*
   * Argument ownership: JS_Call does not free JSValues in argv for us.
   */
  for (int i = 0; i < n; i++) {

    JS_FreeValue(qctx, argv[i]);
  }

  if (argv != NULL) {

    m_del(JSValue, argv, n);
  }

  /* Free this_val (owned) - no longer needed for result conversion */
  JS_FreeValue(qctx, this_val);

  if (JS_IsException(result)) {

    quickjs_raise_exception_state(qctx, result, timed_out);

    return mp_const_none;
  }

  /*
   * JS -> MicroPython
   */
  mp_obj_t mp_result = quickjs_to_mp_owned(qctx, result, result, &st);

  JS_FreeValue(qctx, result);

  return mp_result;
}

mp_obj_t quickjs_call_value_helper(quickjs_ctx_t *state, JSValueConst func,
                                   size_t argc, const mp_obj_t *mp_args) {
  return quickjs_call_value_helper_this(state, func, JS_UNDEFINED, argc,
                                        mp_args);
}

mp_obj_t quickjs_call_helper(quickjs_ctx_t *state, const char *function_name,
                             size_t argc, const mp_obj_t *mp_args) {
  JSContext *qctx = (state != NULL) ? state->ctx : ctx;

  /*
   * Get function from global object
   */
  JSValue global = JS_GetGlobalObject(qctx);

  JSValue func = JS_GetPropertyStr(qctx, global, function_name);

  JS_FreeValue(qctx, global);

  if (JS_IsException(func)) {

    quickjs_raise_exception(qctx, func);

    return mp_const_none;
  }

  /*
   * Verify callable
   */
  if (!JS_IsFunction(qctx, func)) {

    JS_FreeValue(qctx, func);

    mp_raise_msg(&mp_type_TypeError, MP_ERROR_TEXT("JS value is not callable"));
  }

  nlr_buf_t nlr2;

  if (nlr_push(&nlr2) == 0) {

    mp_obj_t mp_result = quickjs_call_value_helper(state, func, argc, mp_args);

    nlr_pop();

    JS_FreeValue(qctx, func);

    return mp_result;

  } else {

    JS_FreeValue(qctx, func);

    nlr_raise(nlr2.ret_val);
  }

  return mp_const_none; /* unreachable */
}
