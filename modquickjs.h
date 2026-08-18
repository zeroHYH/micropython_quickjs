#ifndef MICROPY_INCLUDED_MODQUICKJS_H
#define MICROPY_INCLUDED_MODQUICKJS_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "py/mphal.h"
#include "py/obj.h"
#include "py/objlist.h"
#include "py/objstr.h"
#include "py/objtuple.h"
#include "py/runtime.h"

#include "quickjs.h"

/* -------------------------------------------------------------------------- */
/* Default memory limit                                                       */
/* -------------------------------------------------------------------------- */

#ifndef QUICKJS_DEFAULT_MEMORY_LIMIT
#define QUICKJS_DEFAULT_MEMORY_LIMIT (128 * 1024)
#endif

/* -------------------------------------------------------------------------- */
/* Conversion depth limit and circular reference detection                    */
/* -------------------------------------------------------------------------- */

#ifndef QUICKJS_MAX_CONVERSION_DEPTH
#define QUICKJS_MAX_CONVERSION_DEPTH 32
#endif

typedef enum {
  QUICKJS_CONV_OK = 0,
  QUICKJS_CONV_TOO_DEEP = 1,
  QUICKJS_CONV_CYCLE = 2
} quickjs_convert_err_t;

typedef struct _quickjs_convert_state_t {
  const void *active[QUICKJS_MAX_CONVERSION_DEPTH];
  size_t depth;
} quickjs_convert_state_t;

/* -------------------------------------------------------------------------- */
/* Forward declarations                                                       */
/* -------------------------------------------------------------------------- */

typedef struct _quickjs_ctx_t quickjs_ctx_t;
typedef struct _quickjs_callback_t quickjs_callback_t;
typedef struct _quickjs_rejection_handler_t quickjs_rejection_handler_t;
typedef struct _quickjs_value_entry_t quickjs_value_entry_t;

/* -------------------------------------------------------------------------- */
/* Callback and Rejection structures                                          */
/* -------------------------------------------------------------------------- */

struct _quickjs_callback_t {
  struct _quickjs_callback_t *next;
  quickjs_ctx_t *state;
  mp_obj_t
      callable; /* Python callable (GC root, scanned conservatively via node) */
  mp_obj_t name;      /* Registration name (used for overwrite lookup) */
  JSValue js_func;    /* CClosure reference held by registry */
  bool opaque_active; /* CClosure opaque still points to this node */
  bool dead; /* Closure destroyed by JS GC (opaque is no longer safe) */
};

struct _quickjs_rejection_handler_t {
  quickjs_ctx_t *state;
  mp_obj_t
      callable; /* Python handler (GC root, scanned conservatively via node) */
  mp_obj_t last_error; /* Last exception thrown by handler (recorded, not
                          rethrown) */
};

struct _quickjs_value_entry_t {
  struct _quickjs_value_entry_t *next;
  uint32_t token;
  JSValue val;
};

/* -------------------------------------------------------------------------- */
/* quickjs.Context() instance structure                                       */
/* -------------------------------------------------------------------------- */

struct _quickjs_ctx_t {
  JSRuntime *rt;
  JSContext *ctx;
  bool closed;
  mp_obj_t self_obj;
  quickjs_callback_t *callbacks;
  bool timeout_enabled;
  mp_uint_t timeout_ms;
  mp_uint_t deadline_ms;
  bool timeout_triggered;
  unsigned timeout_depth;
  unsigned executing_depth;
  struct _quickjs_value_entry_t *function_entries;
  struct _quickjs_value_entry_t *promise_entries;
  struct _quickjs_value_entry_t *resolver_entries;
  uint32_t next_token;
  quickjs_rejection_handler_t *rejection_handler;
};

typedef struct _mp_obj_quickjs_context_t {
  mp_obj_base_t base;
  quickjs_ctx_t *state;
} mp_obj_quickjs_context_t;

/* -------------------------------------------------------------------------- */
/* Function / Promise / Resolver / BigInt wrapper structures                  */
/* -------------------------------------------------------------------------- */

typedef struct _mp_obj_quickjs_function_t {
  mp_obj_base_t base;
  quickjs_ctx_t *state;
  mp_obj_t ctx_obj;
  uint32_t token;
} mp_obj_quickjs_function_t;

typedef struct _mp_obj_quickjs_promise_t {
  mp_obj_base_t base;
  quickjs_ctx_t *state;
  mp_obj_t ctx_obj;
  uint32_t token;
} mp_obj_quickjs_promise_t;

typedef struct _mp_obj_quickjs_resolver_t {
  mp_obj_base_t base;
  quickjs_ctx_t *state;
  mp_obj_t ctx_obj;
  uint32_t token;
  bool is_reject;
} mp_obj_quickjs_resolver_t;

typedef struct _mp_obj_quickjs_bigint_t {
  mp_obj_base_t base;
  mp_obj_t value;
} mp_obj_quickjs_bigint_t;

/* -------------------------------------------------------------------------- */
/* Global singleton runtime and context (defined in modquickjs.c)             */
/* -------------------------------------------------------------------------- */

extern JSRuntime *rt;
extern JSContext *ctx;

/* -------------------------------------------------------------------------- */
/* Type objects and builtin function objects declarations                     */
/* -------------------------------------------------------------------------- */

extern const mp_obj_type_t quickjs_context_type;
extern const mp_obj_type_t quickjs_function_type;
extern const mp_obj_type_t quickjs_promise_type;
extern const mp_obj_type_t quickjs_resolver_type;
extern const mp_obj_type_t quickjs_bigint_type;

extern const mp_obj_fun_builtin_fixed_t mod_quickjs_bigint_obj;
extern const mp_obj_fun_builtin_fixed_t mod_quickjs_ctx_add_callable_obj;
extern const mp_obj_fun_builtin_fixed_t mod_quickjs_ctx_set_time_limit_obj;
extern const mp_obj_fun_builtin_fixed_t
    mod_quickjs_ctx_set_unhandled_rejection_handler_obj;

/* -------------------------------------------------------------------------- */
/* Cross-module function declarations                                         */
/* -------------------------------------------------------------------------- */

/* qjs_convert.c */
quickjs_convert_err_t quickjs_convert_push(quickjs_convert_state_t *st,
                                           const void *id);
void quickjs_convert_pop(quickjs_convert_state_t *st);
void quickjs_convert_push_mp(quickjs_convert_state_t *st, const void *id);
bool quickjs_convert_push_js(JSContext *qctx, quickjs_convert_state_t *st,
                             const void *id);
mp_obj_t quickjs_to_mp_obj(JSContext *qctx, JSValueConst val,
                           quickjs_convert_state_t *st);
mp_obj_t quickjs_to_mp_owned(JSContext *qctx, JSValueConst val, JSValue owned,
                             quickjs_convert_state_t *st);
JSValue mp_to_quickjs(JSContext *qctx, mp_obj_t obj,
                      quickjs_convert_state_t *st);
JSValue mp_to_quickjs_impl(JSContext *qctx, mp_obj_t obj,
                           quickjs_convert_state_t *st);
mp_obj_t quickjs_string_to_mp(JSContext *qctx, JSValueConst val);
mp_obj_t quickjs_array_to_mp(JSContext *qctx, JSValueConst val,
                             quickjs_convert_state_t *st);
mp_obj_t quickjs_object_to_mp(JSContext *qctx, JSValueConst val,
                              quickjs_convert_state_t *st);
mp_obj_t quickjs_arraybuffer_to_mp(JSContext *qctx, JSValueConst val);
mp_obj_t quickjs_typedarray_to_mp(JSContext *qctx, JSValueConst val);
JSValue mp_str_to_quickjs(JSContext *qctx, mp_obj_t obj);
JSValue mp_list_to_quickjs(JSContext *qctx, mp_obj_t obj,
                           quickjs_convert_state_t *st);
JSValue mp_tuple_to_quickjs(JSContext *qctx, mp_obj_t obj,
                            quickjs_convert_state_t *st);
JSValue mp_dict_to_quickjs(JSContext *qctx, mp_obj_t obj,
                           quickjs_convert_state_t *st);

/* qjs_error.c */
void quickjs_clear_pending_exception(JSContext *qctx);
void quickjs_raise_value(JSContext *qctx, JSValue exception_val);
void quickjs_raise_exception(JSContext *qctx, JSValue val);
void quickjs_raise_exception_state(JSContext *qctx, JSValue val,
                                   bool timed_out);
JSValue quickjs_exception_to_js_error(JSContext *qctx, mp_obj_t exc);

/* qjs_exec.c */
void quickjs_ctx_enter(quickjs_ctx_t *state);
void quickjs_ctx_leave(quickjs_ctx_t *state);
int quickjs_interrupt_handler(JSRuntime *qrt, void *opaque);
void quickjs_ctx_arm_timeout(quickjs_ctx_t *state);
bool quickjs_ctx_finish_timeout(quickjs_ctx_t *state);
mp_obj_t mod_quickjs_ctx_set_time_limit(mp_obj_t self_in, mp_obj_t ms_obj);
mp_obj_t quickjs_eval_helper(quickjs_ctx_t *state, const char *js_code);
mp_obj_t quickjs_call_value_helper_this(quickjs_ctx_t *state, JSValueConst func,
                                        JSValue this_val, size_t argc,
                                        const mp_obj_t *mp_args);
mp_obj_t quickjs_call_value_helper(quickjs_ctx_t *state, JSValueConst func,
                                   size_t argc, const mp_obj_t *mp_args);
mp_obj_t quickjs_call_helper(quickjs_ctx_t *state, const char *function_name,
                             size_t argc, const mp_obj_t *mp_args);
mp_obj_t quickjs_run_jobs_helper(quickjs_ctx_t *state);

/* qjs_callback.c */
void quickjs_cb_finalize(void *opaque);
JSValue quickjs_callback(JSContext *qctx, JSValueConst this_val, int argc,
                         JSValueConst *argv, int magic, void *opaque);
void quickjs_callback_free_node(quickjs_callback_t *node);
void quickjs_reap_dead_callbacks(quickjs_ctx_t *state);
mp_obj_t mod_quickjs_ctx_add_callable_impl(quickjs_ctx_t *state,
                                           mp_obj_t name_obj,
                                           mp_obj_t callable_obj);
mp_obj_t mod_quickjs_ctx_add_callable(mp_obj_t self_in, mp_obj_t name_obj,
                                      mp_obj_t callable_obj);

/* qjs_func.c */
mp_obj_t quickjs_function_to_mp(JSContext *qctx, JSValueConst val);
JSValue quickjs_function_lookup(mp_obj_quickjs_function_t *f);
JSValue quickjs_function_lookup_jserr(JSContext *ctx,
                                      mp_obj_quickjs_function_t *f);
JSValue quickjs_function_pass_through(JSContext *qctx, mp_obj_t obj);
mp_obj_t mod_quickjs_function_del(mp_obj_t self_in);
mp_obj_t mod_quickjs_function_call(mp_obj_t self_in, size_t n_args, size_t n_kw,
                                   const mp_obj_t *args);
mp_obj_t mod_quickjs_function_call_this(size_t n_args, const mp_obj_t *args);

/* qjs_promise.c */
mp_obj_t quickjs_promise_to_mp(JSContext *qctx, JSValueConst val);
mp_obj_t quickjs_promise_wrap_owned(JSContext *qctx, JSValue q);
JSValue quickjs_promise_lookup(mp_obj_quickjs_promise_t *p);
JSValue quickjs_promise_lookup_jserr(JSContext *ctx,
                                     mp_obj_quickjs_promise_t *p);
JSValue quickjs_promise_pass_through(JSContext *qctx, mp_obj_t obj);
mp_obj_t mod_quickjs_promise_del(mp_obj_t self_in);
mp_obj_t mod_quickjs_promise_done(mp_obj_t self_in);
mp_obj_t mod_quickjs_promise_result(mp_obj_t self_in);
mp_obj_t mod_quickjs_promise_then(size_t n_args, const mp_obj_t *args);
mp_obj_t mod_quickjs_promise_catch(size_t n_args, const mp_obj_t *args);
mp_obj_t mod_quickjs_promise_finally(size_t n_args, const mp_obj_t *args);
JSValue quickjs_new_handler_closure(quickjs_ctx_t *state, mp_obj_t callable);
JSValue quickjs_promise_handler_to_js(quickjs_ctx_t *state, JSContext *qctx,
                                      mp_obj_t handler);
bool quickjs_promise_handler_check(mp_obj_t handler);
mp_obj_t quickjs_resolver_to_mp(JSContext *ctx, JSValueConst val,
                                bool is_reject);
mp_obj_t mod_quickjs_resolver_del(mp_obj_t self_in);
JSValue quickjs_resolver_lookup(mp_obj_quickjs_resolver_t *r);
mp_obj_t mod_quickjs_resolver_call(mp_obj_t self_in, size_t n_args, size_t n_kw,
                                   const mp_obj_t *args);
void quickjs_promise_rejection_tracker(JSContext *qctx, JSValueConst promise,
                                       JSValueConst reason, bool is_handled,
                                       void *opaque);
mp_obj_t mod_quickjs_ctx_set_unhandled_rejection_handler(mp_obj_t self_in,
                                                         mp_obj_t callable);
void quickjs_ctx_release_entries(quickjs_ctx_t *state);

/* qjs_bigint.c */
void quickjs_bigint_print(const mp_print_t *print, mp_obj_t self_in,
                          mp_print_kind_t kind);
mp_obj_t mod_quickjs_bigint(mp_obj_t value_obj);
JSValue quickjs_bigint_wrapper_to_js(JSContext *ctx, mp_obj_t obj);
bool quickjs_bigint_str_to_i64(const char *s, int64_t *out);
mp_obj_t quickjs_bigint_to_mp(JSContext *ctx, JSValueConst val);

/* qjs_context.c */
quickjs_ctx_t *quickjs_ctx_check_open(mp_obj_t self_in);
mp_obj_t quickjs_context_make_new(const mp_obj_type_t *type, size_t n_args,
                                  size_t n_kw, const mp_obj_t *args);
mp_obj_t mod_quickjs_ctx_close(mp_obj_t self_in);
mp_obj_t mod_quickjs_ctx_eval(mp_obj_t self_in, mp_obj_t js_code_obj);
mp_obj_t mod_quickjs_ctx_call(size_t n_args, const mp_obj_t *args);
mp_obj_t mod_quickjs_ctx_get(mp_obj_t self_in, mp_obj_t name_obj);
mp_obj_t mod_quickjs_ctx_set(mp_obj_t self_in, mp_obj_t name_obj,
                             mp_obj_t value_obj);
mp_obj_t mod_quickjs_ctx_promise(mp_obj_t self_in);
mp_obj_t mod_quickjs_ctx_gc(mp_obj_t self_in);
mp_obj_t mod_quickjs_ctx_run_jobs(mp_obj_t self_in);
mp_obj_t mod_quickjs_ctx_has_pending_jobs(mp_obj_t self_in);
mp_obj_t mod_quickjs_ctx_set_memory_limit(mp_obj_t self_in, mp_obj_t limit_obj);
mp_obj_t mod_quickjs_ctx_set_max_stack_size(mp_obj_t self_in,
                                            mp_obj_t limit_obj);

#endif // MICROPY_INCLUDED_MODQUICKJS_H
