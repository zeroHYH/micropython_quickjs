#include <string.h>
#include <stdint.h>

#include "py/runtime.h"
#include "py/obj.h"
#include "py/objstr.h"
#include "py/objlist.h"
#include "py/objtuple.h"
#include "py/mphal.h"

#include "quickjs.h"


/* -------------------------------------------------------------------------- */
/* 默认内存限制                                                                 */
/* -------------------------------------------------------------------------- */

/*
 * 与原始 ESP32 构建保持一致：默认 64 KiB QuickJS 堆。
 *
 * 注意：在 64 位宿主上（例如 Unix port 的 64 位构建），
 * QuickJS-NG 仅初始化一个 JSContext 就需要超过 64 KiB，
 * 因此 Make 型 port 可以通过 CFLAGS 覆盖该值，例如：
 *
 *   -DQUICKJS_DEFAULT_MEMORY_LIMIT=8388608
 *
 * CMake 型 port（ESP32/RP2 等）不使用该宏，保持 64 KiB 不变。
 */
#ifndef QUICKJS_DEFAULT_MEMORY_LIMIT
#define QUICKJS_DEFAULT_MEMORY_LIMIT (128 * 1024)
#endif


/* -------------------------------------------------------------------------- */
/* 转换深度限制与循环引用检测                                                   */
/* -------------------------------------------------------------------------- */

/*
 * 最大嵌套转换深度。
 *
 * 保护：深层嵌套结构（JS 数组/对象、Python list/dict 等）
 * 不会导致 C 栈溢出。达到上限时抛清晰异常。
 */
#ifndef QUICKJS_MAX_CONVERSION_DEPTH
#define QUICKJS_MAX_CONVERSION_DEPTH 32
#endif


typedef enum {
    QUICKJS_CONV_OK = 0,
    QUICKJS_CONV_TOO_DEEP = 1,
    QUICKJS_CONV_CYCLE = 2
} quickjs_convert_err_t;


/*
 * 当前递归路径上的活跃容器栈（固定大小，MCU 友好，无动态分配）。
 *
 * 用于两件事：
 *   1. 深度限制：depth 即当前嵌套深度，超过上限报“深度超限”。
 *   2. 循环检测：只检查“当前路径上的活跃容器”是否已出现
 *      （active stack），而不是所有已访问对象。
 *
 * 因此 [x, x] 这样的共享引用（两个元素指向同一对象）
 * 不会被误判为循环：第二个 x 时，x 已经不在活跃栈上。
 * 真正的循环（a=[a] 或 a.b=b; b.a=a）会在进入第二个容器时命中。
 */
typedef struct _quickjs_convert_state_t {
    const void *active[QUICKJS_MAX_CONVERSION_DEPTH];
    size_t depth;
} quickjs_convert_state_t;


/*
 * 压栈。返回错误码而非直接抛异常，调用方决定如何上报
 * （JS->MP 方向抛 Python 异常，MP->JS 方向设 JS 异常）。
 */
static quickjs_convert_err_t quickjs_convert_push(
    quickjs_convert_state_t *st,
    const void *id
) {
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


static void quickjs_convert_pop(
    quickjs_convert_state_t *st
) {
    st->depth--;
}


/*
 * JS -> MP 方向：压栈失败直接抛 Python 异常。
 * 调用方必须保证通过 nlr 释放已持有的 JSValue。
 */
static void quickjs_convert_push_mp(
    quickjs_convert_state_t *st,
    const void *id
) {
    quickjs_convert_err_t err =
        quickjs_convert_push(st, id);

    if (err == QUICKJS_CONV_TOO_DEEP) {

        mp_raise_msg(
            &mp_type_RuntimeError,
            MP_ERROR_TEXT(
                "maximum conversion depth exceeded"
            )
        );
    }

    if (err == QUICKJS_CONV_CYCLE) {

        mp_raise_msg(
            &mp_type_RuntimeError,
            MP_ERROR_TEXT(
                "circular reference detected"
            )
        );
    }
}


/*
 * MP -> JS 方向：压栈失败设置 JS 异常并返回 false。
 * 不抛 Python 异常，避免中途长跳绕过 JSValue 释放。
 */
static bool quickjs_convert_push_js(
    JSContext *qctx,
    quickjs_convert_state_t *st,
    const void *id
) {
    quickjs_convert_err_t err =
        quickjs_convert_push(st, id);

    if (err == QUICKJS_CONV_TOO_DEEP) {

        JS_ThrowTypeError(
            qctx,
            "maximum conversion depth exceeded"
        );

        return false;
    }

    if (err == QUICKJS_CONV_CYCLE) {

        JS_ThrowTypeError(
            qctx,
            "circular reference detected"
        );

        return false;
    }

    return true;
}


/* -------------------------------------------------------------------------- */
/* QuickJS Runtime / Context                                                  */
/* -------------------------------------------------------------------------- */

/*
 * 默认 singleton：纯 C 的 JSRuntime + JSContext。
 *
 * 不含任何 Python 对象引用，因此不会因 MicroPython GC 而失效。
 * 只在进程退出时由操作系统回收，与阶段 0 行为一致。
 */
static JSRuntime *rt = NULL;
static JSContext *ctx = NULL;


/* -------------------------------------------------------------------------- */
/* quickjs.Context() 实例结构                                                   */
/* -------------------------------------------------------------------------- */

/*
 * 每个 quickjs.Context() 对应一份独立的 JSRuntime + JSContext，
 * 全局状态完全隔离。
 *
 * state 由 m_new_obj() 分配在 MicroPython GC 堆中。
 * MicroPython GC 对已标记的堆块做保守式逐字扫描
 * （py/gc.c: gc_mark_subtree 遍历每个 word），
 * 因此 state 里的 self_obj 会被当作潜在 GC root：
 *
 *   Context 对象 --state--> state 块 --self_obj--> Context 对象
 *
 * 两者互相保持可达；当 Python 侧不再引用 Context 时，
 * 两者一起被 GC 回收（state 无 finaliser，不参与 finaliser 循环）。
 *
 * rt/ctx 是纯 C 资源，生命周期由 close()/finaliser 管理，不依赖 GC。
 */
typedef struct _quickjs_callback_t
    quickjs_callback_t;

/* 阶段 9：unhandled rejection handler 节点（完整定义见下） */
typedef struct _quickjs_rejection_handler_t
    quickjs_rejection_handler_t;

typedef struct _quickjs_ctx_t {
    JSRuntime *rt;
    JSContext *ctx;

    bool closed;

    /* 回指 Context 对象（后续阶段回调/函数包装注册表会用到）。 */
    mp_obj_t self_obj;

    /*
     * 阶段 3：Python callable -> JS CClosure 的回调注册表。
     * 每个节点由 m_new_obj 分配在 GC 堆，节点内的 mp_obj_t 字段
     * 会被 GC 保守扫描，因此 callable 在注册期间不会被回收。
     */
    quickjs_callback_t *callbacks;

    /*
     * 阶段 3：JS 执行超时。
     * 用 mp_hal_ticks_ms()（wrap-safe）在 interrupt handler 中判断 deadline。
     */
    bool timeout_enabled;
    mp_uint_t timeout_ms;
    mp_uint_t deadline_ms;   /* 0 = 当前没有激活的超时窗口 */
    bool timeout_triggered;

    /*
     * 阶段 6：活跃超时窗口计数（arm 之后、finish 之前的窗口数）。
     *
     * JS 执行可嵌套（外层 eval/call -> Python callback -> 内层 eval），
     * 每个执行段都 arm/finish 超时窗口。内层窗口必须共享外层 deadline：
     * 若内层 arm 重新设置 deadline、内层 finish 清除窗口，外层无限循环
     * 会通过回调里的内层 eval 不断“重置”超时预算而永远不被中断。
     *
     * 规则：
     *   - 只有最外层窗口（depth 0 -> 1）设定 deadline、清零 triggered；
     *   - 内层窗口（depth > 0）不改变 deadline/triggered（共享预算）；
     *   - 只有最外层窗口 finish（depth 1 -> 0）才清除 deadline/triggered，
     *     超时状态不污染后续调用；
     *   - 任意窗口 finish 都返回当前 triggered（超时中断可能在任意
     *     嵌套窗口的字节码执行期间触发）。
     */
    unsigned timeout_depth;

    /*
     * 阶段 5：重入保护执行深度。
     * 所有可能进入 QuickJS 执行栈的 Python 入口（eval/call/get/set/
     * add_callable/run_jobs/Function wrapper 调用/Promise 方法）在执行期间
     * quickjs_ctx_enter() 递增、quickjs_ctx_leave() 递减（每个异常/NLR
     * 路径都必须恢复）。
     *
     * executing_depth > 0 时 ctx.close() 被拒绝并抛
     * "context is busy" —— 绝不关闭一个正在执行 JS 的 Runtime，否则
     * QuickJS 栈会在 close 后继续访问已释放的 runtime/context。
     */
    unsigned executing_depth;

    /*
     * 阶段 5：wrapper 的 JS 值条目表（token -> JSValue dup）。
     *
     * 生命周期模型（GC/生命周期审计修复）：
     *
     *   Function / Promise wrapper 不直接持有 JSValue dup，而是持有一个
     *   单调递增的 token。真正的 dup 只存在于此条目表里，归 state 所有：
     *
     *     wrapper --token--> entry { token, JSValue dup }（state 持有）
     *
     *   好处（不依赖 MicroPython GC/finaliser 的时序）：
     *     - wrapper 被 MP GC 回收但不跑 __del__（保守扫描误根等常见情形）
     *       时，dup 不会泄漏：条目仍在表里，close() 统一释放；
     *     - wrapper 正常 __del__ 时按 token 摘除条目并释放 dup；
     *     - close() 只需释放整张表，与 wrapper 对象的死活完全解耦，
     *       绝不触碰 wrapper 内存（没有跨已回收对象的链表遍历）；
     *     - close 后 wrapper 的 token 查不到条目 -> 统一
     *       RuntimeError("context closed")，dup 已被 close 释放，
     *       wrapper 不会访问悬垂 JS 堆。
     *
     * 条目是 m_new 分配的裸结构（不含 mp_obj_t，只有 JSValue），
     * 由 close()/__del__ 用 m_del 释放，不引入 static GC root。
     */
    struct _quickjs_value_entry_t *function_entries;
    struct _quickjs_value_entry_t *promise_entries;

    /*
     * 阶段 7：resolve/reject callable wrapper 的 JS 值条目表
     * （token -> JSValue dup）。
     *
     * ctx.promise() 创建的 resolving function（JS_NewPromiseCapability
     * 的 resolving_funcs[0]/[1]）由本表持有：wrapper（resolve/reject
     * Python callable）只拿 token，dup 归 state 所有，生命周期与
     * function_entries / promise_entries 完全一致（同上模型）：
     *
     *   - wrapper 被 MP GC 回收但不跑 __del__ 时，dup 不泄漏
     *     （close() 统一释放整张表）；
     *   - wrapper 正常 __del__ 时按 token 摘除条目并释放 dup；
     *   - close() 释放整张表；close 后 wrapper 的 token 查不到条目
     *     -> 统一 RuntimeError("context closed")。
     */
    struct _quickjs_value_entry_t *resolver_entries;
    uint32_t next_token;    /* 0 保留给“无 token”；单调递增，不复用 */

    /*
     * 阶段 9：unhandled promise rejection 诊断 handler。
     *
     * 通过 QuickJS 原生 JS_SetHostPromiseRejectionTracker() 上报：
     *   - is_handled == false：promise 被 reject（同步，在 reject 动作内）；
     *   - is_handled == true ：rejection 后续被 handler 接管（.then/.catch
     *     挂到已 rejected 的 promise 上，同步）。
     *
     * 节点（quickjs_rejection_handler_t）由 m_new_obj 分配在 GC 堆，
     * 由本指针引用 -> state 块被 GC 标记时保守扫描到节点 -> 节点的
     * callable/last_error 字段随节点被扫描，handler 在注册期间不会被
     * 回收（与 callbacks 链表同一套结论，无需 static GC root）。
     *
     * 生命周期与 callbacks 完全一致：
     *   - 替换：旧节点 m_del（解除 callable 引用），链入新节点；
     *   - None：解除注册 + m_del 节点；
     *   - close()：先 JS_SetHostPromiseRejectionTracker(rt, NULL, NULL)
     *     解除注册，再 m_del 节点（close 后不再有 tracker 回调）。
     */
    quickjs_rejection_handler_t *rejection_handler;
} quickjs_ctx_t;

/*
 * 阶段 5：state 持有的 wrapper JS 值条目。token 是 wrapper 的查找键。
 * val 持有对 JS 对象的一个引用（JS_DupValue），归条目表所有。
 */
typedef struct _quickjs_value_entry_t {
    struct _quickjs_value_entry_t *next;
    uint32_t token;
    JSValue val;
} quickjs_value_entry_t;

typedef struct _mp_obj_quickjs_context_t {
    mp_obj_base_t base;
    quickjs_ctx_t *state;
} mp_obj_quickjs_context_t;


/* -------------------------------------------------------------------------- */
/* 重入保护：executing_depth 的进入/退出                                        */
/* -------------------------------------------------------------------------- */

/*
 * 进入一次 JS 执行窗口。state 为 NULL（默认 singleton）时是 no-op：
 * singleton 没有 close()，不存在重入关闭问题。
 *
 * 调用约定：凡是在执行期间可能回调 Python（CClosure -> ctx.close()）的
 * 入口，都必须以 enter 开始、并在所有路径（含 NLR 异常）上 leave 结束。
 */
/* 见 quickjs_reap_dead_callbacks 定义（struct 定义之后）。 */
static void quickjs_reap_dead_callbacks(
    quickjs_ctx_t *state
);


static void quickjs_ctx_enter(
    quickjs_ctx_t *state
) {
    if (state != NULL) {

        /*
         * 阶段 10（OOM/生命周期审计修复）：
         * 回收已被 JS GC 销毁的 handler 闭包节点。最终化回调只打
         * dead 标记、绝不 free（保持 close/覆盖路径的无 UAF 排序），
         * 由这里统一 unlink + m_del —— 高频 promise handler churn
         * 不再把 callback 节点累积到 ctx.close()（实测每闭包 96 B）。
         */
        quickjs_reap_dead_callbacks(state);

        state->executing_depth++;
    }
}


/*
 * 退出一次 JS 执行窗口。防御：depth 为 0 时不再递减（不产生下溢）。
 */
static void quickjs_ctx_leave(
    quickjs_ctx_t *state
) {
    if (state != NULL) {
        if (state->executing_depth > 0) {
            state->executing_depth--;
        }
    }
}


/* -------------------------------------------------------------------------- */
/* Forward declarations                                                       */
/* -------------------------------------------------------------------------- */

static mp_obj_t quickjs_to_mp_obj(
    JSContext *ctx,
    JSValueConst val,
    quickjs_convert_state_t *st
);

static JSValue mp_to_quickjs(
    JSContext *ctx,
    mp_obj_t obj,
    quickjs_convert_state_t *st
);

/*
 * 阶段 6：mp_to_quickjs 的裸实现（参见 mp_to_quickjs 自身的说明）。
 */
static JSValue mp_to_quickjs_impl(
    JSContext *ctx,
    mp_obj_t obj,
    quickjs_convert_state_t *st
);

static mp_obj_t quickjs_array_to_mp(
    JSContext *ctx,
    JSValueConst val,
    quickjs_convert_state_t *st
);

static mp_obj_t quickjs_object_to_mp(
    JSContext *ctx,
    JSValueConst val,
    quickjs_convert_state_t *st
);


static void quickjs_clear_pending_exception(
    JSContext *ctx
);

static mp_obj_t quickjs_to_mp_owned(
    JSContext *ctx,
    JSValueConst val,
    JSValue owned,
    quickjs_convert_state_t *st
);

static mp_obj_t quickjs_call_value_helper_this(
    quickjs_ctx_t *state,
    JSValueConst func,
    JSValue this_val,
    size_t argc,
    const mp_obj_t *mp_args
);

static mp_obj_t quickjs_call_value_helper(
    quickjs_ctx_t *state,
    JSValueConst func,
    size_t argc,
    const mp_obj_t *mp_args
);

static JSValue quickjs_promise_pass_through(
    JSContext *ctx,
    mp_obj_t obj
);

static JSValue quickjs_bigint_wrapper_to_js(
    JSContext *ctx,
    mp_obj_t obj
);

static mp_obj_t quickjs_function_to_mp(
    JSContext *ctx,
    JSValueConst val
);

static mp_obj_t mod_quickjs_function_call(
    mp_obj_t self_in,
    size_t n_args,
    size_t n_kw,
    const mp_obj_t *args
);

static mp_obj_t mod_quickjs_function_del(
    mp_obj_t self_in
);

static quickjs_ctx_t *quickjs_ctx_check_open(
    mp_obj_t self_in
);

/*
 * 超时窗口 / 异常抛出辅助的前向声明：
 * 阶段 7 的 resolver 代码位于这些辅助的实现之前。
 */
static void quickjs_ctx_arm_timeout(
    quickjs_ctx_t *state
);

static bool quickjs_ctx_finish_timeout(
    quickjs_ctx_t *state
);

static void quickjs_raise_exception_state(
    JSContext *ctx,
    JSValue val,
    bool timed_out
);

static void quickjs_raise_exception(
    JSContext *ctx,
    JSValue val
);

/*
 * 把调用方持有的 JSValue（异常值/ rejection reason）格式化为
 * MicroPython 异常并抛出。调用方把所有权移交给本函数。
 * 阶段 4：供 rejected promise 的 result() 复用。
 */
static void quickjs_raise_value(
    JSContext *ctx,
    JSValue exception_val
);

static mp_obj_t quickjs_promise_to_mp(
    JSContext *ctx,
    JSValueConst val
);

static mp_obj_t mod_quickjs_promise_done(
    mp_obj_t self_in
);

static mp_obj_t mod_quickjs_promise_result(
    mp_obj_t self_in
);

static mp_obj_t mod_quickjs_promise_del(
    mp_obj_t self_in
);

/* 阶段 7：ctx.promise() 的 resolve/reject wrapper（见定义处注释） */
static mp_obj_t mod_quickjs_resolver_call(
    mp_obj_t self_in,
    size_t n_args,
    size_t n_kw,
    const mp_obj_t *args
);

static mp_obj_t mod_quickjs_resolver_del(
    mp_obj_t self_in
);

static mp_obj_t mod_quickjs_ctx_promise(
    mp_obj_t self_in
);


/* -------------------------------------------------------------------------- */
/* JS Function -> MicroPython callable wrapper                                  */
/* -------------------------------------------------------------------------- */

/*
 * 阶段 3：ctx.get("foo") 遇到 JS Function 时返回可调用的 Python wrapper。
 *
 * 生命周期设计（核心）：
 *
 *   - ctx_obj 是 state->self_obj 的强引用（Context 对象本身）。
 *     wrapper 存活期间，Context 对象不会提前被 GC：
 *
 *       wrapper --ctx_obj--> Context 对象 --state--> state
 *
 *     因此 wrapper 里的裸 state 指针一直有效。
 *
 *   - token 是 state->function_entries 条目表里的查找键；真正的
 *     JS 引用（JS_DupValue）由 state 的条目表持有，wrapper 不持有
 *     JSValue。close() 整体释放条目表；__del__ 按 token 摘除单个条目。
 *
 *   - 释放：__del__（finaliser）中，仅当 state->ctx 仍存在时按 token
 *     摘除条目并 JS_FreeValue。若 Context 已被 close()（state->ctx==NULL），
 *     条目表已整体释放，直接跳过。
 *
 *   - 调用：mod_quickjs_function_call 先检查 context 是否关闭，
 *     关闭则抛 RuntimeError("context closed")；条目查不到同样抛
 *     RuntimeError("context closed")（close 已释放 dup），绝不触碰
 *     悬垂 JS 堆。
 */
typedef struct _mp_obj_quickjs_function_t {
    mp_obj_base_t base;

    quickjs_ctx_t *state;
    mp_obj_t ctx_obj;   /* 强引用：保证 Context 不被 GC（见上） */

    uint32_t token;     /* state->function_entries 的查找键 */
} mp_obj_quickjs_function_t;


static mp_obj_t mod_quickjs_function_del(
    mp_obj_t self_in
) {
    mp_obj_quickjs_function_t *f =
        MP_OBJ_TO_PTR(self_in);

    if (f->state != NULL &&
        f->state->ctx != NULL) {

        /*
         * JS 堆仍有效：按 token 摘除条目并释放 dup。
         * close() 已整体释放条目表时（token 查不到），无事可做。
         */
        quickjs_value_entry_t **pp =
            &f->state->function_entries;

        while (*pp != NULL) {

            if ((*pp)->token == f->token) {

                quickjs_value_entry_t *e =
                    *pp;

                *pp = e->next;

                JS_FreeValue(
                    f->state->ctx,
                    e->val
                );

                m_del(
                    quickjs_value_entry_t,
                    e,
                    1
                );

                break;
            }

            pp = &(*pp)->next;
        }
    }

    return mp_const_none;
}

static MP_DEFINE_CONST_FUN_OBJ_1(
    mod_quickjs_function_del_obj,
    mod_quickjs_function_del
);

/* wrapper.call(this_obj, *args) 的 FUN_OBJ（定义见下文；先声明） */
MP_DECLARE_CONST_FUN_OBJ_VAR(mod_quickjs_function_call_this_obj);


static const mp_rom_map_elem_t
quickjs_function_locals_dict_table[] = {

    {
        MP_ROM_QSTR(MP_QSTR___del__),
        MP_ROM_PTR(
            &mod_quickjs_function_del_obj
        )
    },

    {
        MP_ROM_QSTR(MP_QSTR_call),
        MP_ROM_PTR(
            &mod_quickjs_function_call_this_obj
        )
    },
};


static MP_DEFINE_CONST_DICT(
    quickjs_function_locals_dict,
    quickjs_function_locals_dict_table
);


MP_DEFINE_CONST_OBJ_TYPE(
    quickjs_function_type,
    MP_QSTR_Function,
    MP_TYPE_FLAG_NONE,
    call, mod_quickjs_function_call,
    locals_dict, &quickjs_function_locals_dict
);


static mp_obj_t quickjs_function_to_mp(
    JSContext *ctx,
    JSValueConst val
) {
    /*
     * 只有 Context 运行时有 state（通过 JS_GetContextOpaque 关联）。
     * 默认 singleton 的 ctx 没有 opaque -> 保持阶段 2 行为
     * （抛“unsupported QuickJS value type”）。
     */
    quickjs_ctx_t *state =
        (quickjs_ctx_t *)JS_GetContextOpaque(ctx);

    if (state == NULL ||
        state->closed) {

        mp_raise_msg(
            &mp_type_TypeError,
            MP_ERROR_TEXT(
                "unsupported QuickJS value type"
            )
        );
    }

    mp_obj_quickjs_function_t *f =
        mp_obj_malloc_with_finaliser(
            mp_obj_quickjs_function_t,
            &quickjs_function_type
        );

    f->state = state;
    f->ctx_obj = state->self_obj;
    f->token = ++state->next_token;

    /*
     * 阶段 5：dup 进条目表（state 所有），wrapper 只拿 token。
     * close() 整体释放；__del__ 按 token 摘除。
     */
    quickjs_value_entry_t *e =
        m_new(
            quickjs_value_entry_t,
            1
        );

    e->token = f->token;
    e->val = JS_DupValue(ctx, val);
    e->next = state->function_entries;
    state->function_entries = e;

    return MP_OBJ_FROM_PTR(f);
}


/* -------------------------------------------------------------------------- */
/* JS Promise -> MicroPython wrapper                                           */
/* -------------------------------------------------------------------------- */

/*
 * 阶段 4：ctx.eval() / ctx.get() 遇到 JS Promise 时返回 Promise wrapper。
 *
 * 生命周期设计与 Function wrapper 完全一致（复用同一套约定）：
 *
 *   - ctx_obj 是 state->self_obj 的强引用，wrapper 存活期间 Context 不会
 *     被 MicroPython GC 提前回收 -> 裸 state 指针始终有效。
 *   - token 是 state->promise_entries 条目表里的查找键；真正的 JS 引用
 *     （JS_DupValue）由 state 的条目表持有，wrapper 不持有 JSValue。
 *   - __del__：仅当 state->ctx 仍存在时按 token 摘除条目并释放；
 *     Context close 后条目表已整体释放，直接跳过。
 *   - done()/result() 先检查 context 是否关闭，关闭则抛
 *     RuntimeError("context closed")，绝不触碰悬垂 JS 堆。
 *
 * 读取性 API（不执行 JS 代码，因此不需要 timeout 窗口）：
 *   - p.done()   -> bool：promise 是否已定局（fulfilled 或 rejected）
 *   - p.result() -> fulfilled 返回转换后的值；
 *                   rejected 把 rejection 转成 Python 异常抛出；
 *                   pending 抛 RuntimeError（提示先 ctx.run_jobs()）
 */
typedef struct _mp_obj_quickjs_promise_t {
    mp_obj_base_t base;

    quickjs_ctx_t *state;
    mp_obj_t ctx_obj;   /* 强引用：保证 Context 不被 GC（见上） */

    uint32_t token;     /* state->promise_entries 的查找键 */
} mp_obj_quickjs_promise_t;


static mp_obj_t mod_quickjs_promise_del(
    mp_obj_t self_in
) {
    mp_obj_quickjs_promise_t *p =
        MP_OBJ_TO_PTR(self_in);

    if (p->state != NULL &&
        p->state->ctx != NULL) {

        quickjs_value_entry_t **pp =
            &p->state->promise_entries;

        while (*pp != NULL) {

            if ((*pp)->token == p->token) {

                quickjs_value_entry_t *e =
                    *pp;

                *pp = e->next;

                JS_FreeValue(
                    p->state->ctx,
                    e->val
                );

                m_del(
                    quickjs_value_entry_t,
                    e,
                    1
                );

                break;
            }

            pp = &(*pp)->next;
        }
    }

    return mp_const_none;
}

static MP_DEFINE_CONST_FUN_OBJ_1(
    mod_quickjs_promise_del_obj,
    mod_quickjs_promise_del
);


/*
 * 阶段 5（GC/生命周期审计修复）：释放 state 持有的全部 wrapper JS 值
 * 条目（function_entries / promise_entries）以及 add_callable 的回调
 * js_func（后者在 close() 内原有的回调循环里处理）。
 *
 * 调用时机：close() 内、ctx 仍有效时（否则 wrapper 的 dup 引用跨过
 * JS_FreeRuntime 的内部 GC，使对象残留在 gc_obj_list：DEBUG 构建触发
 * assert，release 构建造成 ~50 KB/Context 的 ASAN 可观测泄漏）。
 *
 * 与 wrapper 对象的死活完全解耦：不遍历 wrapper 对象，只遍历 state
 * 自己拥有的条目；安全性不依赖 MicroPython GC/finaliser 时序。
 *
 * 释放后 wrapper 的 token 在表里查不到 -> done/result/call/__del__ 等
 * 一律走 "context closed" 路径，绝不触碰已释放的 JS 堆 —— 保持
 * "wrapper 可以比 Context 长命"的既有安全模型不变。
 */
static void quickjs_ctx_release_entries(
    quickjs_ctx_t *state
) {
    if (state->ctx == NULL) {

        return;
    }

    /* Function wrapper entries */
    quickjs_value_entry_t *e =
        state->function_entries;

    while (e != NULL) {

        quickjs_value_entry_t *next =
            e->next;

        JS_FreeValue(
            state->ctx,
            e->val
        );

        m_del(
            quickjs_value_entry_t,
            e,
            1
        );

        e = next;
    }

    state->function_entries = NULL;

    /* Promise wrapper entries */
    e = state->promise_entries;

    while (e != NULL) {

        quickjs_value_entry_t *next =
            e->next;

        JS_FreeValue(
            state->ctx,
            e->val
        );

        m_del(
            quickjs_value_entry_t,
            e,
            1
        );

        e = next;
    }

    state->promise_entries = NULL;

    /* 阶段 7：resolve/reject wrapper 条目（ctx.promise()） */
    e = state->resolver_entries;

    while (e != NULL) {

        quickjs_value_entry_t *next =
            e->next;

        JS_FreeValue(
            state->ctx,
            e->val
        );

        m_del(
            quickjs_value_entry_t,
            e,
            1
        );

        e = next;
    }

    state->resolver_entries = NULL;
}


/* -------------------------------------------------------------------------- */
/* 阶段 5：wrapper token -> JS 值查找                                            */
/* -------------------------------------------------------------------------- */

/*
 * 在 state 的 function_entries / promise_entries 里按 token 查找 wrapper
 * 的 JS 值。Close 后、或条目已随 close() 释放时，一律视为
 * "context closed"。
 *
 * 两个变体：
 *   - quickjs_function_lookup / quickjs_promise_lookup：抛 MicroPython
 *     异常（用于 ctx.get/wrapper 方法等 Python 侧接口）；
 *   - quickjs_function_lookup_jserr / quickjs_promise_lookup_jserr：
 *     返回 JS_ThrowTypeError（用于 mp_to_quickjs pass-through 等
 *     JS 可见错误路径）。
 *
 * 返回值均为借用（borrowed）：调用方不得 JS_FreeValue，如需持久持有
 * 必须自行 JS_DupValue。
 */
static JSValue quickjs_function_lookup(
    mp_obj_quickjs_function_t *f
) {
    quickjs_ctx_t *state = f->state;

    if (state == NULL ||
        state->closed ||
        state->ctx == NULL ||
        state->rt == NULL) {

        mp_raise_msg(
            &mp_type_RuntimeError,
            MP_ERROR_TEXT(
                "context closed"
            )
        );
    }

    for (quickjs_value_entry_t *e =
             state->function_entries;
         e != NULL;
         e = e->next) {

        if (e->token == f->token) {

            return e->val;
        }
    }

    mp_raise_msg(
        &mp_type_RuntimeError,
        MP_ERROR_TEXT(
            "context closed"
        )
    );

    return JS_UNDEFINED; /* unreachable */
}


static JSValue quickjs_promise_lookup(
    mp_obj_quickjs_promise_t *p
) {
    quickjs_ctx_t *state = p->state;

    if (state == NULL ||
        state->closed ||
        state->ctx == NULL ||
        state->rt == NULL) {

        mp_raise_msg(
            &mp_type_RuntimeError,
            MP_ERROR_TEXT(
                "context closed"
            )
        );
    }

    for (quickjs_value_entry_t *e =
             state->promise_entries;
         e != NULL;
         e = e->next) {

        if (e->token == p->token) {

            return e->val;
        }
    }

    mp_raise_msg(
        &mp_type_RuntimeError,
        MP_ERROR_TEXT(
            "context closed"
        )
    );

    return JS_UNDEFINED; /* unreachable */
}


static JSValue quickjs_function_lookup_jserr(
    JSContext *ctx,
    mp_obj_quickjs_function_t *f
) {
    quickjs_ctx_t *state = f->state;

    if (state == NULL ||
        state->closed ||
        state->ctx == NULL ||
        state->rt == NULL) {

        return JS_ThrowTypeError(
            ctx,
            "context closed"
        );
    }

    if (state->ctx != ctx) {

        return JS_ThrowTypeError(
            ctx,
            "function belongs to another context"
        );
    }

    for (quickjs_value_entry_t *e =
             state->function_entries;
         e != NULL;
         e = e->next) {

        if (e->token == f->token) {

            return e->val;
        }
    }

    return JS_ThrowTypeError(
        ctx,
        "context closed"
    );
}


static JSValue quickjs_promise_lookup_jserr(
    JSContext *ctx,
    mp_obj_quickjs_promise_t *p
) {
    quickjs_ctx_t *state = p->state;

    if (state == NULL ||
        state->closed ||
        state->ctx == NULL ||
        state->rt == NULL) {

        return JS_ThrowTypeError(
            ctx,
            "context closed"
        );
    }

    if (state->ctx != ctx) {

        return JS_ThrowTypeError(
            ctx,
            "promise belongs to another context"
        );
    }

    for (quickjs_value_entry_t *e =
             state->promise_entries;
         e != NULL;
         e = e->next) {

        if (e->token == p->token) {

            return e->val;
        }
    }

    return JS_ThrowTypeError(
        ctx,
        "context closed"
    );
}


static mp_obj_t mod_quickjs_promise_done(
    mp_obj_t self_in
) {
    mp_obj_quickjs_promise_t *p =
        MP_OBJ_TO_PTR(self_in);

    quickjs_ctx_t *state =
        p->state;

    if (state == NULL ||
        state->closed ||
        state->ctx == NULL ||
        state->rt == NULL) {

        mp_raise_msg(
            &mp_type_RuntimeError,
            MP_ERROR_TEXT(
                "context closed"
            )
        );
    }

    JSPromiseStateEnum s =
        JS_PromiseState(
            state->ctx,
            quickjs_promise_lookup(p)
        );

    if (s == JS_PROMISE_NOT_A_PROMISE) {

        mp_raise_msg(
            &mp_type_TypeError,
            MP_ERROR_TEXT(
                "not a promise"
            )
        );
    }

    return mp_obj_new_bool(
        s != JS_PROMISE_PENDING
    );
}

static MP_DEFINE_CONST_FUN_OBJ_1(
    mod_quickjs_promise_done_obj,
    mod_quickjs_promise_done
);


/*
 * p.result() 的实现（state/p 已通过 open 检查）。
 * 单独抽出，便于外层用 enter/nlr/leave 包住（JSValue 转换可能触发
 * JS getter -> 回调 Python -> 可能调用 ctx.close()，必须在该窗口内）。
 */
static mp_obj_t mod_quickjs_promise_result_impl(
    quickjs_ctx_t *state,
    mp_obj_quickjs_promise_t *p
) {
    JSValue pval =
        quickjs_promise_lookup(p);

    JSPromiseStateEnum s =
        JS_PromiseState(
            state->ctx,
            pval
        );

    if (s == JS_PROMISE_NOT_A_PROMISE) {

        mp_raise_msg(
            &mp_type_TypeError,
            MP_ERROR_TEXT(
                "not a promise"
            )
        );
    }

    if (s == JS_PROMISE_PENDING) {

        mp_raise_msg(
            &mp_type_RuntimeError,
            MP_ERROR_TEXT(
                "promise not settled; call ctx.run_jobs()"
            )
        );
    }


    /*
     * JS_PromiseResult 返回新引用（quickjs.c 中 js_dup），归我们释放。
     */
    JSValue r =
        JS_PromiseResult(
            state->ctx,
            pval
        );


    if (s == JS_PROMISE_FULFILLED) {

        quickjs_convert_state_t st;
        memset(&st, 0, sizeof(st));

        mp_obj_t result =
            quickjs_to_mp_owned(
                state->ctx,
                r,
                r,
                &st
            );

        JS_FreeValue(
            state->ctx,
            r
        );

        return result;
    }


    /*
     * rejected：把 rejection 转成 Python 异常。
     * quickjs_raise_value 持有并释放 r（不经 pending-exception，
     * 不污染 JSContext 异常状态）。
     */
    quickjs_raise_value(
        state->ctx,
        r
    );

    return mp_const_none; /* unreachable */
}


static mp_obj_t mod_quickjs_promise_result(
    mp_obj_t self_in
) {
    mp_obj_quickjs_promise_t *p =
        MP_OBJ_TO_PTR(self_in);

    quickjs_ctx_t *state =
        p->state;

    if (state == NULL ||
        state->closed ||
        state->ctx == NULL ||
        state->rt == NULL) {

        mp_raise_msg(
            &mp_type_RuntimeError,
            MP_ERROR_TEXT(
                "context closed"
            )
        );
    }

    quickjs_ctx_enter(state);

    nlr_buf_t nlr;

    if (nlr_push(&nlr) == 0) {

        mp_obj_t result =
            mod_quickjs_promise_result_impl(
                state,
                p
            );

        nlr_pop();

        quickjs_ctx_leave(state);

        return result;

    } else {

        quickjs_ctx_leave(state);

        nlr_raise(nlr.ret_val);
    }

    return mp_const_none; /* unreachable */
}

static MP_DEFINE_CONST_FUN_OBJ_1(
    mod_quickjs_promise_result_obj,
    mod_quickjs_promise_result
);


/* -------------------------------------------------------------------------- */
/* Promise 双向桥接：p.then / p.catch / p.finally_                              */
/* -------------------------------------------------------------------------- */

/*
 * 这些方法使用 QuickJS 原生 Promise API：
 *
 *   - then / catch : JS_PromiseThen(ctx, promise, on_fulfilled, on_rejected)
 *     （嵌入 API；直接操作 intrinsic promise，不 consult Promise.prototype）
 *   - finally_     : JS_Invoke(promise, JS_ATOM_finally, ...)
 *     （原生 Promise.prototype.finally，保持 spec 语义）
 *
 * 回调 handler 复用阶段 3 的 callback 机制：
 *   - 新建无名回调节点（name = MP_OBJ_NULL），注册表持有节点（GC root）
 *   - CClosure 入口复用 quickjs_callback（JS->MP 转换 / Python 调用 /
 *     MP->JS 返回转换 / Python 异常 -> JS 异常 全复用）
 *   - 节点不持有 js_func 强引用：闭包由 promise reaction 持有，
 *     promise 被 JS GC 回收时闭包随之销毁（finalize 置 opaque_active=false），
 *     不会造成 JS 堆无限增长；节点本身随 ctx.close() 统一释放。
 *
 * 回调返回值语义（交给原生 promise 机制处理）：
 *   - 普通 Python 值 -> 按既有 Python->JS 转换 resolve
 *   - JS Function wrapper -> pass-through
 *   - Promise wrapper -> pass-through，原生 promise assimilation
 *   - None -> JS null（既有 None->null 语义）
 *   - 抛 Python 异常 -> CClosure 返回 JS_EXCEPTION，promise_reaction_job
 *     捕获并 reject 派生 promise（第 4 阶段已核实该路径）
 */

MP_DECLARE_CONST_FUN_OBJ_VAR_BETWEEN(mod_quickjs_promise_then_obj);
MP_DECLARE_CONST_FUN_OBJ_VAR_BETWEEN(mod_quickjs_promise_catch_obj);
MP_DECLARE_CONST_FUN_OBJ_VAR_BETWEEN(mod_quickjs_promise_finally_obj);



static const mp_rom_map_elem_t
quickjs_promise_locals_dict_table[] = {

    {
        MP_ROM_QSTR(MP_QSTR___del__),
        MP_ROM_PTR(
            &mod_quickjs_promise_del_obj
        )
    },

    {
        MP_ROM_QSTR(MP_QSTR_done),
        MP_ROM_PTR(
            &mod_quickjs_promise_done_obj
        )
    },

    {
        MP_ROM_QSTR(MP_QSTR_result),
        MP_ROM_PTR(
            &mod_quickjs_promise_result_obj
        )
    },

    {
        MP_ROM_QSTR(MP_QSTR_then),
        MP_ROM_PTR(
            &mod_quickjs_promise_then_obj
        )
    },

    {
        MP_ROM_QSTR(MP_QSTR_catch),
        MP_ROM_PTR(
            &mod_quickjs_promise_catch_obj
        )
    },

    {
        MP_ROM_QSTR(MP_QSTR_finally_),
        MP_ROM_PTR(
            &mod_quickjs_promise_finally_obj
        )
    },
};

static MP_DEFINE_CONST_DICT(
    quickjs_promise_locals_dict,
    quickjs_promise_locals_dict_table
);


MP_DEFINE_CONST_OBJ_TYPE(
    quickjs_promise_type,
    MP_QSTR_Promise,
    MP_TYPE_FLAG_NONE,
    locals_dict, &quickjs_promise_locals_dict
);


static mp_obj_t quickjs_promise_to_mp(
    JSContext *ctx,
    JSValueConst val
) {
    /*
     * 只有 Context 运行时有 state（通过 JS_GetContextOpaque 关联）。
     * 默认 singleton 的 ctx 没有 opaque -> 保持“unsupported”行为
     * （与 Function wrapper 一致），不做误导性的空 dict 转换。
     */
    quickjs_ctx_t *state =
        (quickjs_ctx_t *)JS_GetContextOpaque(ctx);

    if (state == NULL ||
        state->closed) {

        mp_raise_msg(
            &mp_type_TypeError,
            MP_ERROR_TEXT(
                "unsupported QuickJS value type"
            )
        );
    }

    mp_obj_quickjs_promise_t *p =
        mp_obj_malloc_with_finaliser(
            mp_obj_quickjs_promise_t,
            &quickjs_promise_type
        );

    p->state = state;
    p->ctx_obj = state->self_obj;
    p->token = ++state->next_token;

    /*
     * 阶段 5：dup 进条目表（state 所有），wrapper 只拿 token。
     */
    quickjs_value_entry_t *pe =
        m_new(
            quickjs_value_entry_t,
            1
        );

    pe->token = p->token;
    pe->val = JS_DupValue(ctx, val);
    pe->next = state->promise_entries;
    state->promise_entries = pe;

    return MP_OBJ_FROM_PTR(p);
}


/*
 * 阶段 10（OOM 审计修复）：把 owned 的 JS promise 包装成 Python wrapper。
 *
 * quickjs_promise_to_mp 内部做 MP 分配（wrapper 对象 + 条目表），可能
 * 因内存不足抛 MemoryError；若任其穿透，调用方持有的 q（owned JSValue）
 * 会在 JS_FreeValue(q) 之前被 nlr 跳过而永久泄漏（promise refcount > 0
 * 且无外部引用，JS GC 无法回收）。
 *
 * 本辅助函数保证：无论成功/失败，q 恰好被释放一次。
 */
static mp_obj_t quickjs_promise_wrap_owned(
    JSContext *ctx,
    JSValue q
) {
    nlr_buf_t nlr;

    if (nlr_push(&nlr) == 0) {

        mp_obj_t w =
            quickjs_promise_to_mp(
                ctx,
                q
            );

        nlr_pop();

        JS_FreeValue(ctx, q);

        return w;
    }

    JS_FreeValue(ctx, q);

    nlr_raise(nlr.ret_val);

    return mp_const_none; /* unreachable */
}


/* -------------------------------------------------------------------------- */
/* 阶段 7：ctx.promise() 的 resolve/reject wrapper                              */
/* -------------------------------------------------------------------------- */

/*
 * 生命周期设计与 Promise / Function wrapper 完全一致（同一套约定）：
 *
 *   - ctx_obj 是 state->self_obj 的强引用，wrapper 存活期间 Context
 *     不会被 MicroPython GC 提前回收 -> 裸 state 指针始终有效。
 *   - token 是 state->resolver_entries 条目表里的查找键；真正的 JS
 *     引用（JS_DupValue）由 state 的条目表持有，wrapper 不持有 JSValue。
 *   - __del__：仅当 state->ctx 仍存在时按 token 摘除条目并释放；
 *     Context close 后条目表已整体释放，直接跳过。
 *   - 调用（resolve(v) / reject(r)）：先检查 context 是否关闭，关闭则抛
 *     RuntimeError("context closed")。
 *
 * promise 状态机不在 Python 侧复制：调用即调用 QuickJS 原生 resolving
 * function（js_promise_resolve_function_call），首次调用定局、重复调用
 * 忽略、thenable/promise assimilation 全部由 QuickJS 负责。两个 resolving
 * function 共享同一 already_resolved 标志（同一 JSPromiseFunctionDataResolved），
 * 因此 resolve 与 reject 之间也遵循“先到先得”（标准语义）。
 */
typedef struct _mp_obj_quickjs_resolver_t {
    mp_obj_base_t base;

    quickjs_ctx_t *state;
    mp_obj_t ctx_obj;   /* 强引用：保证 Context 不被 GC（见上） */

    uint32_t token;      /* state->resolver_entries 的查找键 */
    bool is_reject;      /* true = reject wrapper；false = resolve wrapper */
} mp_obj_quickjs_resolver_t;


static mp_obj_t mod_quickjs_resolver_del(
    mp_obj_t self_in
) {
    mp_obj_quickjs_resolver_t *r =
        MP_OBJ_TO_PTR(self_in);

    if (r->state != NULL &&
        r->state->ctx != NULL) {

        quickjs_value_entry_t **pp =
            &r->state->resolver_entries;

        while (*pp != NULL) {

            if ((*pp)->token == r->token) {

                quickjs_value_entry_t *e =
                    *pp;

                *pp = e->next;

                JS_FreeValue(
                    r->state->ctx,
                    e->val
                );

                m_del(
                    quickjs_value_entry_t,
                    e,
                    1
                );

                break;
            }

            pp = &(*pp)->next;
        }
    }

    return mp_const_none;
}

static MP_DEFINE_CONST_FUN_OBJ_1(
    mod_quickjs_resolver_del_obj,
    mod_quickjs_resolver_del
);


/*
 * 在 state 的 resolver_entries 里按 token 查找 wrapper 的 JS 值。
 * Close 后、或条目已随 close() 释放时，一律抛 "context closed"。
 * 返回值是借用（borrowed）：调用方不得 JS_FreeValue。
 */
static JSValue quickjs_resolver_lookup(
    mp_obj_quickjs_resolver_t *r
) {
    quickjs_ctx_t *state = r->state;

    if (state == NULL ||
        state->closed ||
        state->ctx == NULL ||
        state->rt == NULL) {

        mp_raise_msg(
            &mp_type_RuntimeError,
            MP_ERROR_TEXT(
                "context closed"
            )
        );
    }

    for (quickjs_value_entry_t *e =
             state->resolver_entries;
         e != NULL;
         e = e->next) {

        if (e->token == r->token) {

            return e->val;
        }
    }

    mp_raise_msg(
        &mp_type_RuntimeError,
        MP_ERROR_TEXT(
            "context closed"
        )
    );

    return JS_UNDEFINED; /* unreachable */
}


/*
 * 阶段 7：把 Python 异常转成 JS Error 值（仅 reject(exception) 使用）。
 *
 * 复用阶段 3 callback 异常转换的既有机制（quickjs_callback 的异常分支）：
 * JS_ThrowTypeError("%s: %s", 类型名, str(exc))。区别只是消费方式：
 * callback 分支让它留在 pending-exception 并返回 JS_EXCEPTION 抛给 JS；
 * 这里在 JS_ThrowTypeError 之后立即 JS_GetException 取回为 JSValue，
 * 作为 promise 的 rejection reason 传给 reject 函数。
 *
 * 返回值 owned（JS_GetException 取回的新引用），失败时返回 JS_EXCEPTION。
 * msg 提取用第二层 nlr 保护（str(exc) 自身可能抛异常）。
 */
static JSValue quickjs_exception_to_js_error(
    JSContext *qctx,
    mp_obj_t exc
) {
    const char *type_name =
        mp_obj_get_type_str(exc);

    const char *msg = NULL;

    nlr_buf_t nlr;

    if (nlr_push(&nlr) == 0) {

        mp_obj_t args_arr[1];
        args_arr[0] = exc;

        mp_obj_t s =
            (mp_obj_t)mp_obj_str_make_new(
                &mp_type_str,
                1,
                0,
                args_arr
            );

        msg =
            mp_obj_str_get_str(s);

        nlr_pop();

    } else {

        msg = NULL;
    }

    JS_ThrowTypeError(
        qctx,
        "%s: %s",
        type_name,
        (msg != NULL)
            ? msg
            : "exception"
    );

    return JS_GetException(qctx);
}


/*
 * resolve(value) / reject(reason) Python callable。
 *
 *   - resolve(value)：转换 value 后调用 JS resolving function（JS_Call）。
 *     转换失败（unsupported 类型 / 大整数溢出等）走阶段 6 统一异常模型：
 *     抛 Python 异常，promise 保持 pending。
 *   - reject(reason)：同 resolve，但 reason 是 Python 异常时复用
 *     quickjs_exception_to_js_error 转成 JS Error 值再 reject（与
 *     callback 抛错产生的 rejection reason 一致）；其余类型同 resolve。
 *
 * 执行窗口：JS_Call 期间可能取 thenable 的 then 属性（getter -> 任意
 * JS / Python callback），因此必须 enter（close 在窗口内被拒绝 "busy"）、
 * arm/finish timeout（getter 死循环等仍被预算中断）、并包 nlr（转换与
 * JS 异常都经 quickjs_raise_* 抛 MP 异常）。
 *
 * Python 异常绝不穿过 QuickJS C stack：本函数所有失败路径都先把
 * JS 状态清理干净（转换失败释放 v / JS 异常经 quickjs_raise_exception_state
 * 消费 pending exception）再经 nlr 抛出。
 */
static mp_obj_t mod_quickjs_resolver_call(
    mp_obj_t self_in,
    size_t n_args,
    size_t n_kw,
    const mp_obj_t *args
) {
    mp_obj_quickjs_resolver_t *r =
        MP_OBJ_TO_PTR(self_in);

    quickjs_ctx_t *state =
        r->state;

    if (n_kw > 0) {

        mp_raise_msg(
            &mp_type_TypeError,
            MP_ERROR_TEXT(
                "resolver does not support keyword arguments"
            )
        );
    }

    if (n_args > 1) {

        mp_raise_msg(
            &mp_type_TypeError,
            MP_ERROR_TEXT(
                "resolver takes at most 1 value argument"
            )
        );
    }

    if (state == NULL ||
        state->closed ||
        state->ctx == NULL ||
        state->rt == NULL) {

        mp_raise_msg(
            &mp_type_RuntimeError,
            MP_ERROR_TEXT(
                "context closed"
            )
        );
    }

    quickjs_ctx_enter(state);

    nlr_buf_t nlr;

    if (nlr_push(&nlr) == 0) {

        JSContext *qctx =
            state->ctx;

        JSValue func =
            quickjs_resolver_lookup(r);

        quickjs_convert_state_t st;
        memset(&st, 0, sizeof(st));

        /*
         * 无参调用 -> JS undefined（与 JS resolve()/reject() 一致）。
         * 类型 call 槽的约定：args[0] 是第一个（也是唯一一个）用户参数，
         * self 经 self_in 单独传入（与 Function wrapper 一致）。
         * 转换出的 v 由本函数负责释放（JS_Call 不接管 argv）。
         */
        JSValue v = JS_UNDEFINED;
        bool have_v = false;

        if (n_args >= 1) {

            mp_obj_t value = args[0];

            if (r->is_reject &&
                mp_obj_is_exception_instance(value)) {

                v =
                    quickjs_exception_to_js_error(
                        qctx,
                        value
                    );

                if (JS_IsException(v)) {

                    quickjs_raise_exception(
                        qctx,
                        v
                    );
                }

            } else {

                v =
                    mp_to_quickjs(
                        qctx,
                        value,
                        &st
                    );

                if (JS_IsException(v)) {

                    quickjs_raise_exception(
                        qctx,
                        v
                    );
                }
            }

            have_v = true;
        }

        quickjs_ctx_arm_timeout(state);

        JSValue result =
            (have_v)
                ? JS_Call(
                      qctx,
                      func,
                      JS_UNDEFINED,
                      1,
                      &v
                  )
                : JS_Call(
                      qctx,
                      func,
                      JS_UNDEFINED,
                      0,
                      NULL
                  );

        bool timed_out =
            quickjs_ctx_finish_timeout(state);

        if (have_v) {

            JS_FreeValue(
                qctx,
                v
            );
        }

        if (JS_IsException(result)) {

            quickjs_raise_exception_state(
                qctx,
                result,
                timed_out
            );
        }

        /*
         * 成功调用（含“已定局、本次调用被忽略”）：
         * JS resolving function 返回 JS_UNDEFINED，释放后返回 None。
         */
        JS_FreeValue(
            qctx,
            result
        );

        nlr_pop();

        quickjs_ctx_leave(state);

        return mp_const_none;

    } else {

        quickjs_ctx_leave(state);

        nlr_raise(nlr.ret_val);
    }

    return mp_const_none; /* unreachable */
}

/*
 * resolve/reject 由 quickjs_resolver_type 的 call 槽直接调用
 * （mp_obj_is_callable -> 类型 call 槽），无需 FUN_OBJ 对象；
 * 参数范围 1..2（self + 可选 value）由 mp_arg_check_num 约束。
 */


static const mp_rom_map_elem_t
quickjs_resolver_locals_dict_table[] = {

    {
        MP_ROM_QSTR(MP_QSTR___del__),
        MP_ROM_PTR(
            &mod_quickjs_resolver_del_obj
        )
    },
};

static MP_DEFINE_CONST_DICT(
    quickjs_resolver_locals_dict,
    quickjs_resolver_locals_dict_table
);


MP_DEFINE_CONST_OBJ_TYPE(
    quickjs_resolver_type,
    MP_QSTR_PromiseResolver,
    MP_TYPE_FLAG_NONE,
    call, mod_quickjs_resolver_call,
    locals_dict, &quickjs_resolver_locals_dict
);


/*
 * JS resolving function -> Python callable wrapper（阶段 7）。
 * 与 quickjs_promise_to_mp 完全同构：dup 进 resolver_entries（state
 * 所有），wrapper 只拿 token。val 必须来自同一 Context（由调用方保证）。
 */
static mp_obj_t quickjs_resolver_to_mp(
    JSContext *ctx,
    JSValueConst val,
    bool is_reject
) {
    quickjs_ctx_t *state =
        (quickjs_ctx_t *)JS_GetContextOpaque(ctx);

    mp_obj_quickjs_resolver_t *r =
        mp_obj_malloc_with_finaliser(
            mp_obj_quickjs_resolver_t,
            &quickjs_resolver_type
        );

    r->state = state;
    r->ctx_obj = state->self_obj;
    r->token = ++state->next_token;
    r->is_reject = is_reject;

    /*
     * dup 进条目表（state 所有），wrapper 只拿 token。
     */
    quickjs_value_entry_t *re =
        m_new(
            quickjs_value_entry_t,
            1
        );

    re->token = r->token;
    re->val = JS_DupValue(ctx, val);
    re->next = state->resolver_entries;
    state->resolver_entries = re;

    return MP_OBJ_FROM_PTR(r);
}


/* -------------------------------------------------------------------------- */
/* Python callable -> JS CClosure（回调注册表）                                  */
/* -------------------------------------------------------------------------- */

/*
 * 节点生命周期设计（关键）：
 *
 *   - 节点由 m_new_obj 分配在 GC 堆，链入 state->callbacks。
 *     节点内的 mp_obj_t 字段（callable/name）被 GC 保守扫描，
 *     因此注册期间 Python callable 不会被提前回收（无需 static root）。
 *
 *   - js_func 持有对 CClosure 的引用（JS_DupValue 出的第二份）。
 *     全局属性（JS_SetPropertyStr 设置的）持有另一份。
 *
 *   - opaque = 节点指针，opaque_finalize = quickjs_cb_finalize：
 *     只在 closure 对象被销毁时置 opaque_active=false，绝不 free 节点。
 *     节点本身只由注册表（close / 重复注册覆盖）负责 unlink + m_del，
 *     且 m_del 严格排在 JS 资源释放之后，因此 opaque_finalize
 *     触碰的节点永远是存活节点，无 UAF。
 */
struct _quickjs_callback_t {
    struct _quickjs_callback_t *next;

    quickjs_ctx_t *state;

    mp_obj_t callable;  /* Python callable（GC root，经节点保守扫描） */
    mp_obj_t name;      /* 注册名（用于覆盖查找） */

    JSValue js_func;    /* 注册表持有的 CClosure 引用 */

    bool opaque_active; /* CClosure 的 opaque 仍指向本节点 */
    bool dead;          /* 闭包已被 JS GC 销毁（opaque 不再安全）；
                          由 quickjs_ctx_enter 的 reap 统一 unlink+m_del */
};


static void quickjs_cb_finalize(
    void *opaque
) {
    quickjs_callback_t *node =
        (quickjs_callback_t *)opaque;

    if (node != NULL) {

        node->opaque_active = false;
        node->dead = true;
    }
}


/*
 * JSCClosure 入口：JS 调用 Python callable。
 *
 * 绝对禁止 Python 异常直接穿越 QuickJS C stack：
 * 所有 Python 调用都包在 nlr 里，异常被捕获后
 * 转换成 JS 异常（JS_ThrowTypeError）并返回 JS_EXCEPTION。
 *
 * argv 是借用引用（QuickJS 持有），本函数不释放；
 * 转换出的 mp_obj_t 参数数组是临时数组，两种路径都释放。
 */
static JSValue quickjs_callback(
    JSContext *qctx,
    JSValueConst this_val,
    int argc,
    JSValueConst *argv,
    int magic,
    void *opaque
) {
    quickjs_callback_t *node =
        (quickjs_callback_t *)opaque;

    quickjs_ctx_t *state =
        (node != NULL)
            ? node->state
            : NULL;

    /* 防御：闭包只能在 context 打开时被调用 */
    if (state == NULL ||
        state->closed ||
        state->ctx == NULL ||
        state->rt == NULL ||
        node->callable == MP_OBJ_NULL ||
        !node->opaque_active) {

        return JS_ThrowTypeError(
            qctx,
            "context closed"
        );
    }

    /*
     * 阶段 5：callback 执行期间也计入重入深度（纵深防御）。
     * 正常路径外层窗口已计数（eval/call/run_jobs 都会 enter），这里
     * 再 +1 保证"callback 中调用 ctx.close()"必被拒绝；成功/异常
     * 两个路径都 leave，depth 严格恢复。
     */
    quickjs_ctx_enter(state);


    quickjs_convert_state_t st;
    memset(&st, 0, sizeof(st));

    mp_obj_t *mp_args = NULL;

    JSValue result = JS_UNDEFINED;

    nlr_buf_t nlr;

    if (nlr_push(&nlr) == 0) {

        if (argc > 0) {

            mp_args =
                m_new(
                    mp_obj_t,
                    argc
                );
        }

        /* JS -> MicroPython 参数 */
        for (int i = 0; i < argc; i++) {

            mp_args[i] =
                quickjs_to_mp_obj(
                    qctx,
                    argv[i],
                    &st
                );
        }

        /* 调用 Python callable */
        mp_obj_t ret =
            mp_call_function_n_kw(
                node->callable,
                (size_t)argc,
                0,
                mp_args
            );

        if (mp_args != NULL) {

            m_del(
                mp_obj_t,
                mp_args,
                argc
            );

            mp_args = NULL;
        }

        /* Python 返回值 -> JS（不支持的返回类型会设 JS 异常并返回 JS_EXCEPTION） */
        result =
            mp_to_quickjs(
                qctx,
                ret,
                &st
            );

        nlr_pop();

        quickjs_ctx_leave(state);

    } else {

        /* Python exception -> JS exception */
        if (mp_args != NULL) {

            m_del(
                mp_obj_t,
                mp_args,
                argc
            );
        }

        quickjs_ctx_leave(state);

        mp_obj_t exc =
            (mp_obj_t)nlr.ret_val;

        const char *type_name =
            mp_obj_get_type_str(exc);

        /*
         * 提取异常消息（str(exc)）。用第二层 nlr 保护，
         * 防止 str() 自身抛异常。
         */
        const char *msg = NULL;
        mp_obj_t s = MP_OBJ_NULL;

        nlr_buf_t nlr2;

        if (nlr_push(&nlr2) == 0) {

            mp_obj_t args_arr[1];
            args_arr[0] = exc;

            s =
                mp_obj_str_make_new(
                    &mp_type_str,
                    1,
                    0,
                    args_arr
                );

            msg =
                mp_obj_str_get_str(s);

            nlr_pop();

        } else {

            msg = NULL;
        }

        /*
         * msg 是 GC 字符串 s 内的缓冲区指针。
         * 到 JS_ThrowTypeError（只分配 JS 堆、不触发 MP GC）之间
         * 没有 MP 分配，因此 msg 保持有效；s 在调用期间存活。
         */
        JS_ThrowTypeError(
            qctx,
            "%s: %s",
            type_name,
            (msg != NULL)
                ? msg
                : "callback error"
        );

        result = JS_EXCEPTION;
    }

    return result;
}


/*
 * 释放一个回调节点（unlink + m_del）。
 *
 * 调用时机必须保证：相关 JSValue 已释放、closure 已 finalize，
 * 之后才释放节点，否则 opaque_finalize 会触碰悬垂节点。
 *
 * 调用方负责在调用前把 node 从链表中摘除（或传入 prev）。
 */
static void quickjs_callback_free_node(
    quickjs_callback_t *node
) {
    if (node == NULL) {
        return;
    }

    node->callable = MP_OBJ_NULL;
    node->name = MP_OBJ_NULL;
    node->state = NULL;
    node->js_func = JS_UNDEFINED;

    m_del(
        quickjs_callback_t,
        node,
        1
    );
}


/*
 * 阶段 10（OOM/生命周期审计修复）：
 * 统一回收被 JS GC 销毁的闭包节点（dead==true）。
 *
 * 安全论证：
 *   - dead 节点 = 对应 CClosure 已被引擎销毁且 finalizer
 *     （quickjs_cb_finalize）已经执行完毕；此后没有任何代码
 *     会再触碰节点的 opaque 指针，unlink + m_del 无 UAF。
 *   - 执行中的回调节点不是 dead（其闭包必存活）。
 *   - close() 路径先 JS_FreeContext（触发全部 finalizer -> dead），
 *     后按链表释放节点：这里回收过的节点已不在链表中，
 *     不会被 close 双重释放。
 *   - 覆盖注册（add_callable 同名）路径在 JS_FreeValue(js_func)
 *     之后 free_node：若节点已被本函数回收则链表已无它，
 *     直接 free 任意顺序都安全（free_node 内已置空所有字段）。
 */
static void quickjs_reap_dead_callbacks(
    quickjs_ctx_t *state
) {
    quickjs_callback_t **pp =
        &state->callbacks;

    while (*pp != NULL) {

        quickjs_callback_t *node =
            *pp;

        if (node->dead) {

            *pp = node->next;

            quickjs_callback_free_node(node);

        } else {

            pp = &node->next;
        }
    }
}


/* -------------------------------------------------------------------------- */
/* 阶段 9：Unhandled Promise Rejection 诊断（JS_SetHostPromiseRejectionTracker） */
/* -------------------------------------------------------------------------- */

/*
 * handler 节点。与 quickjs_callback_t 同一套 GC 结论：
 *   节点 m_new_obj 分配在 GC 堆；state->rejection_handler 是 state 块
 *   （被标记）里的裸指针，保守扫描使节点保持存活；节点的
 *   callable / last_error（mp_obj_t 字段）随节点一起被扫描，
 *   因此 handler 在注册期间不会被 MicroPython GC 回收。
 *
 * 节点不生成 CClosure、不持有 JSValue：tracker 是纯 C 函数指针
 * （JSHostPromiseRejectionTracker），注册进 JSRuntime 即可。
 */
typedef struct _quickjs_rejection_handler_t {
    quickjs_ctx_t *state;
    mp_obj_t callable;    /* Python handler（GC root，经节点保守扫描） */
    mp_obj_t last_error;  /* handler 最近一次抛出的异常（记录，不重抛） */
} quickjs_rejection_handler_t;


/*
 * QuickJS rejection tracker 回调（quickjs.h:1174 的
 * JSHostPromiseRejectionTracker 签名，逐字对照 vendored 源码）。
 *
 * 调用时机（已在源码中确认）：
 *   - is_handled == false：fulfill_or_reject_promise() 在 reject 动作内
 *     同步调用（quickjs.c:55589），此时 promise 尚未挂任何 reaction；
 *   - is_handled == true ：perform_promise_then() 挂 handler 到
 *     已 rejected 的 promise 时同步调用（quickjs.c:56416），随后
 *     s->is_handled = true。
 *   不会在 Context close / Runtime free 期间调用（源码中 tracker 的
 *   全部 5 个调用点都不在 finalizer/free 路径上）——close 后不会再有
 *   本回调进入。
 *
 * promise / reason 都是【借用】（JSValueConst）：只在本次调用内使用，
 * 立即转换成 MicroPython 对象，绝不保存 JSValue。reason 是
 * s->promise_result（已定局的 rejection 值），转换语义与 p.result()
 * 完全一致（Error 对象 -> {}，字符串 -> str，等等）。
 *
 * 异常模型：本函数可能被 QuickJS 的 resolve 调用链中段调用
 * （fulfill_or_reject_promise 已写入 promise_result / promise_state、
 * 尚未入队 reactions）。Python handler（或 reason 转换）抛出的
 * MicroPython 异常【绝不能】穿过 QuickJS C stack——否则 nlr 长跳会
 * 破坏 QuickJS 中间状态。策略：nlr 捕获 -> 吞掉并记录到
 * node->last_error（GC 安全），不重抛、不转 JS 异常。
 */
static void quickjs_promise_rejection_tracker(
    JSContext *qctx,
    JSValueConst promise,
    JSValueConst reason,
    bool is_handled,
    void *opaque
) {
    quickjs_ctx_t *state =
        (quickjs_ctx_t *)opaque;

    if (state == NULL ||
        state->closed ||
        state->ctx == NULL ||
        state->rt == NULL ||
        qctx != state->ctx) {

        return;
    }

    quickjs_rejection_handler_t *node =
        state->rejection_handler;

    if (node == NULL ||
        node->callable == MP_OBJ_NULL) {

        return;
    }

    /*
     * 纵深防御：与 quickjs_callback 一致，handler 执行期间也计入
     * 重入深度。正常路径本回调只在 JS 执行窗口内被调（eval/call/
     * run_jobs/resolve/then 都已 enter），这里再 +1 保证 handler 内
     * 调 ctx.close() 必被拒绝（RuntimeError("context is busy")）。
     */
    quickjs_ctx_enter(state);

    quickjs_convert_state_t st;
    memset(&st, 0, sizeof(st));

    nlr_buf_t nlr;

    if (nlr_push(&nlr) == 0) {

        mp_obj_t args[2];

        /* reason 是借用值，立即转换成 MP 对象（复制语义） */
        args[0] =
            quickjs_to_mp_obj(
                qctx,
                reason,
                &st
            );

        args[1] =
            mp_obj_new_bool(
                is_handled
            );

        mp_call_function_n_kw(
            node->callable,
            2,
            0,
            args
        );

        nlr_pop();

        quickjs_ctx_leave(state);

        return;

    } else {

        /* handler / reason 转换抛出的 Python 异常：吞掉并记录 */
        node->last_error =
            nlr.ret_val;

        quickjs_ctx_leave(state);

        return; /* 不 re-raise：绝不穿透 QuickJS C stack */
    }
}


/*
 * Python:
 *
 *     ctx.set_unhandled_rejection_handler(callable_or_none)
 *
 * 注册（或禁用）该 Context 的 promise rejection 诊断回调。回调签名：
 *
 *     handler(reason, is_handled)
 *
 *   - is_handled == False：promise 被 reject（同步，reject 动作内）；
 *   - is_handled == True ：rejection 被 .then/.catch 接管（同步）。
 *
 * handler 抛出的异常被吞掉并记录（见 quickjs_promise_rejection_tracker），
 * 不影响后续事件。传 None 解除注册（等价于删除）。
 */
static mp_obj_t mod_quickjs_ctx_set_unhandled_rejection_handler(
    mp_obj_t self_in,
    mp_obj_t callable
) {
    quickjs_ctx_t *state =
        quickjs_ctx_check_open(self_in);

    if (callable != mp_const_none &&
        !mp_obj_is_callable(callable)) {

        mp_raise_TypeError(
            MP_ERROR_TEXT(
                "handler must be callable or None"
            )
        );
    }

    /*
     * 替换/禁用：先摘除旧节点（m_del 解除对旧 callable 的引用），
     * 再写 tracker 注册。
     */
    if (state->rejection_handler != NULL) {

        quickjs_rejection_handler_t *old =
            state->rejection_handler;

        state->rejection_handler = NULL;

        old->state = NULL;
        old->callable = MP_OBJ_NULL;
        old->last_error = MP_OBJ_NULL;

        m_del(
            quickjs_rejection_handler_t,
            old,
            1
        );
    }

    if (callable == mp_const_none) {

        JS_SetHostPromiseRejectionTracker(
            state->rt,
            NULL,
            NULL
        );

        return mp_const_none;
    }

    quickjs_rejection_handler_t *node =
        m_new_obj(
            quickjs_rejection_handler_t
        );

    node->state = state;
    node->callable = callable;
    node->last_error = MP_OBJ_NULL;

    state->rejection_handler = node;

    JS_SetHostPromiseRejectionTracker(
        state->rt,
        quickjs_promise_rejection_tracker,
        state
    );

    return mp_const_none;
}

static MP_DEFINE_CONST_FUN_OBJ_2(
    mod_quickjs_ctx_set_unhandled_rejection_handler_obj,
    mod_quickjs_ctx_set_unhandled_rejection_handler
);


/* -------------------------------------------------------------------------- */
/* 执行超时（JS_SetInterruptHandler）                                           */
/* -------------------------------------------------------------------------- */

/*
 * interrupt handler：运行在 QuickJS 字节码执行过程中。
 *
 * 严格遵守限制：不调用任何 MicroPython API（不 mp_raise、不 nlr_raise、
 * 不 mp_obj_new_*、不触发 GC），只用 mp_hal_ticks_ms() 读时钟 + 置标志。
 * 返回 1 让 QuickJS 抛出 uncatchable "interrupted" 异常。
 */
static int quickjs_interrupt_handler(
    JSRuntime *rt,
    void *opaque
) {
    quickjs_ctx_t *state =
        (quickjs_ctx_t *)opaque;

    if (state == NULL ||
        !state->timeout_enabled ||
        state->deadline_ms == 0) {

        return 0; /* 继续执行 */
    }

    mp_uint_t now =
        mp_hal_ticks_ms();

    /* wrap-safe：now >= deadline */
    if ((mp_uint_t)(now - state->deadline_ms) <
        ((mp_uint_t)-1) / 2) {

        state->timeout_triggered = true;

        return 1; /* 中断 */
    }

    return 0;
}


/*
 * 每次 JS 执行（eval/call/函数 wrapper 调用）开始前调用：
 * 若启用了 timeout，打开一个超时窗口。窗口可嵌套（X）
 * （外层执行 -> Python callback -> 内层执行），见 timeout_depth 注释。
 */
static void quickjs_ctx_arm_timeout(
    quickjs_ctx_t *state
) {
    if (state == NULL ||
        !state->timeout_enabled) {

        return;
    }

    if (state->timeout_depth == 0) {

        /* 最外层窗口：设定本次（整条调用链）的超时预算 */
        state->deadline_ms =
            mp_hal_ticks_ms() +
            state->timeout_ms;

        state->timeout_triggered = false;
    }

    state->timeout_depth++;
}


/*
 * JS 执行结束后调用：关闭一个超时窗口。
 * 返回当前 triggered（本次执行的快速路径是否触发过超时中断）。
 * 只有最外层窗口关闭时才清除 deadline / triggered（见 timeout_depth
 * 注释）——超时状态不污染后续调用，但嵌套执行共享同一预算。
 */
static bool quickjs_ctx_finish_timeout(
    quickjs_ctx_t *state
) {
    if (state == NULL) {
        return false;
    }

    bool t =
        state->timeout_triggered;

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
/* Python: ctx.set_time_limit(ms)                                              */
/* -------------------------------------------------------------------------- */

static mp_obj_t mod_quickjs_ctx_set_time_limit(
    mp_obj_t self_in,
    mp_obj_t ms_obj
) {
    quickjs_ctx_t *state =
        quickjs_ctx_check_open(self_in);

    mp_int_t ms =
        mp_obj_get_int(ms_obj);

    if (ms < 0) {

        mp_raise_msg(
            &mp_type_ValueError,
            MP_ERROR_TEXT(
                "time limit must be non-negative"
            )
        );
    }

    if (ms == 0) {

        state->timeout_enabled = false;
        state->timeout_ms = 0;
        state->deadline_ms = 0;
        state->timeout_triggered = false;

    } else {

        state->timeout_enabled = true;
        state->timeout_ms =
            (mp_uint_t)ms;
    }

    return mp_const_none;
}

static MP_DEFINE_CONST_FUN_OBJ_2(
    mod_quickjs_ctx_set_time_limit_obj,
    mod_quickjs_ctx_set_time_limit
);


/*
 * Python:
 *
 *     ctx.add_callable("add", add)
 *
 * 后一次同名注册覆盖前一次：先释放旧回调的 JSValue 与节点引用。
 */
static mp_obj_t mod_quickjs_ctx_add_callable_impl(
    quickjs_ctx_t *state,
    mp_obj_t name_obj,
    mp_obj_t callable_obj
) {
    if (!mp_obj_is_callable(callable_obj)) {

        mp_raise_msg(
            &mp_type_TypeError,
            MP_ERROR_TEXT(
                "callable must be callable"
            )
        );
    }

    if (!mp_obj_is_str(name_obj)) {

        mp_raise_msg(
            &mp_type_ValueError,
            MP_ERROR_TEXT(
                "invalid function name"
            )
        );
    }

    {
        size_t name_len = 0;

        mp_obj_str_get_data(
            name_obj,
            &name_len
        );

        if (name_len == 0) {

            mp_raise_msg(
                &mp_type_ValueError,
                MP_ERROR_TEXT(
                    "invalid function name"
                )
            );
        }
    }


    const char *name =
        mp_obj_str_get_str(name_obj);


    /*
     * 创建 CClosure。
     * opaque = 节点（尚未分配）；先分配节点再创建闭包。
     */
    quickjs_callback_t *node =
        m_new_obj(
            quickjs_callback_t
        );

    memset(node, 0, sizeof(*node));

    node->state = state;
    node->callable = callable_obj;
    node->name = name_obj;
    node->opaque_active = true;
    node->js_func = JS_UNDEFINED;


    JSValue func =
        JS_NewCClosure(
            state->ctx,
            quickjs_callback,
            name,
            quickjs_cb_finalize,
            0,
            0,
            node
        );


    if (JS_IsException(func)) {

        /* 创建失败：释放节点（JSValue 已由 JS_NewCClosure 内部清理） */
        quickjs_callback_free_node(node);

        quickjs_raise_exception(
            state->ctx,
            func
        );

        return mp_const_none;
    }


    /*
     * 注册表持有第二份引用，用于 close / 覆盖时释放。
     */
    node->js_func =
        JS_DupValue(
            state->ctx,
            func
        );


    /*
     * 写入全局对象。JS_SetPropertyStr 成功/失败均接管 func 的所有权
     * （v0.16.1 set_value 直接把值 move 进属性槽）。
     */
    JSValue global =
        JS_GetGlobalObject(
            state->ctx
        );

    int rc =
        JS_SetPropertyStr(
            state->ctx,
            global,
            name,
            func
        );

    JS_FreeValue(
        state->ctx,
        global
    );


    if (rc < 0) {

        /*
         * 失败：释放注册表引用，再释放节点。
         * 此时闭包没有其他引用（全局没写入），会 finalize ->
         * opaque_finalize(node) 先执行（节点仍存活），然后我们 free 节点。
         */
        JS_FreeValue(
            state->ctx,
            node->js_func
        );

        quickjs_callback_free_node(node);

        quickjs_raise_exception(
            state->ctx,
            JS_UNDEFINED
        );

        return mp_const_none;
    }


    /*
     * 覆盖语义：若已有同名回调，释放旧回调。
     *
     * 顺序：先写入新属性（JS_SetPropertyStr 已 free 旧全局值），
     * 再释放旧节点 js_func（旧闭包 last ref -> finalize ->
     * opaque_finalize(旧节点) 置位，旧节点仍存活），
     * 最后 unlink + m_del 旧节点。
     */
    quickjs_callback_t *prev =
        NULL;

    quickjs_callback_t *old =
        state->callbacks;

    while (old != NULL) {

        if (old == node) {

            break;
        }

        /*
         * 无名节点（Promise handler 节点，name = MP_OBJ_NULL）
         * 不参与 add_callable 的名字覆盖匹配。
         * 绝不能对 MP_OBJ_NULL 调用 mp_obj_equal（会解引用空指针）。
         */
        if (old->name != MP_OBJ_NULL &&
            mp_obj_equal(
                old->name,
                name_obj
            )) {

            break;
        }

        prev = old;
        old = old->next;
    }

    if (old != NULL &&
        old != node) {

        /* unlink */
        if (prev != NULL) {
            prev->next = old->next;
        } else {
            state->callbacks = old->next;
        }

        /* 释放旧闭包引用（覆盖时全局槽已 free 旧值） */
        JS_FreeValue(
            state->ctx,
            old->js_func
        );

        quickjs_callback_free_node(old);
    }


    /* 新节点入链 */
    node->next = state->callbacks;
    state->callbacks = node;

    return mp_const_none;
}


/*
 * ctx.add_callable(name, callable) —— 重入保护窗口。
 * JS_NewCClosure / JS_SetPropertyStr（可能触发全局 setter）都会
 * 进入 JS 执行栈，可能回调 Python，因此包 enter/nlr/leave。
 */
static mp_obj_t mod_quickjs_ctx_add_callable(
    mp_obj_t self_in,
    mp_obj_t name_obj,
    mp_obj_t callable_obj
) {
    quickjs_ctx_t *state =
        quickjs_ctx_check_open(self_in);

    quickjs_ctx_enter(state);

    nlr_buf_t nlr;

    if (nlr_push(&nlr) == 0) {

        mp_obj_t result =
            mod_quickjs_ctx_add_callable_impl(
                state,
                name_obj,
                callable_obj
            );

        nlr_pop();

        quickjs_ctx_leave(state);

        return result;

    } else {

        quickjs_ctx_leave(state);

        nlr_raise(nlr.ret_val);
    }

    return mp_const_none; /* unreachable */
}

static MP_DEFINE_CONST_FUN_OBJ_3(
    mod_quickjs_ctx_add_callable_obj,
    mod_quickjs_ctx_add_callable
);


/* -------------------------------------------------------------------------- */
/* QuickJS exception -> MicroPython exception                                 */
/* -------------------------------------------------------------------------- */

/*
 * 提取 JS 异常对象的字符串属性（name / message / stack）。
 *
 * - 只在属性是字符串（含 rope）时返回（JS_ToCString 结果，需 JS_FreeCString）；
 * - 属性缺失/undefined/null/非字符串 -> 返回 NULL；
 * - 属性访问抛异常 -> 清除挂起异常，返回 NULL。
 */
static const char *quickjs_error_prop_str(
    JSContext *ctx,
    JSValueConst obj,
    const char *prop
) {
    JSValue v =
        JS_GetPropertyStr(
            ctx,
            obj,
            prop
        );

    if (JS_IsException(v)) {

        /* 访问属性本身失败（例如 Proxy），清除挂起异常。 */
        quickjs_clear_pending_exception(ctx);

        return NULL;
    }

    int tag = JS_VALUE_GET_TAG(v);

    if (tag != JS_TAG_STRING &&
        tag != JS_TAG_STRING_ROPE) {

        JS_FreeValue(ctx, v);

        return NULL;
    }

    const char *str =
        JS_ToCString(ctx, v);

    JS_FreeValue(ctx, v);

    if (str == NULL) {

        quickjs_clear_pending_exception(ctx);

        return NULL;
    }

    return str;
}


/*
 * QuickJS 异常 -> MicroPython 异常。
 *
 * 增强：优先从 Error 对象提取 name/message/stack，
 * 构造形如：
 *
 *   Error: hello
 *   stack:
 *       at ...
 *
 * 非 Error 的抛出值（throw "str" / throw 42）回退到 JS_ToCString。
 *
 * 流程严格保证：
 *   构建消息（vstr，GC 内存）
 *   -> 创建 MicroPython 异常（复制消息）
 *   -> 释放所有 JS 字符串 / 异常值
 *   -> nlr_raise
 *
 * 任何一步失败都用 nlr 保护，先释放 JS 资源再重抛，
 * 不会 use-after-free，不会泄漏。
 */
static void quickjs_raise_value(
    JSContext *ctx,
    JSValue exception_val
) {
    /*
     * 把调用方持有的 JSValue 异常值（或 promise rejection reason）
     * 格式化为 MicroPython 异常并 nlr_raise。exception_val 的所有权
     * 移交给本函数（负责释放）。
     *
     * 与 quickjs_raise_exception 的区别：这里直接处理值本身，不从 ctx
     * 取回 pending exception —— rejected promise 的 result() 场景下，
     * rejection 是 promise 内部状态，并非 JSContext 上的 pending
     * exception，不能走 JS_GetException 路径。
     *
     * 流程严格保证：
     *   构建消息（vstr，GC 内存）
     *   -> 创建 MicroPython 异常（复制消息）
     *   -> 释放所有 JS 字符串 / 异常值
     *   -> nlr_raise
     *
     * 任何一步失败都用 nlr 保护，先释放 JS 资源再重抛，
     * 不会 use-after-free，不会泄漏。
     */
    const char *name = NULL;
    const char *message = NULL;
    const char *stack = NULL;

    if (JS_VALUE_GET_TAG(exception_val) ==
        JS_TAG_OBJECT) {

        name =
            quickjs_error_prop_str(ctx, exception_val, "name");

        message =
            quickjs_error_prop_str(ctx, exception_val, "message");

        stack =
            quickjs_error_prop_str(ctx, exception_val, "stack");
    }


    const char *fallback = NULL;

    if (name == NULL && message == NULL) {

        fallback =
            JS_ToCString(ctx, exception_val);

        if (fallback == NULL) {
            quickjs_clear_pending_exception(ctx);
        }
    }


    vstr_t vstr;
    bool vstr_ok = false;
    mp_obj_t exc = MP_OBJ_NULL;
    nlr_buf_t nlr;

    if (nlr_push(&nlr) == 0) {

        vstr_init(&vstr, 64);
        vstr_ok = true;

        if (name != NULL &&
            message != NULL) {

            vstr_add_str(&vstr, name);
            vstr_add_str(&vstr, ": ");
            vstr_add_str(&vstr, message);

        } else if (name != NULL) {

            vstr_add_str(&vstr, name);

        } else if (message != NULL) {

            vstr_add_str(&vstr, message);

        } else if (fallback != NULL) {

            vstr_add_str(&vstr, fallback);

        } else {

            vstr_add_str(&vstr, "QuickJS exception");
        }


        if (stack != NULL &&
            stack[0] != '\0' &&
            vstr_len(&vstr) > 0) {

            vstr_add_str(&vstr, "\nstack:\n");
            vstr_add_str(&vstr, stack);
        }


        vstr_null_terminated_str(&vstr);

        exc =
            mp_obj_new_exception_msg_varg(
                &mp_type_RuntimeError,
                MP_ERROR_TEXT("%s"),
                vstr.buf
            );

        nlr_pop();

    } else {

        /*
         * 构建/创建异常失败：先释放 JS 资源再重抛。
         */
        if (vstr_ok) {
            vstr_clear(&vstr);
        }

        if (name != NULL) {
            JS_FreeCString(ctx, name);
        }
        if (message != NULL) {
            JS_FreeCString(ctx, message);
        }
        if (stack != NULL) {
            JS_FreeCString(ctx, stack);
        }
        if (fallback != NULL) {
            JS_FreeCString(ctx, fallback);
        }

        JS_FreeValue(ctx, exception_val);

        nlr_raise(nlr.ret_val);
    }


    /*
     * 成功路径：释放 JS 资源后再抛出。
     * 消息已经被 mp_obj_new_exception_msg_varg 拷贝进异常对象。
     */
    vstr_clear(&vstr);

    if (name != NULL) {
        JS_FreeCString(ctx, name);
    }
    if (message != NULL) {
        JS_FreeCString(ctx, message);
    }
    if (stack != NULL) {
        JS_FreeCString(ctx, stack);
    }
    if (fallback != NULL) {
        JS_FreeCString(ctx, fallback);
    }

    JS_FreeValue(ctx, exception_val);

    nlr_raise(exc);
}


/*
 * QuickJS 挂起异常（JS_GetException）-> MicroPython 异常。
 *
 * 与 quickjs_raise_value 的区别：从 ctx 取回 pending exception 后
 * 交给 quickjs_raise_value 格式化。val 是调用方传入的 JS_EXCEPTION
 * 哨兵（或 JS_UNDEFINED），无引用计数，JS_FreeValue 是无操作。
 */
static void quickjs_raise_exception(
    JSContext *ctx,
    JSValue val
) {
    JSValue exception_val =
        JS_GetException(ctx);

    JS_FreeValue(ctx, val);

    if (JS_IsUninitialized(exception_val)) {

        mp_raise_msg(
            &mp_type_RuntimeError,
            MP_ERROR_TEXT("QuickJS exception")
        );
    }

    quickjs_raise_value(
        ctx,
        exception_val
    );
}


/*
 * 阶段 3：带 timeout 标志的异常抛出。
 *
 * 若本次 JS 执行因 interrupt（超时）被中断，QuickJS 抛出的
 * 是 uncatchable "InternalError: interrupted"。这里消费掉挂起的
 * 异常使 context 恢复可用，再抛出清晰的 MicroPython 超时异常。
 */
static void quickjs_raise_exception_state(
    JSContext *ctx,
    JSValue val,
    bool timed_out
) {
    if (timed_out) {

        JSValue exc =
            JS_GetException(ctx);

        if (!JS_IsUninitialized(exc)) {

            JS_FreeValue(ctx, exc);
        }

        JS_FreeValue(ctx, val);

        mp_raise_msg(
            &mp_type_RuntimeError,
            MP_ERROR_TEXT(
                "JavaScript execution timeout"
            )
        );
    }

    quickjs_raise_exception(
        ctx,
        val
    );
}


/* -------------------------------------------------------------------------- */
/* 辅助函数                                                                    */
/* -------------------------------------------------------------------------- */

/*
 * 取回并释放 QuickJS 上下文中挂起的异常。
 *
 * 用于非致命错误路径：某些失败（例如 JS_ToCStringLen 失败、
 * 跳过某个不可转换的对象属性）会让 QuickJS 留下“待取回”的异常，
 * 如果不清除，后续 JS 调用会看到陈旧的异常状态。
 */
static void quickjs_clear_pending_exception(
    JSContext *ctx
) {
    JSValue exc = JS_GetException(ctx);

    if (!JS_IsUninitialized(exc)) {

        JS_FreeValue(
            ctx,
            exc
        );
    }
}


/*
 * 把 JSValue 转换为 MicroPython 对象，并在转换抛出 MicroPython
 * 异常时释放 owned 持有的 JSValue 引用。
 *
 * 转换函数内部可能因为“不支持的嵌套类型”等原因 mp_raise 抛异常，
 * 该异常会越过调用者直接跳转。如果调用者此前持有 owned 引用，
 * 就会泄漏。这里用 nlr 保护：异常时先释放 owned，再重新抛出。
 */
static mp_obj_t quickjs_to_mp_owned(
    JSContext *ctx,
    JSValueConst val,
    JSValue owned,
    quickjs_convert_state_t *st
) {
    nlr_buf_t nlr;

    if (nlr_push(&nlr) == 0) {

        mp_obj_t result =
            quickjs_to_mp_obj(
                ctx,
                val,
                st
            );

        nlr_pop();

        return result;
    }

    JS_FreeValue(
        ctx,
        owned
    );

    nlr_raise(nlr.ret_val);

    return mp_const_none; /* unreachable */
}


/* -------------------------------------------------------------------------- */
/* JS job queue（microtask 泵）                                                 */
/* -------------------------------------------------------------------------- */

/*
 * 执行当前 Runtime 的 pending jobs（microtasks），直到队列清空。
 *
 * 返回执行的 job 数量（无 job 时返回 0）。
 *
 * 设计要点：
 *
 *   - 复用 js_std_loop 的泵模式（quickjs-libc.c）：
 *     JS_ExecutePendingJob 循环直到返回 0；每个 job 执行时可能入队新 job
 *     （.then 链 / async），因此必须循环而非只执行一次。
 *
 *   - JS_ExecutePendingJob 返回值语义（quickjs.c:2531）：
 *       < 0   job 执行抛出 uncatchable 异常（interrupt/"interrupted"/OOM），
 *             异常挂在返回的 *pctx 上；
 *       0     无 pending job；
 *       1     成功执行一个 job。
 *
 *   - Promise rejection 不是执行错误：promise_reaction_job 内部会消费普通
 *     异常并转给 reject 函数，因此 rejected promise 不会让本函数抛 Python
 *     异常；rejection 作为 promise 状态由 wrapper 的 result() 呈现。
 *
 *   - 整个泵过程包在 timeout 窗口内（arm/finish），超时中断（uncatchable
 *     "interrupted"）会在此转成 timeout 异常。
 *
 *   - state 可空：NULL 表示默认 singleton（无 timeout，用全局 rt/ctx）。
 */
static mp_obj_t quickjs_run_jobs_helper(
    quickjs_ctx_t *state
) {
    JSContext *qctx =
        (state != NULL)
            ? state->ctx
            : ctx;

    JSRuntime *qrt =
        (state != NULL)
            ? state->rt
            : rt;

    int count = 0;
    JSContext *err_ctx = NULL;


    quickjs_ctx_arm_timeout(state);

    for (;;) {

        JSContext *jctx = NULL;

        int err =
            JS_ExecutePendingJob(
                qrt,
                &jctx
            );

        if (err <= 0) {

            if (err < 0) {

                err_ctx =
                    (jctx != NULL)
                        ? jctx
                        : qctx;
            }

            break;
        }

        count++;
    }


    bool timed_out =
        quickjs_ctx_finish_timeout(state);


    if (err_ctx != NULL) {

        /*
         * job 执行失败：消费挂在 *pctx 上的异常并转成 Python 异常。
         * 超时中断则转成 timeout 异常。异常抛出后 runtime 恢复可用，
         * 剩余 job 仍留在队列，后续可再次 run_jobs()。
         */
        quickjs_raise_exception_state(
            err_ctx,
            JS_UNDEFINED,
            timed_out
        );
    }


    return mp_obj_new_int(count);
}


/* ========================================================================= */
/*                         JS -> MicroPython                                  */
/* ========================================================================= */


/* -------------------------------------------------------------------------- */
/* JS String -> MicroPython str                                               */
/* -------------------------------------------------------------------------- */

static mp_obj_t quickjs_string_to_mp(
    JSContext *ctx,
    JSValueConst val
) {
    size_t len = 0;

    const char *str =
        JS_ToCStringLen(
            ctx,
            &len,
            val
        );

    if (str == NULL) {

        /* JS_ToCStringLen 失败会在 ctx 挂起异常，取回并释放。 */
        quickjs_clear_pending_exception(ctx);

        mp_raise_msg(
            &mp_type_RuntimeError,
            MP_ERROR_TEXT(
                "failed to convert JS string"
            )
        );
    }

    /*
     * 阶段 10（OOM 审计修复）：mp_obj_new_str 复制字节，可能因 MP
     * 内存不足抛 MemoryError。若不在 nlr 内先释放 str，JS_ToCStringLen
     * 留下的字符串引用会永久泄漏（rope 情形额外泄漏整个扁平化副本；
     * 实测 js_mem 增长 39 KB）。用 nlr 保护，异常路径先 JS_FreeCString
     * 再重抛（与 quickjs_typedarray_to_mp 同一模式）。
     */
    nlr_buf_t nlr;

    if (nlr_push(&nlr) == 0) {

        mp_obj_t result =
            mp_obj_new_str(
                str,
                len
            );

        nlr_pop();

        JS_FreeCString(
            ctx,
            str
        );

        return result;

    } else {

        JS_FreeCString(
            ctx,
            str
        );

        nlr_raise(nlr.ret_val);
    }

    return mp_const_none; /* unreachable */
}


/* -------------------------------------------------------------------------- */
/* JS Array -> MicroPython list                                               */
/* -------------------------------------------------------------------------- */

static mp_obj_t quickjs_array_to_mp(
    JSContext *ctx,
    JSValueConst val,
    quickjs_convert_state_t *st
) {
    uint32_t len = 0;

    JSValue length_val =
        JS_GetPropertyStr(
            ctx,
            val,
            "length"
        );

    if (JS_IsException(length_val)) {

        quickjs_raise_exception(
            ctx,
            length_val
        );

        return mp_const_none;
    }

    if (JS_ToUint32(
            ctx,
            &len,
            length_val
        ) < 0) {

        JS_FreeValue(
            ctx,
            length_val
        );

        quickjs_raise_exception(
            ctx,
            JS_UNDEFINED
        );

        return mp_const_none;
    }

    JS_FreeValue(
        ctx,
        length_val
    );


    mp_obj_t *items = NULL;
    bool pushed = false;

    /*
     * 整体 nlr 保护：
     * - push（循环/深度检测）抛异常时不 pop（pushed==false），释放 items。
     * - m_new 分配 items 失败时释放 items（仍为 NULL）。
     * - 嵌套元素转换抛异常时，quickjs_to_mp_owned 已释放该元素 JSValue，
     *   这里负责 pop + 释放 items 数组。
     */
    nlr_buf_t nlr;

    if (nlr_push(&nlr) == 0) {

        quickjs_convert_push_mp(
            st,
            JS_VALUE_GET_PTR(val)
        );

        pushed = true;

        if (len > 0) {
            items = m_new(
                mp_obj_t,
                len
            );
        }


        for (uint32_t i = 0; i < len; i++) {

            JSValue item =
                JS_GetPropertyUint32(
                    ctx,
                    val,
                    i
                );

            if (JS_IsException(item)) {

                quickjs_raise_exception(
                    ctx,
                    item
                );

                /* unreachable */
            }


            /*
             * 转换抛出 MicroPython 异常时，
             * quickjs_to_mp_owned 负责释放 item。
             */
            items[i] =
                quickjs_to_mp_owned(
                    ctx,
                    item,
                    item,
                    st
                );


            JS_FreeValue(
                ctx,
                item
            );
        }


        nlr_pop();

    } else {

        if (pushed) {
            quickjs_convert_pop(st);
        }

        if (items != NULL) {
            m_del(
                mp_obj_t,
                items,
                len
            );
        }

        nlr_raise(nlr.ret_val);
    }


    if (pushed) {
        quickjs_convert_pop(st);
    }


    mp_obj_t result =
        mp_obj_new_list(
            len,
            items
        );


    if (items != NULL) {
        m_del(
            mp_obj_t,
            items,
            len
        );
    }

    return result;
}


/* -------------------------------------------------------------------------- */
/* JS Object -> MicroPython dict                                              */
/* -------------------------------------------------------------------------- */

static mp_obj_t quickjs_object_to_mp(
    JSContext *ctx,
    JSValueConst val,
    quickjs_convert_state_t *st
) {
    JSPropertyEnum *props = NULL;
    uint32_t prop_count = 0;

    int ret =
        JS_GetOwnPropertyNames(
            ctx,
            &props,
            &prop_count,
            val,
            JS_GPN_STRING_MASK |
            JS_GPN_ENUM_ONLY
        );

    if (ret < 0) {

        quickjs_raise_exception(
            ctx,
            JS_UNDEFINED
        );

        return mp_const_none;
    }


    mp_obj_t result = MP_OBJ_NULL;

    bool pushed = false;

    /*
     * 外层 nlr：任何属性转换抛异常时，
     * pop 循环检测栈 + 释放 props（JS_FreePropertyEnum）。
     * 内层按属性 nlr 负责释放 value / key / key_val。
     */
    nlr_buf_t outer;

    if (nlr_push(&outer) == 0) {

        /*
         * 阶段 10（OOM 审计修复）：mp_obj_new_dict 必须在 nlr 保护内
         * 分配。否则它因 MP 内存不足抛 MemoryError 时，props
         * （JSPropertyEnum 数组 + 每个属性的 atom 引用）会绕过
         * JS_FreePropertyEnum 而永久泄漏（LSan 实测 4.6 MB）。
         */
        result =
            mp_obj_new_dict(
                prop_count
            );

        quickjs_convert_push_mp(
            st,
            JS_VALUE_GET_PTR(val)
        );

        pushed = true;


        for (uint32_t i = 0;
             i < prop_count;
             i++) {

            JSAtom atom =
                props[i].atom;


            JSValue key_val =
                JS_AtomToString(
                    ctx,
                    atom
                );

            if (JS_IsException(key_val)) {

                quickjs_clear_pending_exception(ctx);

                continue;
            }


            size_t key_len = 0;

            const char *key =
                JS_ToCStringLen(
                    ctx,
                    &key_len,
                    key_val
                );

            if (key == NULL) {

                quickjs_clear_pending_exception(ctx);

                JS_FreeValue(
                    ctx,
                    key_val
                );

                continue;
            }


            JSValue value =
                JS_GetProperty(
                    ctx,
                    val,
                    atom
                );

            if (JS_IsException(value)) {

                quickjs_clear_pending_exception(ctx);

                JS_FreeCString(
                    ctx,
                    key
                );

                JS_FreeValue(
                    ctx,
                    key_val
                );

                continue;
            }


            {
                nlr_buf_t nlr;

                if (nlr_push(&nlr) == 0) {

                    mp_obj_t mp_key =
                        mp_obj_new_str(
                            key,
                            key_len
                        );

                    mp_obj_t mp_value =
                        quickjs_to_mp_obj(
                            ctx,
                            value,
                            st
                        );

                    mp_obj_dict_store(
                        MP_OBJ_TO_PTR(result),
                        mp_key,
                        mp_value
                    );

                    nlr_pop();

                } else {

                    /*
                     * 转换抛出 MicroPython 异常：
                     * 释放 value / key / key_val 后重新抛出。
                     */
                    JS_FreeValue(
                        ctx,
                        value
                    );

                    JS_FreeCString(
                        ctx,
                        key
                    );

                    JS_FreeValue(
                        ctx,
                        key_val
                    );

                    nlr_raise(nlr.ret_val);
                }
            }


            JS_FreeValue(
                ctx,
                value
            );

            JS_FreeCString(
                ctx,
                key
            );

            JS_FreeValue(
                ctx,
                key_val
            );
        }


        nlr_pop();

    } else {

        if (pushed) {
            quickjs_convert_pop(st);
        }

        JS_FreePropertyEnum(
            ctx,
            props,
            prop_count
        );

        nlr_raise(outer.ret_val);
    }


    if (pushed) {
        quickjs_convert_pop(st);
    }


    /*
     * 必须使用 JS_FreePropertyEnum：
     * 它会逐个释放每个 property 的 atom，再释放数组本身。
     * 直接 js_free(props) 会泄漏所有 atom。
     */
    JS_FreePropertyEnum(
        ctx,
        props,
        prop_count
    );

    return result;
}


/* -------------------------------------------------------------------------- */
/* JS ArrayBuffer -> MicroPython bytes                                         */
/* -------------------------------------------------------------------------- */

/*
 * copy 语义：mp_obj_new_bytes() 会把数据拷贝进 MicroPython GC 堆，
 * 不会借用 QuickJS 缓冲区指针（QuickJS 内存生命周期短于 Python 对象）。
 */
static mp_obj_t quickjs_arraybuffer_to_mp(
    JSContext *ctx,
    JSValueConst val
) {
    size_t size = 0;

    uint8_t *data =
        JS_GetArrayBuffer(
            ctx,
            &size,
            val
        );

    if (data == NULL) {

        /*
         * 失败会挂起异常（例如 detached buffer），
         * 统一转换成 MicroPython 异常。
         */
        quickjs_raise_exception(
            ctx,
            JS_UNDEFINED
        );

        return mp_const_none;
    }

    return mp_obj_new_bytes(
        data,
        size
    );
}


/* -------------------------------------------------------------------------- */
/* JS TypedArray -> MicroPython bytes（raw byte representation，阶段 5）        */
/* -------------------------------------------------------------------------- */

/*
 * 把任意 TypedArray（Int8/Uint8/Uint8Clamped/Int16/Uint16/Int32/Uint32/
 * Float32/Float64）转换为 bytes：**raw byte representation**。
 *
 * 明确语义（不是元素级转换）：
 *   - 返回的是该 typed array 视图 [byte_offset, byte_offset + byte_length)
 *     的原始字节（含 subarray/offset 视图的正确切片，与 JS_GetUint8Array
 *     的既有行为一致）。
 *   - 字节序是宿主原生字节序（QuickJS 直接按原生序读写 backing store）；
 *     不执行 signed/unsigned/float/大小端解释。
 *   - Float32/Float64 的字节是 IEEE-754（宿主字节序）。
 *   - detached / OOB buffer -> 由 JS API 挂起异常，统一转 Python 异常。
 *   - copy 语义：数据拷贝进 MP GC 堆，不借用 QuickJS 缓冲区指针。
 *
 * 因此不要把它当作"类型完全等价"的往返转换：bytes 只保留原始字节。
 * Python -> JS 仍只支持 bytes -> ArrayBuffer / bytearray -> Uint8Array。
 */
static mp_obj_t quickjs_typedarray_to_mp(
    JSContext *ctx,
    JSValueConst val
) {
    size_t off = 0;
    size_t len = 0;
    size_t bpe = 0;

    /*
     * JS_GetTypedArrayBuffer 返回 js_dup 的新引用（调用方持有），
     * 并给出视图的 byte_offset / byte_length / bytes_per_element。
     */
    JSValue abuf =
        JS_GetTypedArrayBuffer(
            ctx,
            val,
            &off,
            &len,
            &bpe
        );

    if (JS_IsException(abuf)) {

        quickjs_raise_exception(
            ctx,
            abuf
        );

        return mp_const_none;
    }

    size_t size = 0;

    uint8_t *data =
        JS_GetArrayBuffer(
            ctx,
            &size,
            abuf
        );

    if (data == NULL) {

        /* detached buffer -> 已挂起 TypeError，转 Python 异常 */
        JS_FreeValue(
            ctx,
            abuf
        );

        quickjs_raise_exception(
            ctx,
            JS_UNDEFINED
        );

        return mp_const_none;
    }

    /*
     * 先拷贝成 MP bytes（MP 分配不触发 JS，data 保持有效），
     * 再释放 abuf 引用（val 仍持有 buffer，data 不会悬垂）。
     * nlr 保护 mp_obj_new_bytes 的 OOM 路径。
     */
    nlr_buf_t nlr;

    if (nlr_push(&nlr) == 0) {

        mp_obj_t result =
            mp_obj_new_bytes(
                data + off,
                len
            );

        nlr_pop();

        JS_FreeValue(
            ctx,
            abuf
        );

        return result;

    } else {

        JS_FreeValue(
            ctx,
            abuf
        );

        nlr_raise(nlr.ret_val);
    }

    return mp_const_none; /* unreachable */
}


/* -------------------------------------------------------------------------- */
/* JS BigInt -> MicroPython int                                               */
/* -------------------------------------------------------------------------- */

/*
 * 把 BigInt 的十进制字符串严格解析为 int64，带溢出检测。
 *
 * 不能使用 JS_ToBigInt64()：它对超出 int64 的 BigInt 做 mod 2^64
 * 截断（quickjs.c JS_ToBigInt64Free），而不是报错，
 * 会静默产生错误数值。因此先取十进制字符串再逐位解析。
 */
static bool quickjs_bigint_str_to_i64(
    const char *s,
    int64_t *out
) {
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
    const uint64_t limit =
        neg ? neg_limit : pos_limit;

    uint64_t acc = 0;

    while (*s != '\0') {

        if (*s < '0' || *s > '9') {
            return false;
        }

        unsigned d =
            (unsigned)(*s - '0');

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


/*
 * BigInt -> int（仅限 int64 范围）。
 * 超出范围抛 TypeError（不静默转 float，不截断）。
 */
static mp_obj_t quickjs_bigint_to_mp(
    JSContext *ctx,
    JSValueConst val
) {
    size_t len = 0;

    const char *str =
        JS_ToCStringLen(
            ctx,
            &len,
            val
        );

    if (str == NULL) {

        quickjs_clear_pending_exception(ctx);

        mp_raise_msg(
            &mp_type_RuntimeError,
            MP_ERROR_TEXT(
                "failed to convert JS BigInt"
            )
        );
    }


    int64_t result = 0;

    if (!quickjs_bigint_str_to_i64(
            str,
            &result
        )) {

        JS_FreeCString(ctx, str);

        mp_raise_msg(
            &mp_type_TypeError,
            MP_ERROR_TEXT(
                "BigInt out of range"
            )
        );
    }


    JS_FreeCString(ctx, str);

    return mp_obj_new_int_from_ll(
        result
    );
}


/* -------------------------------------------------------------------------- */
/* JSValue -> MicroPython                                                     */
/* -------------------------------------------------------------------------- */

static mp_obj_t quickjs_to_mp_obj(
    JSContext *ctx,
    JSValueConst val,
    quickjs_convert_state_t *st
) {
    int tag =
        JS_VALUE_GET_TAG(val);


    switch (tag) {

        /* ---------------------------------------------------------------- */
        /* undefined / null                                                  */
        /* ---------------------------------------------------------------- */

        case JS_TAG_UNDEFINED:
            return mp_const_none;

        case JS_TAG_NULL:
            return mp_const_none;


        /* ---------------------------------------------------------------- */
        /* boolean                                                           */
        /* ---------------------------------------------------------------- */

        case JS_TAG_BOOL: {

            int b =
                JS_ToBool(
                    ctx,
                    val
                );

            return mp_obj_new_bool(b);
        }


        /* ---------------------------------------------------------------- */
        /* integer                                                           */
        /* ---------------------------------------------------------------- */

        case JS_TAG_INT: {

            int32_t value =
                JS_VALUE_GET_INT(val);

            return mp_obj_new_int(value);
        }


        /* ---------------------------------------------------------------- */
        /* string / string rope                                              */
        /* ---------------------------------------------------------------- */

        /*
         * QuickJS 中字符串有两种 tag：
         *   JS_TAG_STRING      普通字符串
         *   JS_TAG_STRING_ROPE 拼接产生的 rope 字符串
         * 两者都必须通过 JS_ToCStringLen() 转换。
         */
        case JS_TAG_STRING:
        case JS_TAG_STRING_ROPE:

            return quickjs_string_to_mp(
                ctx,
                val
            );


        /* ---------------------------------------------------------------- */
        /* BigInt                                                            */
        /* ---------------------------------------------------------------- */

        case JS_TAG_BIG_INT:
        case JS_TAG_SHORT_BIG_INT:

            return quickjs_bigint_to_mp(
                ctx,
                val
            );


        /* ---------------------------------------------------------------- */
        /* object                                                            */
        /* ---------------------------------------------------------------- */

        case JS_TAG_OBJECT: {

            /*
             * Promise：阶段 4 返回 Promise wrapper（Context 运行时）；
             * 默认 singleton 无 opaque -> 保持“不支持”行为。
             * 必须放在 Object->dict 之前，否则 Promise 会退化成空 dict。
             */
            if (JS_IsPromise(val)) {

                return quickjs_promise_to_mp(
                    ctx,
                    val
                );
            }

            /*
             * 函数：阶段 3 返回可调用 wrapper（Context 运行时）；
             * 默认 singleton 无 opaque -> 保持“不支持”行为。
             */
            if (JS_IsFunction(ctx, val)) {

                return quickjs_function_to_mp(
                    ctx,
                    val
                );
            }

            if (JS_IsArray(val)) {

                return quickjs_array_to_mp(
                    ctx,
                    val,
                    st
                );
            }

            /* ArrayBuffer -> bytes（copy 语义） */
            if (JS_IsArrayBuffer(val)) {

                return quickjs_arraybuffer_to_mp(
                    ctx,
                    val
                );
            }

            /*
             * TypedArray（阶段 5）：任意 TypedArray 都转换为 raw bytes
             * （视图切片，宿主字节序，见 quickjs_typedarray_to_mp）。
             * Uint8Array 的既有行为（bytes）保持不变。
             */
            {
                int ta =
                    JS_GetTypedArrayType(val);

                if (ta >= 0) {

                    return quickjs_typedarray_to_mp(
                        ctx,
                        val
                    );
                }
            }

            return quickjs_object_to_mp(
                ctx,
                val,
                st
            );
        }


        default:
            break;
    }


    /*
     * QuickJS-NG Number（含 float64）
     */
    if (JS_IsNumber(val)) {

        double value;

        if (JS_ToFloat64(
                ctx,
                &value,
                val
            ) < 0) {

            quickjs_clear_pending_exception(ctx);

            mp_raise_msg(
                &mp_type_TypeError,
                MP_ERROR_TEXT(
                    "failed to convert JS number"
                )
            );
        }

        return mp_obj_new_float(
            value
        );
    }


    mp_raise_msg(
        &mp_type_TypeError,
        MP_ERROR_TEXT(
            "unsupported QuickJS value type"
        )
    );

    return mp_const_none;
}


/* ========================================================================= */
/*                         MicroPython -> JS                                  */
/* ========================================================================= */


/* -------------------------------------------------------------------------- */
/* quickjs.bigint() 显式 BigInt 标记对象（阶段 5）                              */
/* -------------------------------------------------------------------------- */

/*
 * Python:
 *
 *     quickjs.bigint(value)   # value 是任意精度 Python int
 *
 * 返回一个内部标记对象，只有传入 QuickJS Context（ctx.set / 参数 / callback
 * 返回值等）时才被 mp_to_quickjs 转换为 JS BigInt。绝不改变普通
 * Python int -> JS number（int32/float64）的既有语义。
 *
 * 生命周期：对象只持有一个 mp_obj_t（Python int），无 JSValue、无
 * finaliser，由 MicroPython GC 保守扫描管理（int 是 GC 对象）。
 */
typedef struct _mp_obj_quickjs_bigint_t {
    mp_obj_base_t base;
    mp_obj_t value;   /* Python int（GC root，经本对象保守扫描） */
} mp_obj_quickjs_bigint_t;


static void quickjs_bigint_print(
    const mp_print_t *print,
    mp_obj_t self_in,
    mp_print_kind_t kind
) {
    mp_obj_quickjs_bigint_t *b =
        MP_OBJ_TO_PTR(self_in);

    /* 调试友好：打印底层 Python int 的十进制值 */
    mp_obj_print_helper(
        print,
        b->value,
        kind
    );
}


MP_DEFINE_CONST_OBJ_TYPE(
    quickjs_bigint_type,
    MP_QSTR_bigint,
    MP_TYPE_FLAG_NONE,
    print, quickjs_bigint_print
);


/*
 * Python:
 *
 *     quickjs.bigint(value)
 *
 * value 必须是 Python int（任意精度）。返回标记对象。
 */
static mp_obj_t mod_quickjs_bigint(
    mp_obj_t value_obj
) {
    if (!mp_obj_is_int(value_obj)) {

        mp_raise_msg(
            &mp_type_ValueError,
            MP_ERROR_TEXT(
                "bigint() requires an integer"
            )
        );
    }

    mp_obj_quickjs_bigint_t *b =
        mp_obj_malloc(
            mp_obj_quickjs_bigint_t,
            &quickjs_bigint_type
        );

    b->value = value_obj;

    return MP_OBJ_FROM_PTR(b);
}

static MP_DEFINE_CONST_FUN_OBJ_1(
    mod_quickjs_bigint_obj,
    mod_quickjs_bigint
);


/*
 * 标记对象 -> JS BigInt。
 *
 * 优先使用官方 C API：
 *   - int64 范围内 -> JS_NewBigInt64()（v0.16.1 公共 BigInt 创建 API）
 *   - 超出 int64  -> QuickJS v0.16.1 没有公共的任意精度 BigInt 创建 C API
 *     （js_bigint_from_string 是 static），因此用经过严格校验的十进制
 *     字面量通过 JS_Eval 构造："(digits)n" / "(-digits)n"。
 *     校验只允许 [+-]?[0-9]+，不可能注入代码。
 *
 * 不改变 ctx.set("x", 123) -> number 的行为（只有本类型才走这里）。
 */
static JSValue quickjs_bigint_wrapper_to_js(
    JSContext *ctx,
    mp_obj_t obj
) {
    mp_obj_quickjs_bigint_t *b =
        MP_OBJ_TO_PTR(obj);

    /*
     * 取十进制字符串：str(value)（MicroPython 对任意精度 int 输出纯十进制）。
     * s_obj 声明在函数作用域：后续 m_new 可能触发 MP GC，保守式栈扫描
     * 会把 s_obj 当作 root，保证 s（指向其缓冲区）在整段使用期间有效。
     * 用 nlr 保护，str() 理论上可能 OOM。
     */
    mp_obj_t s_obj = MP_OBJ_NULL;
    const char *s = NULL;

    nlr_buf_t nlr;

    if (nlr_push(&nlr) == 0) {

        s_obj =
            mp_obj_str_make_new(
                &mp_type_str,
                1,
                0,
                (mp_obj_t[]){ b->value }
            );

        s = mp_obj_str_get_str(s_obj);

        nlr_pop();

    } else {

        /* str() 失败（OOM）：吞掉并返回 JS 异常，避免跨 C 栈泄漏 */
        JS_ThrowTypeError(
            ctx,
            "cannot stringify bigint value"
        );

        return JS_EXCEPTION;
    }


    /*
     * 快路径：int64 范围内用官方 C API。
     * quickjs_bigint_str_to_i64 已严格校验 [+-]?[0-9]+ 并检测溢出。
     */
    int64_t i64 = 0;

    if (quickjs_bigint_str_to_i64(
            s,
            &i64
        )) {

        return JS_NewBigInt64(
            ctx,
            i64
        );
    }


    /*
     * 任意精度路径：构造字面量并 JS_Eval。
     * s 是 str(int) 的结果，必为 [+-]?[0-9]+；这里再显式校验一次
     * （纵深防御），任何非数字字符直接拒绝。
     */
    const char *p = s;
    bool neg = false;

    if (*p == '-') {
        neg = true;
        p++;
    } else if (*p == '+') {
        p++;
    }

    if (*p == '\0') {

        JS_ThrowTypeError(
            ctx,
            "invalid bigint value"
        );

        return JS_EXCEPTION;
    }

    for (; *p != '\0'; p++) {

        if (*p < '0' || *p > '9') {

            JS_ThrowTypeError(
                ctx,
                "invalid bigint value"
            );

            return JS_EXCEPTION;
        }
    }


    /*
     * 构造 "digitsn" 或 "-digitsn"（BigInt 字面量：数字后紧跟 n，同一个
     * token；不能用括号包起来，否则 n 会被解析成标识符）。
     * 数字已严格校验为 [0-9]+，不可能注入代码。
     */
    const char *digits = s;

    if (neg) {
        digits++;
    }

    size_t dlen = strlen(digits);

    /* "-"（可选） + digits + "n" + 终止符 */
    char *buf =
        m_new(
            char,
            dlen + (neg ? 2 : 1) + 1
        );

    size_t pos = 0;

    if (neg) {
        buf[pos++] = '-';
    }

    memcpy(
        buf + pos,
        digits,
        dlen
    );

    pos += dlen;

    buf[pos++] = 'n';
    buf[pos] = '\0';


    JSValue v =
        JS_Eval(
            ctx,
            buf,
            pos,
            "<bigint>",
            JS_EVAL_TYPE_GLOBAL
        );

    m_del(
        char,
        buf,
        dlen + (neg ? 2 : 1) + 1
    );

    return v;
}


/* -------------------------------------------------------------------------- */
/* Python str -> JS String                                                    */
/* -------------------------------------------------------------------------- */

static JSValue mp_str_to_quickjs(
    JSContext *ctx,
    mp_obj_t obj
) {
    size_t len = 0;

    const char *str =
        mp_obj_str_get_data(
            obj,
            &len
        );

    return JS_NewStringLen(
        ctx,
        str,
        len
    );
}


/* -------------------------------------------------------------------------- */
/* Python list -> JS Array                                                    */
/* -------------------------------------------------------------------------- */

static JSValue mp_list_to_quickjs(
    JSContext *ctx,
    mp_obj_t obj,
    quickjs_convert_state_t *st
) {
    size_t len;

    mp_obj_t *items;

    mp_obj_get_array(
        obj,
        &len,
        &items
    );


    JSValue array =
        JS_NewArray(ctx);

    if (JS_IsException(array)) {
        return array;
    }


    /*
     * 循环/深度检测（MP->JS 方向：失败设 JS 异常，不抛 Python 异常，
     * 因此下面所有错误路径都显式 pop + 释放 array）。
     */
    if (!quickjs_convert_push_js(
            ctx,
            st,
            MP_OBJ_TO_PTR(obj)
        )) {

        JS_FreeValue(ctx, array);

        return JS_EXCEPTION;
    }


    for (size_t i = 0; i < len; i++) {

        JSValue value =
            mp_to_quickjs(
                ctx,
                items[i],
                st
            );


        if (JS_IsException(value)) {

            JS_FreeValue(ctx, array);
            quickjs_convert_pop(st);

            return JS_EXCEPTION;
        }


        if (JS_SetPropertyUint32(
                ctx,
                array,
                i,
                value
            ) < 0) {

            /*
             * JS_SetPropertyUint32() 成功/失败均接管 value 所有权。
             */
            JS_FreeValue(ctx, array);
            quickjs_convert_pop(st);

            return JS_EXCEPTION;
        }
    }


    quickjs_convert_pop(st);

    return array;
}


/* -------------------------------------------------------------------------- */
/* Python tuple -> JS Array                                                   */
/* -------------------------------------------------------------------------- */

static JSValue mp_tuple_to_quickjs(
    JSContext *ctx,
    mp_obj_t obj,
    quickjs_convert_state_t *st
) {
    size_t len;

    mp_obj_t *items;

    mp_obj_get_array(
        obj,
        &len,
        &items
    );


    JSValue array =
        JS_NewArray(ctx);

    if (JS_IsException(array)) {
        return array;
    }


    if (!quickjs_convert_push_js(
            ctx,
            st,
            MP_OBJ_TO_PTR(obj)
        )) {

        JS_FreeValue(ctx, array);

        return JS_EXCEPTION;
    }


    for (size_t i = 0; i < len; i++) {

        JSValue value =
            mp_to_quickjs(
                ctx,
                items[i],
                st
            );


        if (JS_IsException(value)) {

            JS_FreeValue(ctx, array);
            quickjs_convert_pop(st);

            return JS_EXCEPTION;
        }


        if (JS_SetPropertyUint32(
                ctx,
                array,
                i,
                value
            ) < 0) {

            /*
             * 同 mp_list_to_quickjs：
             * JS_SetPropertyUint32() 已接管 value 所有权。
             */
            JS_FreeValue(ctx, array);
            quickjs_convert_pop(st);

            return JS_EXCEPTION;
        }
    }


    quickjs_convert_pop(st);

    return array;
}


/* -------------------------------------------------------------------------- */
/* Python dict -> JS Object                                                   */
/* -------------------------------------------------------------------------- */

static JSValue mp_dict_to_quickjs(
    JSContext *ctx,
    mp_obj_t obj,
    quickjs_convert_state_t *st
) {
    mp_map_t *map =
        mp_obj_dict_get_map(
            obj
        );


    JSValue object =
        JS_NewObject(ctx);

    if (JS_IsException(object)) {
        return object;
    }


    if (!quickjs_convert_push_js(
            ctx,
            st,
            MP_OBJ_TO_PTR(obj)
        )) {

        JS_FreeValue(ctx, object);

        return JS_EXCEPTION;
    }


    /*
     * MicroPython dict 是哈希表：活跃条目并非连续排在
     * table[0..used-1]，必须遍历全部 alloc 槽位并用
     * mp_map_slot_is_filled() 判断（参考 py/objdict.c dict_iter_next）。
     * 旧代码按 map->used 线性遍历是潜在 bug。
     */
    for (size_t i = 0;
         i < map->alloc;
         i++) {

        if (!mp_map_slot_is_filled(map, i)) {

            continue;
        }

        mp_map_elem_t *elem =
            &map->table[i];

        if (elem->value == MP_OBJ_NULL) {

            continue;
        }


        mp_obj_t key =
            elem->key;

        mp_obj_t value_obj =
            elem->value;


        /*
         * 当前要求 dict key 是 str。
         * 不在调用栈中途抛 Python 异常，而是设 JS 异常并返回。
         */
        if (!mp_obj_is_str(key)) {

            JS_FreeValue(ctx, object);
            quickjs_convert_pop(st);

            return JS_ThrowTypeError(
                ctx,
                "JS object keys must be str"
            );
        }


        size_t key_len;

        const char *key_str =
            mp_obj_str_get_data(
                key,
                &key_len
            );


        JSValue value =
            mp_to_quickjs(
                ctx,
                value_obj,
                st
            );


        if (JS_IsException(value)) {

            JS_FreeValue(ctx, object);
            quickjs_convert_pop(st);

            return JS_EXCEPTION;
        }


        if (JS_SetPropertyStr(
                ctx,
                object,
                key_str,
                value
            ) < 0) {

            /*
             * JS_SetPropertyStr() 已接管 value 所有权。
             */
            JS_FreeValue(ctx, object);
            quickjs_convert_pop(st);

            return JS_EXCEPTION;
        }
    }


    quickjs_convert_pop(st);

    return object;
}


/* -------------------------------------------------------------------------- */
/* MicroPython object -> JSValue                                              */
/* -------------------------------------------------------------------------- */

/*
 * JS Function wrapper -> JS（pass-through）。
 *
 * 场景（阶段 4）：
 *   - ctx.set("bar", f)：f 是同 Context 的 JS 函数 wrapper
 *   - Python callback 返回 JS 函数 wrapper（此前报
 *     "unsupported MicroPython type"）
 *
 * 安全性：
 *   - 只允许同 Context（同 Runtime）pass-through；跨 Context / 跨
 *     Runtime 一律拒绝 —— 绝不能把不同 Runtime 的 JSValue 混过去。
 *     该错误在 ctx.set 边界经 quickjs_raise_exception 映射为
 *     RuntimeError，在 callback 返回路径成为 JS 可见异常。
 *   - Context close / 条目已随 close() 释放后，quickjs_function_lookup_jserr
 *     直接报 context closed（JS 可见 TypeError），绝不触碰悬垂 JS 堆。
 *   - 返回 JS_DupValue(查到的值)（新引用），所有权交给外层
 *     （JS_SetProperty* 等 move 语义消费），无泄漏。
 */
static JSValue quickjs_function_pass_through(
    JSContext *ctx,
    mp_obj_t obj
) {
    mp_obj_quickjs_function_t *f =
        (mp_obj_quickjs_function_t *)MP_OBJ_TO_PTR(obj);

    JSValue v =
        quickjs_function_lookup_jserr(
            ctx,
            f
        );

    if (JS_IsException(v)) {

        return v;
    }

    return JS_DupValue(
        ctx,
        v
    );
}


/* -------------------------------------------------------------------------- */
/* JS Promise wrapper -> JS（pass-through，阶段 5）                             */
/* -------------------------------------------------------------------------- */

/*
 * Promise wrapper -> 其 promise JSValue（JS_DupValue 新引用）。
 *
 * 场景：Python callback 返回 Promise wrapper 时，mp_to_quickjs 需要把
 * 该 promise 原样传回 JS，让 promise_reaction_job 对它做原生 assimilation
 * （resolve 一个 thenable），而不是报“unsupported MicroPython type”。
 *
 * 安全性（与 Function pass-through 完全一致）：
 *   - 只允许同 Context（同 Runtime）；跨 Context / close 一律拒绝。
 *   - 返回 JS_DupValue（新引用），所有权交给外层 move 语义消费。
 */
static JSValue quickjs_promise_pass_through(
    JSContext *ctx,
    mp_obj_t obj
) {
    mp_obj_quickjs_promise_t *p =
        (mp_obj_quickjs_promise_t *)MP_OBJ_TO_PTR(obj);

    JSValue v =
        quickjs_promise_lookup_jserr(
            ctx,
            p
        );

    if (JS_IsException(v)) {

        return v;
    }

    return JS_DupValue(
        ctx,
        v
    );
}


/* -------------------------------------------------------------------------- */
/* Promise 双向桥接：handler 构造与 p.then / p.catch / p.finally_               */
/* -------------------------------------------------------------------------- */

/*
 * 为 Promise 方法创建一个 handler CClosure（无名回调节点）。
 *
 * 节点生命周期（与 add_callable 一致的安全模型）：
 *   - 节点 m_new_obj 分配在 GC 堆，链入 state->callbacks（GC root），
 *     因此 callable 在 handler 存活期间不会被回收。
 *   - 节点不持有 js_func 强引用：闭包由 promise reaction 持有。
 *     promise 被 JS GC 回收时闭包销毁 -> opaque_finalize 置
 *     opaque_active=false（节点仍存活），不会造成 JS 堆无限增长。
 *   - 节点随 ctx.close() 统一 unlink + m_del（close 先 JS_FreeContext
 *     再释放节点，opaque_finalize 触碰的永远是存活节点）。
 */
static JSValue quickjs_new_handler_closure(
    quickjs_ctx_t *state,
    mp_obj_t callable
) {
    quickjs_callback_t *node =
        m_new_obj(
            quickjs_callback_t
        );

    memset(node, 0, sizeof(*node));

    node->state = state;
    node->callable = callable;
    node->name = MP_OBJ_NULL;        /* 无名：不参与 add_callable 覆盖匹配 */
    node->opaque_active = true;
    node->js_func = JS_UNDEFINED;    /* 注册表不强引用闭包 */


    JSValue func =
        JS_NewCClosure(
            state->ctx,
            quickjs_callback,
            "<promise-handler>",
            quickjs_cb_finalize,
            0,
            0,
            node
        );

    if (JS_IsException(func)) {

        quickjs_callback_free_node(node);

        return func;
    }


    /* 先入链（成为 GC root）再返回闭包 */
    node->next = state->callbacks;
    state->callbacks = node;

    return func;
}


/*
 * 把 Python 侧的 promise handler 参数转成 JS handler 值（owned）。
 *
 *   - None / 缺省          -> JS_UNDEFINED（保持 JS 原生 then/catch/finally 语义）
 *   - JS Function wrapper  -> pass-through（同 Context；跨 Context 拒绝）
 *   - Python callable      -> 新建 handler CClosure（复用 quickjs_callback）
 *   - 其他                  -> TypeError
 *
 * 注意顺序：Function wrapper 也满足 mp_obj_is_callable，必须先判断 wrapper
 * 类型走 pass-through，而不是包一层 CClosure 再间接调用。
 */
static JSValue quickjs_promise_handler_to_js(
    quickjs_ctx_t *state,
    JSContext *qctx,
    mp_obj_t handler
) {
    if (handler == mp_const_none) {

        return JS_UNDEFINED;
    }

    if (mp_obj_is_type(
            handler,
            &quickjs_function_type
        )) {

        return quickjs_function_pass_through(
            qctx,
            handler
        );
    }

    if (mp_obj_is_callable(handler)) {

        return quickjs_new_handler_closure(
            state,
            handler
        );
    }

    return JS_ThrowTypeError(
        qctx,
        "promise handler must be callable or None"
    );
}


/*
 * 校验 handler 参数类型（不创建任何 JS 值）。用于在创建闭包之前
 * 尽早拒绝非法参数，避免失败路径留下僵尸回调节点。
 * 返回 false 时已抛出 Python 异常。
 */
static bool quickjs_promise_handler_check(
    mp_obj_t handler
) {
    if (handler == mp_const_none) {

        return true;
    }

    if (mp_obj_is_type(
            handler,
            &quickjs_function_type
        )) {

        return true;
    }

    if (mp_obj_is_callable(handler)) {

        return true;
    }

    mp_raise_msg(
        &mp_type_TypeError,
        MP_ERROR_TEXT(
            "promise handler must be callable or None"
        )
    );

    return false;
}


/*
 * Python:
 *
 *     q = p.then(on_fulfilled=None, on_rejected=None)
 *
 * 用 JS_PromiseThen 创建派生 promise 并返回新的 Promise wrapper。
 * 回调在 microtask 中执行，需 ctx.run_jobs() 驱动。
 */
static mp_obj_t mod_quickjs_promise_then(
    size_t n_args,
    const mp_obj_t *args
) {
    mp_obj_quickjs_promise_t *p =
        MP_OBJ_TO_PTR(args[0]);

    quickjs_ctx_t *state =
        p->state;

    if (state == NULL ||
        state->closed ||
        state->ctx == NULL ||
        state->rt == NULL) {

        mp_raise_msg(
            &mp_type_RuntimeError,
            MP_ERROR_TEXT(
                "context closed"
            )
        );
    }

    mp_obj_t on_fulfilled =
        (n_args >= 2)
            ? args[1]
            : mp_const_none;

    mp_obj_t on_rejected =
        (n_args >= 3)
            ? args[2]
            : mp_const_none;

    /* 创建闭包前先校验参数类型 */
    quickjs_promise_handler_check(on_fulfilled);
    quickjs_promise_handler_check(on_rejected);


    quickjs_ctx_enter(state);

    nlr_buf_t nlr;

    if (nlr_push(&nlr) == 0) {

        JSContext *qctx =
            state->ctx;

        JSValue pval =
            quickjs_promise_lookup(p);

        JSValue fh =
            quickjs_promise_handler_to_js(
                state,
                qctx,
                on_fulfilled
            );

        if (JS_IsException(fh)) {

            quickjs_raise_exception(
                qctx,
                fh
            );
        }

        JSValue rh =
            quickjs_promise_handler_to_js(
                state,
                qctx,
                on_rejected
            );

        if (JS_IsException(rh)) {

            JS_FreeValue(
                qctx,
                fh
            );

            quickjs_raise_exception(
                qctx,
                rh
            );
        }


        quickjs_ctx_arm_timeout(state);

        JSValue q =
            JS_PromiseThen(
                qctx,
                pval,
                fh,
                rh
            );

        bool timed_out =
            quickjs_ctx_finish_timeout(state);

        /* JS_PromiseThen 已 js_dup handlers 进 reaction；释放我们的引用 */
        JS_FreeValue(
            qctx,
            fh
        );

        JS_FreeValue(
            qctx,
            rh
        );

        if (JS_IsException(q)) {

            quickjs_raise_exception_state(
                qctx,
                q,
                timed_out
            );
        }

        mp_obj_t w =
            quickjs_promise_wrap_owned(
                qctx,
                q
            );

        nlr_pop();

        quickjs_ctx_leave(state);

        return w;

    } else {

        quickjs_ctx_leave(state);

        nlr_raise(nlr.ret_val);
    }

    return mp_const_none; /* unreachable */
}

/*
 * p.then(...)：最多 3 个参数（self, on_fulfilled, on_rejected）。
 */
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(
    mod_quickjs_promise_then_obj,
    1,
    3,
    mod_quickjs_promise_then
);


/*
 * Python:
 *
 *     q = p.catch(on_rejected=None)
 *
 * 等价于 p.then(None, on_rejected)。
 */
static mp_obj_t mod_quickjs_promise_catch(
    size_t n_args,
    const mp_obj_t *args
) {
    mp_obj_quickjs_promise_t *p =
        MP_OBJ_TO_PTR(args[0]);

    quickjs_ctx_t *state =
        p->state;

    if (state == NULL ||
        state->closed ||
        state->ctx == NULL ||
        state->rt == NULL) {

        mp_raise_msg(
            &mp_type_RuntimeError,
            MP_ERROR_TEXT(
                "context closed"
            )
        );
    }

    mp_obj_t on_rejected =
        (n_args >= 2)
            ? args[1]
            : mp_const_none;

    quickjs_promise_handler_check(on_rejected);


    quickjs_ctx_enter(state);

    nlr_buf_t nlr;

    if (nlr_push(&nlr) == 0) {

        JSContext *qctx =
            state->ctx;

        JSValue pval =
            quickjs_promise_lookup(p);

        JSValue rh =
            quickjs_promise_handler_to_js(
                state,
                qctx,
                on_rejected
            );

        if (JS_IsException(rh)) {

            quickjs_raise_exception(
                qctx,
                rh
            );
        }


        quickjs_ctx_arm_timeout(state);

        JSValue q =
            JS_PromiseThen(
                qctx,
                pval,
                JS_UNDEFINED,
                rh
            );

        bool timed_out =
            quickjs_ctx_finish_timeout(state);

        JS_FreeValue(
            qctx,
            rh
        );

        if (JS_IsException(q)) {

            quickjs_raise_exception_state(
                qctx,
                q,
                timed_out
            );
        }

        mp_obj_t w =
            quickjs_promise_wrap_owned(
                qctx,
                q
            );

        nlr_pop();

        quickjs_ctx_leave(state);

        return w;

    } else {

        quickjs_ctx_leave(state);

        nlr_raise(nlr.ret_val);
    }

    return mp_const_none; /* unreachable */
}

/*
 * p.catch(...)：最多 2 个参数（self, on_rejected）。
 */
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(
    mod_quickjs_promise_catch_obj,
    1,
    2,
    mod_quickjs_promise_catch
);


/*
 * Python:
 *
 *     q = p.finally_(callback=None)
 *
 * 调用原生 Promise.prototype.finally（经 JS_Invoke + JS_ATOM_finally），
 * 保持 spec 语义：回调返回值不影响派生的 fulfillment/rejection 值，
 * 回调抛异常会传播。callback 为 None 时等价于原生 finally(undefined)。
 */
static mp_obj_t mod_quickjs_promise_finally(
    size_t n_args,
    const mp_obj_t *args
) {
    mp_obj_quickjs_promise_t *p =
        MP_OBJ_TO_PTR(args[0]);

    quickjs_ctx_t *state =
        p->state;

    if (state == NULL ||
        state->closed ||
        state->ctx == NULL ||
        state->rt == NULL) {

        mp_raise_msg(
            &mp_type_RuntimeError,
            MP_ERROR_TEXT(
                "context closed"
            )
        );
    }

    mp_obj_t callback =
        (n_args >= 2)
            ? args[1]
            : mp_const_none;

    quickjs_promise_handler_check(callback);


    quickjs_ctx_enter(state);

    nlr_buf_t nlr;

    if (nlr_push(&nlr) == 0) {

        JSContext *qctx =
            state->ctx;

        JSValue pval =
            quickjs_promise_lookup(p);

        JSValue fh =
            quickjs_promise_handler_to_js(
                state,
                qctx,
                callback
            );

        if (JS_IsException(fh)) {

            quickjs_raise_exception(
                qctx,
                fh
            );
        }


        quickjs_ctx_arm_timeout(state);

        /*
         * 调用原生 Promise.prototype.finally：
         *   JS_GetPropertyStr(promise, "finally") 经原型链取到原生实现，
         *   JS_Call(finally_fn, this=promise, 1, {fh})。
         * 返回新 promise（owned）。原子/属性所有权：finally_fn 由我们释放。
         */
        JSValue finally_fn =
            JS_GetPropertyStr(
                qctx,
                pval,
                "finally"
            );

        if (JS_IsException(finally_fn)) {

            JS_FreeValue(
                qctx,
                fh
            );

            quickjs_raise_exception(
                qctx,
                finally_fn
            );
        }

        JSValue q =
            JS_Call(
                qctx,
                finally_fn,
                pval,
                1,
                &fh
            );


        bool timed_out =
            quickjs_ctx_finish_timeout(state);

        JS_FreeValue(
            qctx,
            finally_fn
        );

        JS_FreeValue(
            qctx,
            fh
        );

        if (JS_IsException(q)) {

            quickjs_raise_exception_state(
                qctx,
                q,
                timed_out
            );
        }

        mp_obj_t w =
            quickjs_promise_wrap_owned(
                qctx,
                q
            );

        nlr_pop();

        quickjs_ctx_leave(state);

        return w;

    } else {

        quickjs_ctx_leave(state);

        nlr_raise(nlr.ret_val);
    }

    return mp_const_none; /* unreachable */
}

/*
 * p.finally_(...)：最多 2 个参数（self, callback）。
 */
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(
    mod_quickjs_promise_finally_obj,
    1,
    2,
    mod_quickjs_promise_finally
);


/*
 * MP -> JS 转换的根入口。
 *
 * 阶段 6：把转换过程中抛出的任何 MicroPython 异常（例如
 * mp_obj_get_int 对超出 mp_int_t 的大整数抛 OverflowError）转成
 * JS 异常返回，而不是让 nlr 长跳越过失配的清理点直接逃逸。
 *
 * 转换层的不变量（各处注释反复强调）是：“MP -> JS 方向失败设 JS
 * 异常，不抛 Python 异常”——只有少数叶子转换（mp_obj_get_int 等）
 * 真正抛出，本包装器恢复这个不变量：所有调用方既有的
 * JS_IsException 清理路径（释放已转换参数 / this_val / 部分构建的
 * 容器 / set 的 global 对象）都能执行，不再泄漏 JSValue。容器/循环
 * 检测 tracker 的 pop 也由各自的清理路径完成，保持平衡。
 */
static JSValue mp_to_quickjs(
    JSContext *qctx,
    mp_obj_t obj,
    quickjs_convert_state_t *st
) {
    nlr_buf_t nlr;

    if (nlr_push(&nlr) == 0) {

        JSValue result =
            mp_to_quickjs_impl(
                qctx,
                obj,
                st
            );

        nlr_pop();

        return result;
    }


    /*
     * 转换中途抛出的 Python 异常 -> JS 异常（带类型名和消息）。
     * 消息提取用第二层 nlr 保护（str(exc) 自身可能抛异常）。
     */
    mp_obj_t exc =
        (mp_obj_t)nlr.ret_val;

    const char *type_name =
        mp_obj_get_type_str(exc);

    const char *msg = NULL;

    nlr_buf_t nlr2;

    if (nlr_push(&nlr2) == 0) {

        mp_obj_t args_arr[1];
        args_arr[0] = exc;

        mp_obj_t s =
            (mp_obj_t)mp_obj_str_make_new(
                &mp_type_str,
                1,
                0,
                args_arr
            );

        msg =
            mp_obj_str_get_str(s);

        nlr_pop();

    } else {

        msg = NULL;
    }

    JS_ThrowTypeError(
        qctx,
        "%s: %s",
        type_name,
        (msg != NULL)
            ? msg
            : "conversion error"
    );

    return JS_EXCEPTION;
}


/*
 * MP -> JS 转换的裸实现。必须经由 mp_to_quickjs 调用（见其说明）。
 */
static JSValue mp_to_quickjs_impl(
    JSContext *ctx,
    mp_obj_t obj,
    quickjs_convert_state_t *st
) {
    /* ---------------------------------------------------------------------- */
    /* None                                                                   */
    /* ---------------------------------------------------------------------- */

    if (obj == mp_const_none) {

        return JS_NULL;
    }


    /* ---------------------------------------------------------------------- */
    /* bool                                                                   */
    /* ---------------------------------------------------------------------- */

    if (mp_obj_is_bool(obj)) {

        bool value =
            mp_obj_is_true(obj);

        return JS_NewBool(
            ctx,
            value
        );
    }


    /* ---------------------------------------------------------------------- */
    /* int                                                                    */
    /* ---------------------------------------------------------------------- */

    if (mp_obj_is_int(obj)) {

        mp_int_t value =
            mp_obj_get_int(obj);

        /*
         * MicroPython int 在 ESP32 上
         * 通常可以安全地先作为 32-bit JS integer。
         */
        if (value >= INT32_MIN &&
            value <= INT32_MAX) {

            return JS_NewInt32(
                ctx,
                (int32_t)value
            );
        }


        /*
         * 超出 32-bit 时转成 double。
         *
         * 后续如果需要可以再实现
         * BigInt。
         */
        return JS_NewFloat64(
            ctx,
            (double)value
        );
    }


    /* ---------------------------------------------------------------------- */
    /* float                                                                  */
    /* ---------------------------------------------------------------------- */

    if (mp_obj_is_float(obj)) {

        mp_float_t value =
            mp_obj_get_float(obj);

        return JS_NewFloat64(
            ctx,
            (double)value
        );
    }


    /* ---------------------------------------------------------------------- */
    /* string                                                                 */
    /* ---------------------------------------------------------------------- */

    if (mp_obj_is_str(obj)) {

        return mp_str_to_quickjs(
            ctx,
            obj
        );
    }


    /* ---------------------------------------------------------------------- */
    /* list                                                                    */
    /* ---------------------------------------------------------------------- */

    if (mp_obj_is_type(
            obj,
            &mp_type_list
        )) {

        return mp_list_to_quickjs(
            ctx,
            obj,
            st
        );
    }


    /* ---------------------------------------------------------------------- */
    /* tuple                                                                   */
    /* ---------------------------------------------------------------------- */

    if (mp_obj_is_type(
            obj,
            &mp_type_tuple
        )) {

        return mp_tuple_to_quickjs(
            ctx,
            obj,
            st
        );
    }


    /* ---------------------------------------------------------------------- */
    /* dict                                                                    */
    /* ---------------------------------------------------------------------- */

    if (mp_obj_is_type(
            obj,
            &mp_type_dict
        )) {

        return mp_dict_to_quickjs(
            ctx,
            obj,
            st
        );
    }


    /* ---------------------------------------------------------------------- */
    /* bytes -> ArrayBuffer（copy 语义）                                        */
    /* ---------------------------------------------------------------------- */

    if (mp_obj_is_type(
            obj,
            &mp_type_bytes
        )) {

        mp_buffer_info_t bufinfo;

        if (!mp_get_buffer(
                obj,
                &bufinfo,
                MP_BUFFER_READ
            )) {

            return JS_ThrowTypeError(
                ctx,
                "cannot read bytes buffer"
            );
        }

        /*
         * copy：不借用 MicroPython buffer 指针，
         * Python bytes 生命周期可能早于 JSValue。
         */
        return JS_NewArrayBufferCopy(
            ctx,
            (const uint8_t *)bufinfo.buf,
            bufinfo.len
        );
    }


    /* ---------------------------------------------------------------------- */
    /* bytearray -> Uint8Array（copy 语义）                                     */
    /* ---------------------------------------------------------------------- */

    if (mp_obj_is_type(
            obj,
            &mp_type_bytearray
        )) {

        mp_buffer_info_t bufinfo;

        if (!mp_get_buffer(
                obj,
                &bufinfo,
                MP_BUFFER_READ
            )) {

            return JS_ThrowTypeError(
                ctx,
                "cannot read bytearray buffer"
            );
        }

        /*
         * copy：同上。
         */
        return JS_NewUint8ArrayCopy(
            ctx,
            (const uint8_t *)bufinfo.buf,
            bufinfo.len
        );
    }


    /* ---------------------------------------------------------------------- */
    /* JS Function wrapper -> JS（pass-through）                               */
    /* ---------------------------------------------------------------------- */

    /*
     * 阶段 4：识别 JS 函数 wrapper。同 Context 时 JS_DupValue 传回 JS；
     * 跨 Context / context closed 设 JS 异常并返回 JS_EXCEPTION。
     */
    if (mp_obj_is_type(
            obj,
            &quickjs_function_type
        )) {

        return quickjs_function_pass_through(
            ctx,
            obj
        );
    }


    /* ---------------------------------------------------------------------- */
    /* JS Promise wrapper -> JS（pass-through，阶段 5）                         */
    /* ---------------------------------------------------------------------- */

    /*
     * 阶段 5：识别 Promise wrapper。同 Context 时 JS_DupValue 传回 JS。
     * 用途：callback 返回 Promise wrapper -> promise assimilation；
     * ctx.set("x", p) 也允许把 promise 存回 JS。
     */
    if (mp_obj_is_type(
            obj,
            &quickjs_promise_type
        )) {

        return quickjs_promise_pass_through(
            ctx,
            obj
        );
    }


    /* ---------------------------------------------------------------------- */
    /* quickjs.bigint() 标记对象 -> JS BigInt（阶段 5）                         */
    /* ---------------------------------------------------------------------- */

    /*
     * 显式 BigInt 包装器：只有传入 QuickJS Context 时才变成 JS BigInt，
     * 不影响普通 Python int -> JS number 的既有行为。
     */
    if (mp_obj_is_type(
            obj,
            &quickjs_bigint_type
        )) {

        return quickjs_bigint_wrapper_to_js(
            ctx,
            obj
        );
    }


    /* ---------------------------------------------------------------------- */
    /* unsupported                                                             */
    /* ---------------------------------------------------------------------- */

    /*
     * 不在这里抛 Python 异常：mp_to_quickjs 可能处于
     * 参数/嵌套转换的中途，nlr 长跳会泄漏外层已创建的 JSValue。
     *
     * 改为设置 JS 异常并返回 JS_EXCEPTION，
     * 由最外层统一转换成 MicroPython 异常。
     */
    return JS_ThrowTypeError(
        ctx,
        "unsupported MicroPython type"
    );
}


/* ========================================================================= */
/*                         quickjs.init()                                     */
/* ========================================================================= */

static mp_obj_t mod_quickjs_init(void) {

    if (rt != NULL &&
        ctx != NULL) {

        return mp_const_none;
    }


    rt =
        JS_NewRuntime();

    if (rt == NULL) {

        mp_raise_msg(
            &mp_type_RuntimeError,
            MP_ERROR_TEXT(
                "failed to create JS Runtime"
            )
        );
    }


    /*
     * 默认 QuickJS 堆上限（可通过编译期宏覆盖，见文件头部说明）
     */
    JS_SetMemoryLimit(
        rt,
        QUICKJS_DEFAULT_MEMORY_LIMIT
    );


    ctx =
        JS_NewContext(rt);

    if (ctx == NULL) {

        JS_FreeRuntime(rt);

        rt = NULL;

        mp_raise_msg(
            &mp_type_RuntimeError,
            MP_ERROR_TEXT(
                "failed to create JS Context"
            )
        );
    }


    return mp_const_none;
}

static MP_DEFINE_CONST_FUN_OBJ_0(
    mod_quickjs_init_obj,
    mod_quickjs_init
);


/* ========================================================================= */
/*                         quickjs.version()                                  */
/* ========================================================================= */

static mp_obj_t mod_quickjs_version(void) {

    const char *ver =
        JS_GetVersion();

    if (ver == NULL) {
        return mp_const_none;
    }

    return mp_obj_new_str(
        ver,
        strlen(ver)
    );
}

static MP_DEFINE_CONST_FUN_OBJ_0(
    mod_quickjs_version_obj,
    mod_quickjs_version
);


/* ========================================================================= */
/*                         quickjs.eval()                                     */
/* ========================================================================= */

/*
 * 在指定 JSContext 上执行 JS 代码。
 *
 * 同时被默认 singleton 的 quickjs.eval() 和 Context.eval() 复用。
 */
static mp_obj_t quickjs_eval_helper(
    quickjs_ctx_t *state,
    const char *js_code
) {
    JSContext *qctx =
        (state != NULL)
            ? state->ctx
            : ctx;

    quickjs_convert_state_t st;
    memset(&st, 0, sizeof(st));


    quickjs_ctx_arm_timeout(state);

    JSValue val =
        JS_Eval(
            qctx,
            js_code,
            strlen(js_code),
            "<eval>",
            JS_EVAL_TYPE_GLOBAL
        );

    bool timed_out =
        quickjs_ctx_finish_timeout(state);


    if (JS_IsException(val)) {

        quickjs_raise_exception_state(
            qctx,
            val,
            timed_out
        );

        return mp_const_none;
    }


    /*
     * 转换过程中抛出 MicroPython 异常时，
     * quickjs_to_mp_owned 负责释放 val。
     */
    mp_obj_t result =
        quickjs_to_mp_owned(
            qctx,
            val,
            val,
            &st
        );


    JS_FreeValue(
        qctx,
        val
    );


    return result;
}


static mp_obj_t mod_quickjs_eval(
    mp_obj_t js_code_obj
) {
    if (ctx == NULL) {
        mod_quickjs_init();
    }


    const char *js_code =
        mp_obj_str_get_str(
            js_code_obj
        );


    return quickjs_eval_helper(
        NULL,
        js_code
    );
}

static MP_DEFINE_CONST_FUN_OBJ_1(
    mod_quickjs_eval_obj,
    mod_quickjs_eval
);


/* ========================================================================= */
/*                         quickjs.call()                                     */
/* ========================================================================= */

/*
 * Python:
 *
 *     quickjs.call("add", 1, 2)
 *
 * 等价于 JS：
 *
 *     add(1, 2)
 *
 * 同时被默认 singleton 的 quickjs.call() 和 Context.call() 复用。
 */

/* -------------------------------------------------------------------------- */
/* JS Call 核心（给定 JSValue 函数）                                            */
/* -------------------------------------------------------------------------- */

/*
 * 调用一个已持有的 JS 函数值，参数为 MicroPython 对象。
 *
 * 复用处：
 *   - Context.call() / quickjs.call()（先按名取函数再交给本函数）
 *   - Function wrapper 的 __call__（JS Function -> Python callable）
 *
 * state 可空：NULL 表示默认 singleton（无 timeout）。
 * func 是借用引用（调用方持有），本函数不释放。
 * this_val 是 owned 引用：本函数在成功与失败路径上都负责释放
 * （JS_UNDEFINED 的 JS_FreeValue 是 no-op，兼容旧调用方）。
 * 返回值：mp_obj_t 直接返回（JS 结果已转换并释放）。
 */
static mp_obj_t quickjs_call_value_helper_this(
    quickjs_ctx_t *state,
    JSValueConst func,
    JSValue this_val,
    size_t argc,
    const mp_obj_t *mp_args
) {
    JSContext *qctx =
        (state != NULL)
            ? state->ctx
            : ctx;

    quickjs_convert_state_t st;
    memset(&st, 0, sizeof(st));


    int n = (int)argc;


    JSValue *argv = NULL;

    if (n > 0) {

        /*
         * 阶段 10（OOM 审计修复）：argv（m_new 分配）失败时 this_val
         * 仍是本函数拥有的 JSValue，必须释放后再重抛，否则 this_val
         * 引用跨过 JSRuntime 生命周期泄漏（DEBUG assert / LSan 可观测）。
         */
        nlr_buf_t nlr_argv;

        if (nlr_push(&nlr_argv) == 0) {

            argv =
                m_new(
                    JSValue,
                    n
                );

            nlr_pop();

        } else {

            JS_FreeValue(
                qctx,
                this_val
            );

            nlr_raise(nlr_argv.ret_val);
        }
    }


    /*
     * MicroPython -> JS
     */
    for (int i = 0; i < n; i++) {

        argv[i] =
            mp_to_quickjs(
                qctx,
                mp_args[i],
                &st
            );


        if (JS_IsException(argv[i])) {

            /*
             * 释放已经创建/转换的参数。
             */
            for (int j = 0;
                 j < i;
                 j++) {

                JS_FreeValue(
                    qctx,
                    argv[j]
                );
            }


            if (argv != NULL) {

                m_del(
                    JSValue,
                    argv,
                    n
                );
            }

            /* 释放 this_val（owned）后再抛异常 */
            JS_FreeValue(
                qctx,
                this_val
            );

            quickjs_raise_exception(
                qctx,
                argv[i]
            );

            return mp_const_none;
        }
    }


    quickjs_ctx_arm_timeout(state);

    /*
     * JS Function call
     */
    JSValue result =
        JS_Call(
            qctx,
            func,
            this_val,
            n,
            argv
        );

    bool timed_out =
        quickjs_ctx_finish_timeout(state);


    /*
     * 参数所有权：JS_Call 不会替我们释放 argv 中的 JSValue。
     */
    for (int i = 0; i < n; i++) {

        JS_FreeValue(
            qctx,
            argv[i]
        );
    }


    if (argv != NULL) {

        m_del(
            JSValue,
            argv,
            n
        );
    }

    /* 释放 this_val（owned）——结果转换不再需要它 */
    JS_FreeValue(
        qctx,
        this_val
    );


    if (JS_IsException(result)) {

        quickjs_raise_exception_state(
            qctx,
            result,
            timed_out
        );

        return mp_const_none;
    }


    /*
     * JS -> MicroPython
     */
    mp_obj_t mp_result =
        quickjs_to_mp_owned(
            qctx,
            result,
            result,
            &st
        );


    JS_FreeValue(
        qctx,
        result
    );


    return mp_result;
}


/*
 * 保持旧签名：以 JS_UNDEFINED 作为 this 调用。
 * JS_UNDEFINED 是常量，JS_FreeValue 是 no-op，不改变所有权语义。
 */
static mp_obj_t quickjs_call_value_helper(
    quickjs_ctx_t *state,
    JSValueConst func,
    size_t argc,
    const mp_obj_t *mp_args
) {
    return quickjs_call_value_helper_this(
        state,
        func,
        JS_UNDEFINED,
        argc,
        mp_args
    );
}


/* -------------------------------------------------------------------------- */
/* Function wrapper __call__                                                   */
/* -------------------------------------------------------------------------- */

/*
 * wrapper(*args)：this = JS_UNDEFINED（既有行为不变）。
 * 阶段 5：包在重入保护窗口内（执行期间可能回调 Python）。
 */
static mp_obj_t mod_quickjs_function_call(
    mp_obj_t self_in,
    size_t n_args,
    size_t n_kw,
    const mp_obj_t *args
) {
    mp_obj_quickjs_function_t *f =
        MP_OBJ_TO_PTR(self_in);

    quickjs_ctx_t *state =
        f->state;

    if (n_kw > 0) {

        mp_raise_msg(
            &mp_type_TypeError,
            MP_ERROR_TEXT(
                "QuickJS function does not support keyword arguments"
            )
        );
    }

    if (state == NULL ||
        state->closed ||
        state->ctx == NULL ||
        state->rt == NULL) {

        mp_raise_msg(
            &mp_type_RuntimeError,
            MP_ERROR_TEXT(
                "context closed"
            )
        );
    }

    quickjs_ctx_enter(state);

    nlr_buf_t nlr;

    if (nlr_push(&nlr) == 0) {

        JSValue fval =
            quickjs_function_lookup(f);

        mp_obj_t result =
            quickjs_call_value_helper(
                state,
                fval,
                n_args,
                args
            );

        nlr_pop();

        quickjs_ctx_leave(state);

        return result;

    } else {

        /* 异常路径：必须 leave，否则 depth 卡死，close() 永久被拒 */
        quickjs_ctx_leave(state);

        nlr_raise(nlr.ret_val);
    }

    return mp_const_none; /* unreachable */
}


/* -------------------------------------------------------------------------- */
/* Function wrapper .call(this_obj, *args) —— 阶段 5：this 绑定                 */
/* -------------------------------------------------------------------------- */

/*
 * Python:
 *
 *     wrapper.call(this_obj, *args)
 *
 * 把 this_obj 用当前 Context 的转换规则转成 JS 值（dict -> Object、
 * Function/Promise wrapper -> pass-through、跨 Context -> 拒绝），
 * 然后以它为 this 调用 JS 函数。
 *
 * 默认的 wrapper(*args)（this = undefined）行为不受影响。
 */
static mp_obj_t mod_quickjs_function_call_this(
    size_t n_args,
    const mp_obj_t *args
) {
    mp_obj_quickjs_function_t *f =
        MP_OBJ_TO_PTR(args[0]);

    quickjs_ctx_t *state =
        f->state;

    if (state == NULL ||
        state->closed ||
        state->ctx == NULL ||
        state->rt == NULL) {

        mp_raise_msg(
            &mp_type_RuntimeError,
            MP_ERROR_TEXT(
                "context closed"
            )
        );
    }

    quickjs_ctx_enter(state);

    nlr_buf_t nlr;

    if (nlr_push(&nlr) == 0) {

        quickjs_convert_state_t st;
        memset(&st, 0, sizeof(st));

        JSValue this_js =
            mp_to_quickjs(
                state->ctx,
                args[1],
                &st
            );

        if (JS_IsException(this_js)) {

            /*
             * quickjs_raise_exception 负责释放 this_js 哨兵并抛出。
             * 该异常被外层 nlr 捕获 -> leave -> 重抛。
             */
            quickjs_raise_exception(
                state->ctx,
                this_js
            );
        }

        mp_obj_t result =
            quickjs_call_value_helper_this(
                state,
                quickjs_function_lookup(f),
                this_js,        /* owned：helper 释放 */
                n_args - 2,
                args + 2
            );

        nlr_pop();

        quickjs_ctx_leave(state);

        return result;

    } else {

        quickjs_ctx_leave(state);

        nlr_raise(nlr.ret_val);
    }

    return mp_const_none; /* unreachable */
}

/*
 * wrapper.call(this_obj, *args)：至少 2 个参数（self + this_obj），
 * 之后的参数全部传给 JS 函数。
 */
MP_DEFINE_CONST_FUN_OBJ_VAR(
    mod_quickjs_function_call_this_obj,
    2,
    mod_quickjs_function_call_this
);


static mp_obj_t quickjs_call_helper(
    quickjs_ctx_t *state,
    const char *function_name,
    size_t argc,
    const mp_obj_t *mp_args
) {
    JSContext *qctx =
        (state != NULL)
            ? state->ctx
            : ctx;


    /*
     * 从 global object 获取函数
     */
    JSValue global =
        JS_GetGlobalObject(qctx);


    JSValue func =
        JS_GetPropertyStr(
            qctx,
            global,
            function_name
        );


    JS_FreeValue(
        qctx,
        global
    );


    if (JS_IsException(func)) {

        quickjs_raise_exception(
            qctx,
            func
        );

        return mp_const_none;
    }


    /*
     * 确认是 callable
     */
    if (!JS_IsFunction(
            qctx,
            func
        )) {

        JS_FreeValue(
            qctx,
            func
        );

        mp_raise_msg(
            &mp_type_TypeError,
            MP_ERROR_TEXT(
                "JS value is not callable"
            )
        );
    }


    /*
     * 参数转换 + JS_Call + 结果转换全部复用
     * quickjs_call_value_helper（不复制第二套转换逻辑）。
     * func 所有权由本函数负责释放。
     *
     * 注意：quickjs_call_value_helper 内部会因参数转换错误等原因
     * 经 quickjs_raise_exception 抛 MP 异常（nlr 长跳转），必须在
     * 自己的一层 nlr 里保证 JS_FreeValue(func) 在成功/失败路径都执行，
     * 否则 func 的引用跨过 JS_FreeRuntime -> DEBUG assert / 泄漏。
     */
    nlr_buf_t nlr2;

    if (nlr_push(&nlr2) == 0) {

        mp_obj_t mp_result =
            quickjs_call_value_helper(
                state,
                func,
                argc,
                mp_args
            );

        nlr_pop();

        JS_FreeValue(
            qctx,
            func
        );

        return mp_result;

    } else {

        JS_FreeValue(
            qctx,
            func
        );

        nlr_raise(nlr2.ret_val);
    }

    return mp_const_none; /* unreachable */
}


static mp_obj_t mod_quickjs_call(
    size_t n_args,
    const mp_obj_t *args
) {
    if (ctx == NULL) {
        mod_quickjs_init();
    }


    /*
     * 第一个参数：function name
     */
    const char *function_name =
        mp_obj_str_get_str(
            args[0]
        );


    return quickjs_call_helper(
        NULL,
        function_name,
        n_args - 1,
        args + 1
    );
}

static MP_DEFINE_CONST_FUN_OBJ_VAR(
    mod_quickjs_call_obj,
    1,
    mod_quickjs_call
);


/* ========================================================================= */
/*                         quickjs.run_jobs() / has_pending_jobs()            */
/* ========================================================================= */

/*
 * 默认 singleton 的 job queue 泵（阶段 4）。
 * 与 Context 的 run_jobs() 共享同一实现；state=NULL 表示 singleton。
 * 保持 quickjs.init() 自动初始化语义。
 */
static mp_obj_t mod_quickjs_run_jobs(void) {

    if (rt == NULL ||
        ctx == NULL) {

        mod_quickjs_init();
    }

    return quickjs_run_jobs_helper(
        NULL
    );
}

static MP_DEFINE_CONST_FUN_OBJ_0(
    mod_quickjs_run_jobs_obj,
    mod_quickjs_run_jobs
);


static mp_obj_t mod_quickjs_has_pending_jobs(void) {

    if (rt == NULL ||
        ctx == NULL) {

        mod_quickjs_init();
    }

    return mp_obj_new_bool(
        JS_IsJobPending(rt)
    );
}

static MP_DEFINE_CONST_FUN_OBJ_0(
    mod_quickjs_has_pending_jobs_obj,
    mod_quickjs_has_pending_jobs
);


/* ========================================================================= */
/*                         quickjs.Context()                                  */
/* ========================================================================= */

/* -------------------------------------------------------------------------- */
/* Context 创建                                                               */
/* -------------------------------------------------------------------------- */

static mp_obj_t quickjs_context_make_new(
    const mp_obj_type_t *type,
    size_t n_args,
    size_t n_kw,
    const mp_obj_t *args
) {
    mp_arg_check_num(
        n_args,
        n_kw,
        0,
        0,
        false
    );


    JSRuntime *rt =
        JS_NewRuntime();

    if (rt == NULL) {

        mp_raise_msg(
            &mp_type_RuntimeError,
            MP_ERROR_TEXT(
                "failed to create JS Runtime"
            )
        );
    }


    JS_SetMemoryLimit(
        rt,
        QUICKJS_DEFAULT_MEMORY_LIMIT
    );


    JSContext *qctx =
        JS_NewContext(rt);

    if (qctx == NULL) {

        JS_FreeRuntime(rt);

        mp_raise_msg(
            &mp_type_RuntimeError,
            MP_ERROR_TEXT(
                "failed to create JS Context"
            )
        );
    }


    quickjs_ctx_t *state = NULL;

    mp_obj_t self =
        MP_OBJ_NULL;


    /*
     * 后续 m_new_obj / mp_obj_malloc_with_finaliser 可能
     * 因为内存不足抛 MicroPython 异常。用 nlr 保护：
     * 一旦失败，先释放已创建的 JS 资源再重新抛出，
     * 保证失败路径不泄漏 JSRuntime/JSContext。
     */
    nlr_buf_t nlr;

    if (nlr_push(&nlr) == 0) {

        state =
            m_new_obj(
                quickjs_ctx_t
            );

        memset(
            state,
            0,
            sizeof(quickjs_ctx_t)
        );


        mp_obj_quickjs_context_t *obj =
            mp_obj_malloc_with_finaliser(
                mp_obj_quickjs_context_t,
                type
            );

        obj->state = state;


        state->rt = rt;
        state->ctx = qctx;
        state->closed = false;
        state->self_obj =
            MP_OBJ_FROM_PTR(obj);

        /*
         * 阶段 3：把 state 关联到 JSContext 的 opaque，
         * 转换层（quickjs_function_to_mp 等）可从 JSContext
         * 找回 state，无需改所有转换函数签名。
         */
        JS_SetContextOpaque(
            qctx,
            state
        );

        /*
         * 阶段 3：安装 interrupt handler（timeout）。
         * handler 常驻；未启用 timeout 时立即返回 0，开销极小。
         */
        JS_SetInterruptHandler(
            rt,
            quickjs_interrupt_handler,
            state
        );

        self =
            MP_OBJ_FROM_PTR(obj);

        nlr_pop();

    } else {

        /*
         * 分配失败：释放 JS 资源。
         * state 若已分配但未被引用，会被 GC 回收。
         */
        JS_FreeContext(qctx);
        JS_FreeRuntime(rt);

        nlr_raise(nlr.ret_val);
    }


    return self;
}


/* -------------------------------------------------------------------------- */
/* Context 状态检查                                                            */
/* -------------------------------------------------------------------------- */

/*
 * 返回打开的 state，若 Context 已关闭则抛出异常。
 * 所有需要 JS 上下文的 Context 方法先调用本函数。
 */
static quickjs_ctx_t *quickjs_ctx_check_open(
    mp_obj_t self_in
) {
    mp_obj_quickjs_context_t *self =
        MP_OBJ_TO_PTR(self_in);

    quickjs_ctx_t *state =
        self->state;

    if (state == NULL ||
        state->closed ||
        state->ctx == NULL ||
        state->rt == NULL) {

        mp_raise_msg(
            &mp_type_RuntimeError,
            MP_ERROR_TEXT(
                "context closed"
            )
        );
    }

    return state;
}


/* -------------------------------------------------------------------------- */
/* Context.close()                                                            */
/* -------------------------------------------------------------------------- */

/*
 * 释放 JSContext 和 JSRuntime。
 *
 * - 幂等：重复 close 是 no-op
 * - 也是 __del__ finaliser 的目标：GC 回收 Context 时兜底释放
 * - close 后不释放 state（state 由 GC 管理，随 Context 对象回收）
 * - 必须先 JS_FreeContext 再 JS_FreeRuntime
 *   （JS_FreeContext 会把自身从 runtime 的 context_list 摘除）
 *
 * 阶段 5（重入保护）：
 * - executing_depth > 0（正在执行 JS，可能从 Python callback 里调用
 *   close）时拒绝关闭并抛 RuntimeError("context is busy")——绝不关闭
 *   一个仍在执行的 JSRuntime，否则 QuickJS C 栈会在 close 后继续访问
 *   已释放的 runtime/context。
 * - 检查在任何变更之前：close() 失败时 Context 保持完全可用。
 */
static mp_obj_t mod_quickjs_ctx_close(
    mp_obj_t self_in
) {
    mp_obj_quickjs_context_t *self =
        MP_OBJ_TO_PTR(self_in);

    quickjs_ctx_t *state =
        self->state;

    if (state == NULL ||
        state->closed) {

        return mp_const_none;
    }

    if (state->executing_depth > 0) {

        mp_raise_msg(
            &mp_type_RuntimeError,
            MP_ERROR_TEXT(
                "context is busy"
            )
        );
    }


    state->closed = true;


    /*
     * 阶段 3：释放回调注册表的 JSValue 引用（ctx 仍有效时）。
     * 闭包在 JS_FreeContext 里 finalize 时会触碰其 opaque 节点，
     * 因此节点必须先保持存活、再释放（见下方 m_del 循环）。
     *
     * 阶段 9：先解除 rejection tracker 注册（rt 仍有效时）。
     * 源码确认 tracker 不会在 close/free 期间被调用，这里解除是
     * 纵深防御 + 语义清晰：close 后该 runtime 不再产生诊断事件。
     */
    if (state->rt != NULL) {

        JS_SetHostPromiseRejectionTracker(
            state->rt,
            NULL,
            NULL
        );
    }

    if (state->ctx != NULL) {

        /*
         * 阶段 5：先释放所有 wrapper 的 JS 值条目（否则跨过
         * JS_FreeRuntime 内部 GC 的外部 dup 引用会触发 assert /
         * 泄漏，见 quickjs_ctx_release_entries 注释）。
         */
        quickjs_ctx_release_entries(state);

        quickjs_callback_t *cb =
            state->callbacks;

        while (cb != NULL) {

            JS_FreeValue(
                state->ctx,
                cb->js_func
            );

            cb->js_func = JS_UNDEFINED;

            cb = cb->next;
        }
    }


    if (state->ctx != NULL) {

        JS_FreeContext(
            state->ctx
        );

        state->ctx = NULL;
    }


    if (state->rt != NULL) {

        JS_FreeRuntime(
            state->rt
        );

        state->rt = NULL;
    }


    /*
     * 阶段 3：释放回调节点（此刻所有闭包已 finalize，
     * opaque_finalize 不会再触碰节点）。
     * 同时解除对 Python callable / name 的引用。
     */
    quickjs_callback_t *cb =
        state->callbacks;

    while (cb != NULL) {

        quickjs_callback_t *next =
            cb->next;

        quickjs_callback_free_node(cb);

        cb = next;
    }

    state->callbacks = NULL;


    /*
     * 阶段 9：释放 rejection handler 节点（此时 runtime 已释放、
     * tracker 已解除注册，不会再有任何回调进入）。
     */
    if (state->rejection_handler != NULL) {

        quickjs_rejection_handler_t *rh =
            state->rejection_handler;

        state->rejection_handler = NULL;

        rh->state = NULL;
        rh->callable = MP_OBJ_NULL;
        rh->last_error = MP_OBJ_NULL;

        m_del(
            quickjs_rejection_handler_t,
            rh,
            1
        );
    }


    return mp_const_none;
}


/* -------------------------------------------------------------------------- */
/* Context.eval()                                                             */
/* -------------------------------------------------------------------------- */

static mp_obj_t mod_quickjs_ctx_eval(
    mp_obj_t self_in,
    mp_obj_t js_code_obj
) {
    quickjs_ctx_t *state =
        quickjs_ctx_check_open(self_in);

    /*
     * 阶段 5：重入保护窗口。JS_Eval 及其结果转换都可能触发 JS getter
     * -> 回调 Python -> 可能调用 ctx.close()，必须在窗口内。
     */
    quickjs_ctx_enter(state);

    nlr_buf_t nlr;

    if (nlr_push(&nlr) == 0) {

        const char *js_code =
            mp_obj_str_get_str(
                js_code_obj
            );

        mp_obj_t result =
            quickjs_eval_helper(
                state,
                js_code
            );

        nlr_pop();

        quickjs_ctx_leave(state);

        return result;

    } else {

        quickjs_ctx_leave(state);

        nlr_raise(nlr.ret_val);
    }

    return mp_const_none; /* unreachable */
}

static MP_DEFINE_CONST_FUN_OBJ_2(
    mod_quickjs_ctx_eval_obj,
    mod_quickjs_ctx_eval
);


/* -------------------------------------------------------------------------- */
/* Context.call()                                                             */
/* -------------------------------------------------------------------------- */

/*
 * Python:
 *
 *     ctx.call("add", 1, 2)
 *
 * args[0] 是 self，args[1] 是函数名。
 */
static mp_obj_t mod_quickjs_ctx_call(
    size_t n_args,
    const mp_obj_t *args
) {
    quickjs_ctx_t *state =
        quickjs_ctx_check_open(args[0]);

    quickjs_ctx_enter(state);

    nlr_buf_t nlr;

    if (nlr_push(&nlr) == 0) {

        const char *function_name =
            mp_obj_str_get_str(
                args[1]
            );

        mp_obj_t result =
            quickjs_call_helper(
                state,
                function_name,
                n_args - 2,
                args + 2
            );

        nlr_pop();

        quickjs_ctx_leave(state);

        return result;

    } else {

        quickjs_ctx_leave(state);

        nlr_raise(nlr.ret_val);
    }

    return mp_const_none; /* unreachable */
}

static MP_DEFINE_CONST_FUN_OBJ_VAR(
    mod_quickjs_ctx_call_obj,
    2,
    mod_quickjs_ctx_call
);


/* -------------------------------------------------------------------------- */
/* Context.get()                                                              */
/* -------------------------------------------------------------------------- */

/*
 * 从 JS global object 读取变量。
 *
 * 变量不存在（undefined）或为 null 时返回 None。
 */
/*
 * ctx.get(name) 的实现。state 已通过 open 检查。
 * JS_GetPropertyStr + 结果转换可能触发 JS getter -> 回调 Python，
 * 因此外层必须包重入窗口。
 */
static mp_obj_t mod_quickjs_ctx_get_impl(
    quickjs_ctx_t *state,
    mp_obj_t name_obj
) {
    quickjs_convert_state_t st;
    memset(&st, 0, sizeof(st));


    const char *name =
        mp_obj_str_get_str(
            name_obj
        );


    JSValue global =
        JS_GetGlobalObject(
            state->ctx
        );


    JSValue val =
        JS_GetPropertyStr(
            state->ctx,
            global,
            name
        );


    JS_FreeValue(
        state->ctx,
        global
    );


    if (JS_IsException(val)) {

        quickjs_raise_exception(
            state->ctx,
            val
        );

        return mp_const_none;
    }


    /* 缺失（undefined）或 null -> None */
    if (JS_IsUndefined(val) ||
        JS_IsNull(val)) {

        JS_FreeValue(
            state->ctx,
            val
        );

        return mp_const_none;
    }


    mp_obj_t result =
        quickjs_to_mp_owned(
            state->ctx,
            val,
            val,
            &st
        );


    JS_FreeValue(
        state->ctx,
        val
    );


    return result;
}


static mp_obj_t mod_quickjs_ctx_get(
    mp_obj_t self_in,
    mp_obj_t name_obj
) {
    quickjs_ctx_t *state =
        quickjs_ctx_check_open(self_in);

    quickjs_ctx_enter(state);

    nlr_buf_t nlr;

    if (nlr_push(&nlr) == 0) {

        mp_obj_t result =
            mod_quickjs_ctx_get_impl(
                state,
                name_obj
            );

        nlr_pop();

        quickjs_ctx_leave(state);

        return result;

    } else {

        quickjs_ctx_leave(state);

        nlr_raise(nlr.ret_val);
    }

    return mp_const_none; /* unreachable */
}

static MP_DEFINE_CONST_FUN_OBJ_2(
    mod_quickjs_ctx_get_obj,
    mod_quickjs_ctx_get
);


/* -------------------------------------------------------------------------- */
/* Context.set()                                                              */
/* -------------------------------------------------------------------------- */

/*
 * ctx.set(name, value) 的实现。state 已通过 open 检查。
 * mp_to_quickjs（可能触发 setter/proxy）+ JS_SetPropertyStr
 * 都可能回调 Python，外层必须包重入窗口。
 */
static mp_obj_t mod_quickjs_ctx_set_impl(
    quickjs_ctx_t *state,
    mp_obj_t name_obj,
    mp_obj_t value_obj
) {
    quickjs_convert_state_t st;
    memset(&st, 0, sizeof(st));


    const char *name =
        mp_obj_str_get_str(
            name_obj
        );


    JSValue global =
        JS_GetGlobalObject(
            state->ctx
        );


    JSValue value =
        mp_to_quickjs(
            state->ctx,
            value_obj,
            &st
        );


    if (JS_IsException(value)) {

        JS_FreeValue(
            state->ctx,
            global
        );

        quickjs_raise_exception(
            state->ctx,
            value
        );

        return mp_const_none;
    }


    if (JS_SetPropertyStr(
            state->ctx,
            global,
            name,
            value
        ) < 0) {

        /*
         * JS_SetPropertyStr() 成功/失败均接管 value 所有权。
         */
        JS_FreeValue(
            state->ctx,
            global
        );

        quickjs_raise_exception(
            state->ctx,
            JS_UNDEFINED
        );

        return mp_const_none;
    }


    JS_FreeValue(
        state->ctx,
        global
    );


    return mp_const_none;
}


static mp_obj_t mod_quickjs_ctx_set(
    mp_obj_t self_in,
    mp_obj_t name_obj,
    mp_obj_t value_obj
) {
    quickjs_ctx_t *state =
        quickjs_ctx_check_open(self_in);

    quickjs_ctx_enter(state);

    nlr_buf_t nlr;

    if (nlr_push(&nlr) == 0) {

        mp_obj_t result =
            mod_quickjs_ctx_set_impl(
                state,
                name_obj,
                value_obj
            );

        nlr_pop();

        quickjs_ctx_leave(state);

        return result;

    } else {

        quickjs_ctx_leave(state);

        nlr_raise(nlr.ret_val);
    }

    return mp_const_none; /* unreachable */
}

static MP_DEFINE_CONST_FUN_OBJ_3(
    mod_quickjs_ctx_set_obj,
    mod_quickjs_ctx_set
);


/* -------------------------------------------------------------------------- */
/* Context.promise() —— Python 侧显式 Promise 创建（阶段 7）                     */
/* -------------------------------------------------------------------------- */

/*
 * Python:
 *
 *     p, resolve, reject = ctx.promise()
 *
 * 创建自定 Promise 并返回三个对象：
 *   - p        : Promise wrapper（与 eval/get 返回的 wrapper 完全一致，
 *                done/result/then/catch/finally_ 全部可用）
 *   - resolve  : Python callable；resolve(v) 把 promise 定局为 fulfilled
 *   - reject   : Python callable；reject(r) 把 promise 定局为 rejected
 *
 * 实现：QuickJS 原生 JS_NewPromiseCapability（不 consult
 * Promise.prototype / Symbol.species）。
 *
 * Ownership（对照 quickjs.c js_promise_new / js_create_resolving_functions）：
 *   - promise            : 1 个新引用 —— dup 进 promise_entries
 *     （quickjs_promise_to_mp 内部 dup；本函数用完原始引用后释放）；
 *   - resolving_funcs[0] : 1 个新引用 —— dup 进 resolver_entries
 *     （resolve wrapper 持有 token）；
 *   - resolving_funcs[1] : 1 个新引用 —— dup 进 resolver_entries
 *     （reject wrapper 持有 token）；
 *   - 失败时 resolving_funcs 均为 JS_UNDEFINED（无引用，free 是 no-op）。
 *
 * 状态机不在这里复制：resolve/reject 是 JS 原生 resolving function，
 * 首次调用定局、重复调用忽略、promise/thenable assimilation 全由
 * QuickJS 负责（见 quickjs.c js_promise_resolve_function_call：
 * 非对象值同步定局；对象取 then——函数则入 thenable job——非函数定局）。
 *
 * 窗口：JS_NewPromiseCapability 可能分配失败抛 JS 异常（OOM），
 * wrapper 创建可能抛 MP 异常（内存不足），统一包 nlr；三份原生引用
 * 在两条失败路径上都释放，成功路径 wrapper 创建完成后释放。
 */
static mp_obj_t mod_quickjs_ctx_promise(
    mp_obj_t self_in
) {
    quickjs_ctx_t *state =
        quickjs_ctx_check_open(self_in);

    quickjs_ctx_enter(state);

    nlr_buf_t nlr;

    if (nlr_push(&nlr) == 0) {

        JSContext *qctx =
            state->ctx;

        quickjs_ctx_arm_timeout(state);

        /*
         * 阶段 10（OOM 审计修复）：必须显式初始化。
         *
         * js_promise_new()（quickjs.c:55919）在更早的失败点
         * （js_create_from_ctor 的 js_malloc、JSPromiseData 的
         * js_mallocz）直接返回 JS_EXCEPTION，根本不写 resolving_funcs
         * （只有 js_create_resolving_functions 失败时才会把两个槽写成
         * JS_UNDEFINED）。若不初始化，下面 JS_IsException(promise) 的
         * 失败路径会对未初始化的栈值执行 JS_FreeValue —— 未初始化读取
         * + 释放随机垃圾的 UB。
         */
        JSValue resolving_funcs[2] = {
            JS_UNDEFINED,
            JS_UNDEFINED
        };

        JSValue promise =
            JS_NewPromiseCapability(
                qctx,
                resolving_funcs
            );

        bool timed_out =
            quickjs_ctx_finish_timeout(state);

        if (JS_IsException(promise)) {

            /*
             * js_promise_new 失败时 resolving_funcs[0/1] 已置为
             * JS_UNDEFINED（JS_FreeValue 无操作），仍显式释放，
             * 保持“三份引用要么都释放要么都移交”的不变量。
             */
            JS_FreeValue(
                qctx,
                resolving_funcs[0]
            );

            JS_FreeValue(
                qctx,
                resolving_funcs[1]
            );

            quickjs_raise_exception_state(
                qctx,
                promise,
                timed_out
            );
        }

        /*
         * 创建三个 wrapper。MP 分配失败（nlr）时释放三份原生引用
         * 再重抛，保证失败路径不泄漏 JSValue。
         */
        nlr_buf_t nlr2;

        if (nlr_push(&nlr2) == 0) {

            mp_obj_t pw =
                quickjs_promise_to_mp(
                    qctx,
                    promise
                );

            mp_obj_t rw =
                quickjs_resolver_to_mp(
                    qctx,
                    resolving_funcs[0],
                    false
                );

            mp_obj_t jw =
                quickjs_resolver_to_mp(
                    qctx,
                    resolving_funcs[1],
                    true
                );

            /* wrapper 已各自 dup 进条目表，释放原始引用 */
            JS_FreeValue(
                qctx,
                promise
            );

            JS_FreeValue(
                qctx,
                resolving_funcs[0]
            );

            JS_FreeValue(
                qctx,
                resolving_funcs[1]
            );

            mp_obj_t items[3];
            items[0] = pw;
            items[1] = rw;
            items[2] = jw;

            mp_obj_t result =
                mp_obj_new_tuple(
                    3,
                    items
                );

            /* 成功路径：两个 nlr 窗口都必须 pop（遗漏会留下悬垂的
             * nlr_top，后续任何 nlr_raise 都会经残留链跳到已回收的
             * 栈帧 -> 野跳转/内存破坏；阶段 6 审计的同类问题）。 */
            nlr_pop();      /* pop 内层窗口 */
            nlr_pop();      /* pop 外层窗口 */

            quickjs_ctx_leave(state);

            return result;

        } else {

            JS_FreeValue(
                qctx,
                promise
            );

            JS_FreeValue(
                qctx,
                resolving_funcs[0]
            );

            JS_FreeValue(
                qctx,
                resolving_funcs[1]
            );

            nlr_raise(nlr2.ret_val);
        }

    } else {

        quickjs_ctx_leave(state);

        nlr_raise(nlr.ret_val);
    }

    return mp_const_none; /* unreachable */
}

static MP_DEFINE_CONST_FUN_OBJ_1(
    mod_quickjs_ctx_promise_obj,
    mod_quickjs_ctx_promise
);


/* -------------------------------------------------------------------------- */
/* Context.gc()                                                               */
/* -------------------------------------------------------------------------- */

static mp_obj_t mod_quickjs_ctx_gc(
    mp_obj_t self_in
) {
    quickjs_ctx_t *state =
        quickjs_ctx_check_open(self_in);


    JS_RunGC(
        state->rt
    );


    return mp_const_none;
}

static MP_DEFINE_CONST_FUN_OBJ_1(
    mod_quickjs_ctx_gc_obj,
    mod_quickjs_ctx_gc
);


/* -------------------------------------------------------------------------- */
/* Context._js_mem()（调试：QuickJS 堆用量）                                    */
/* -------------------------------------------------------------------------- */

/*
 * 返回当前 runtime 的 QuickJS 堆用量（JS_ComputeMemoryUsage 的
 * memory_used_size，字节）。调试用途：验证无 JS 堆泄漏时用
 * “创建 N 个 context 后新 context 的堆用量与基线差 0” 来判断。
 */
static mp_obj_t mod_quickjs_ctx_js_mem(
    mp_obj_t self_in
) {
    quickjs_ctx_t *state =
        quickjs_ctx_check_open(self_in);

    JSMemoryUsage usage;
    memset(&usage, 0, sizeof(usage));

    JS_ComputeMemoryUsage(
        state->rt,
        &usage
    );

    return mp_obj_new_int_from_ll(
        (long long)usage.memory_used_size
    );
}

static MP_DEFINE_CONST_FUN_OBJ_1(
    mod_quickjs_ctx_js_mem_obj,
    mod_quickjs_ctx_js_mem
);


/* -------------------------------------------------------------------------- */
/* Context.run_jobs() / has_pending_jobs()                                    */
/* -------------------------------------------------------------------------- */

/*
 * Python:
 *
 *     n = ctx.run_jobs()
 *
 * 泵出当前 Runtime 的 pending jobs（Promise microtasks / .then / async）。
 * 返回执行的 job 数量；无 job 返回 0。job 执行错误（含超时中断）转成
 * Python 异常。
 *
 *     ctx.has_pending_jobs() -> bool
 *
 * 是否有尚未执行的 job（JS_IsJobPending）。
 */
static mp_obj_t mod_quickjs_ctx_run_jobs(
    mp_obj_t self_in
) {
    quickjs_ctx_t *state =
        quickjs_ctx_check_open(self_in);

    /*
     * 阶段 5：重入窗口。job 执行会运行 microtask，可能调用 Python
     * callback -> 可能调用 ctx.close()，必须在窗口内。
     */
    quickjs_ctx_enter(state);

    nlr_buf_t nlr;

    if (nlr_push(&nlr) == 0) {

        mp_obj_t result =
            quickjs_run_jobs_helper(
                state
            );

        nlr_pop();

        quickjs_ctx_leave(state);

        return result;

    } else {

        quickjs_ctx_leave(state);

        nlr_raise(nlr.ret_val);
    }

    return mp_const_none; /* unreachable */
}

static MP_DEFINE_CONST_FUN_OBJ_1(
    mod_quickjs_ctx_run_jobs_obj,
    mod_quickjs_ctx_run_jobs
);


static mp_obj_t mod_quickjs_ctx_has_pending_jobs(
    mp_obj_t self_in
) {
    quickjs_ctx_t *state =
        quickjs_ctx_check_open(self_in);

    return mp_obj_new_bool(
        JS_IsJobPending(
            state->rt
        )
    );
}

static MP_DEFINE_CONST_FUN_OBJ_1(
    mod_quickjs_ctx_has_pending_jobs_obj,
    mod_quickjs_ctx_has_pending_jobs
);


/* -------------------------------------------------------------------------- */
/* Context.set_memory_limit()                                                 */
/* -------------------------------------------------------------------------- */

static mp_obj_t mod_quickjs_ctx_set_memory_limit(
    mp_obj_t self_in,
    mp_obj_t limit_obj
) {
    quickjs_ctx_t *state =
        quickjs_ctx_check_open(self_in);


    mp_int_t limit =
        mp_obj_get_int(
            limit_obj
        );

    if (limit < 0) {

        mp_raise_msg(
            &mp_type_ValueError,
            MP_ERROR_TEXT(
                "memory limit must be >= 0"
            )
        );
    }


    /*
     * JS_SetMemoryLimit() 是 void，对 runtime 立即生效。
     * 0 = 无限制。
     */
    JS_SetMemoryLimit(
        state->rt,
        (size_t)limit
    );


    return mp_const_none;
}

static MP_DEFINE_CONST_FUN_OBJ_2(
    mod_quickjs_ctx_set_memory_limit_obj,
    mod_quickjs_ctx_set_memory_limit
);


/* -------------------------------------------------------------------------- */
/* Context.set_max_stack_size()                                               */
/* -------------------------------------------------------------------------- */

static mp_obj_t mod_quickjs_ctx_set_max_stack_size(
    mp_obj_t self_in,
    mp_obj_t limit_obj
) {
    quickjs_ctx_t *state =
        quickjs_ctx_check_open(self_in);


    mp_int_t limit =
        mp_obj_get_int(
            limit_obj
        );

    if (limit < 0) {

        mp_raise_msg(
            &mp_type_ValueError,
            MP_ERROR_TEXT(
                "max stack size must be >= 0"
            )
        );
    }


    /*
     * JS_SetMaxStackSize() 是 void，对 runtime 立即生效。
     * 0 = 无限制（默认 1 MiB）。
     */
    JS_SetMaxStackSize(
        state->rt,
        (size_t)limit
    );


    return mp_const_none;
}

static MP_DEFINE_CONST_FUN_OBJ_2(
    mod_quickjs_ctx_set_max_stack_size_obj,
    mod_quickjs_ctx_set_max_stack_size
);


/* -------------------------------------------------------------------------- */
/* Context 类型                                                               */
/* -------------------------------------------------------------------------- */

static MP_DEFINE_CONST_FUN_OBJ_1(
    mod_quickjs_ctx_close_obj,
    mod_quickjs_ctx_close
);


static const mp_rom_map_elem_t
quickjs_context_locals_dict_table[] = {

    {
        MP_ROM_QSTR(MP_QSTR___del__),
        MP_ROM_PTR(
            &mod_quickjs_ctx_close_obj
        )
    },

    {
        MP_ROM_QSTR(MP_QSTR_eval),
        MP_ROM_PTR(
            &mod_quickjs_ctx_eval_obj
        )
    },

    {
        MP_ROM_QSTR(MP_QSTR_call),
        MP_ROM_PTR(
            &mod_quickjs_ctx_call_obj
        )
    },

    {
        MP_ROM_QSTR(MP_QSTR_get),
        MP_ROM_PTR(
            &mod_quickjs_ctx_get_obj
        )
    },

    {
        MP_ROM_QSTR(MP_QSTR_set),
        MP_ROM_PTR(
            &mod_quickjs_ctx_set_obj
        )
    },

    {
        MP_ROM_QSTR(MP_QSTR_promise),
        MP_ROM_PTR(
            &mod_quickjs_ctx_promise_obj
        )
    },

    {
        MP_ROM_QSTR(MP_QSTR_add_callable),
        MP_ROM_PTR(
            &mod_quickjs_ctx_add_callable_obj
        )
    },

    {
        MP_ROM_QSTR(MP_QSTR_set_time_limit),
        MP_ROM_PTR(
            &mod_quickjs_ctx_set_time_limit_obj
        )
    },

    {
        MP_ROM_QSTR(MP_QSTR_close),
        MP_ROM_PTR(
            &mod_quickjs_ctx_close_obj
        )
    },

    {
        MP_ROM_QSTR(MP_QSTR_gc),
        MP_ROM_PTR(
            &mod_quickjs_ctx_gc_obj
        )
    },

    {
        MP_ROM_QSTR(MP_QSTR__js_mem),
        MP_ROM_PTR(
            &mod_quickjs_ctx_js_mem_obj
        )
    },

    {
        MP_ROM_QSTR(MP_QSTR_run_jobs),
        MP_ROM_PTR(
            &mod_quickjs_ctx_run_jobs_obj
        )
    },

    {
        MP_ROM_QSTR(MP_QSTR_has_pending_jobs),
        MP_ROM_PTR(
            &mod_quickjs_ctx_has_pending_jobs_obj
        )
    },

    {
        MP_ROM_QSTR(MP_QSTR_set_unhandled_rejection_handler),
        MP_ROM_PTR(
            &mod_quickjs_ctx_set_unhandled_rejection_handler_obj
        )
    },

    {
        MP_ROM_QSTR(MP_QSTR_set_memory_limit),
        MP_ROM_PTR(
            &mod_quickjs_ctx_set_memory_limit_obj
        )
    },

    {
        MP_ROM_QSTR(MP_QSTR_set_max_stack_size),
        MP_ROM_PTR(
            &mod_quickjs_ctx_set_max_stack_size_obj
        )
    },
};


static MP_DEFINE_CONST_DICT(
    quickjs_context_locals_dict,
    quickjs_context_locals_dict_table
);


MP_DEFINE_CONST_OBJ_TYPE(
    quickjs_context_type,
    MP_QSTR_Context,
    MP_TYPE_FLAG_NONE,
    make_new, quickjs_context_make_new,
    locals_dict, &quickjs_context_locals_dict
);


/* ========================================================================= */
/*                         quickjs.help()                                     */
/* ========================================================================= */

static mp_obj_t mod_quickjs_help(void) {

    mp_printf(
        &mp_plat_print,

        "QuickJS-NG JavaScript Engine for MicroPython\n"
        "============================================\n"

        "Functions:\n"

        "  quickjs.init()\n"
        "      Initialize JS Runtime & Context\n"

        "  quickjs.eval(code)\n"
        "      Execute JavaScript code\n"

        "  quickjs.call(name, *args)\n"
        "      Call a global JavaScript function\n"

        "  quickjs.run_jobs()\n"
        "      Execute pending JS jobs (promise microtasks); returns count\n"

        "  quickjs.has_pending_jobs()\n"
        "      True if there are unexecuted JS jobs\n"

        "  quickjs.version()\n"
        "      Return QuickJS-NG engine version\n"

        "  quickjs.help()\n"
        "      Show this help documentation\n"

        "\n"

        "Context (isolated runtime):\n"

        "  quickjs.Context()\n"
        "      Create an independent JS runtime + context\n"

        "  ctx.eval(code)\n"
        "      Execute JavaScript code\n"

        "  ctx.call(name, *args)\n"
        "      Call a global JavaScript function\n"

        "  ctx.get(name)\n"
        "      Get a global variable (None if missing)\n"

        "  ctx.set(name, value)\n"
        "      Set a global variable\n"

        "  ctx.add_callable(name, callable)\n"
        "      Register a Python callable as a JS function\n"

        "  ctx.gc()\n"
        "      Run the QuickJS garbage collector\n"

        "  ctx._js_mem()\n"
        "      Debug: return QuickJS heap usage in bytes\n"

        "  ctx.run_jobs()\n"
        "      Execute pending JS jobs (promise microtasks); returns count\n"

        "  ctx.has_pending_jobs()\n"
        "      True if there are unexecuted JS jobs\n"

        "  ctx.set_unhandled_rejection_handler(cb_or_None)\n"
        "      Diagnose unhandled promise rejections: cb(reason, is_handled)\n"

        "  ctx.set_memory_limit(bytes)\n"
        "      Set the JS heap limit (0 = unlimited)\n"

        "  ctx.set_max_stack_size(bytes)\n"
        "      Set the JS stack limit (0 = unlimited)\n"

        "  ctx.set_time_limit(ms)\n"
        "      Set the JS execution timeout (0 = disabled)\n"

        "  ctx.close()\n"
        "      Free the runtime (idempotent; also runs on GC)\n"

        "  ctx.promise()\n"
        "      -> (p, resolve, reject): create a pending Promise from Python\n"
        "      (settle with resolve(value) / reject(reason); first call wins,\n"
        "       thenables/other promises are assimilated natively)\n"

        "  ctx.get('func') -> callable wrapper\n"
        "      JS functions convert to Python callables\n"
        "      wrapper.call(this_obj, *args): bind `this`\n"

        "\n"

        "Promise wrapper:\n"

        "  p.done()   -> settled (fulfilled or rejected)?\n"
        "  p.result() -> value, or raises the rejection / 'not settled'\n"
        "  p.then(on_fulfilled=None, on_rejected=None) -> new wrapper\n"
        "  p.catch(on_rejected=None)                    -> new wrapper\n"
        "  p.finally_(callback=None)                    -> new wrapper\n"
        "      (callbacks run via ctx.run_jobs(); return values resolve/assimilate)\n"

        "\n"

        "BigInt:\n"

        "  quickjs.bigint(value) -> marker object -> JS BigInt on conversion\n"
        "      (arbitrary precision; int64-range uses JS_NewBigInt64)\n"

        "\n"

        "JS -> MicroPython:\n"

        "  undefined -> None\n"
        "  null      -> None\n"
        "  boolean   -> bool\n"
        "  integer   -> int\n"
        "  number    -> float\n"
        "  string    -> str\n"
        "  Array     -> list\n"
        "  Object    -> dict\n"
        "  TypedArray-> bytes (raw byte representation)\n"
        "  Function  -> Python callable (Context)\n"
        "  Promise   -> Promise wrapper (Context): done/result/then/catch/finally_\n"

        "\n"

        "MicroPython -> JS:\n"

        "  None      -> null\n"
        "  bool      -> boolean\n"
        "  int       -> number\n"
        "  float     -> number\n"
        "  str       -> string\n"
        "  list      -> Array\n"
        "  tuple     -> Array\n"
        "  dict      -> Object\n"
        "  callable  -> Function (via add_callable)\n"
        "  bigint()  -> BigInt\n"

        "\n"

        "Example:\n"

        "  >>> import quickjs\n"
        "  >>> quickjs.init()\n"

        "  >>> quickjs.eval('function add(a,b) { return a+b; }')\n"

        "  >>> quickjs.call('add', 10, 20)\n"
        "  30\n"

        "  >>> quickjs.eval('function f(x) { return {value:x*2}; }')\n"

        "  >>> quickjs.call('f', {'x': 10})\n"

        "\n"
    );

    return mp_const_none;
}

static MP_DEFINE_CONST_FUN_OBJ_0(
    mod_quickjs_help_obj,
    mod_quickjs_help
);


/* ========================================================================= */
/*                         Module globals                                     */
/* ========================================================================= */

static const mp_rom_map_elem_t
quickjs_module_globals_table[] = {

    {
        MP_ROM_QSTR(MP_QSTR___name__),
        MP_ROM_QSTR(MP_QSTR_quickjs)
    },

    {
        MP_ROM_QSTR(MP_QSTR_help),
        MP_ROM_PTR(
            &mod_quickjs_help_obj
        )
    },

    {
        MP_ROM_QSTR(MP_QSTR_init),
        MP_ROM_PTR(
            &mod_quickjs_init_obj
        )
    },

    {
        MP_ROM_QSTR(MP_QSTR_eval),
        MP_ROM_PTR(
            &mod_quickjs_eval_obj
        )
    },

    {
        MP_ROM_QSTR(MP_QSTR_call),
        MP_ROM_PTR(
            &mod_quickjs_call_obj
        )
    },

    {
        MP_ROM_QSTR(MP_QSTR_run_jobs),
        MP_ROM_PTR(
            &mod_quickjs_run_jobs_obj
        )
    },

    {
        MP_ROM_QSTR(MP_QSTR_has_pending_jobs),
        MP_ROM_PTR(
            &mod_quickjs_has_pending_jobs_obj
        )
    },

    {
        MP_ROM_QSTR(MP_QSTR_version),
        MP_ROM_PTR(
            &mod_quickjs_version_obj
        )
    },

    {
        MP_ROM_QSTR(MP_QSTR_bigint),
        MP_ROM_PTR(
            &mod_quickjs_bigint_obj
        )
    },

    {
        MP_ROM_QSTR(MP_QSTR_Context),
        MP_ROM_PTR(
            &quickjs_context_type
        )
    },
};


static MP_DEFINE_CONST_DICT(
    quickjs_module_globals,
    quickjs_module_globals_table
);


/* ========================================================================= */
/*                         Module definition                                  */
/* ========================================================================= */

const mp_obj_module_t mp_module_quickjs = {

    .base = {
        &mp_type_module
    },

    .globals =
        (mp_obj_dict_t *)&quickjs_module_globals,
};


/*
 * 注意：必须写成单行。
 *
 * MicroPython 的 makeqstrdefs.py 用正则从预处理输出中提取
 * MP_REGISTER_MODULE(...)，其正则默认不跨行匹配。
 */
MP_REGISTER_MODULE(MP_QSTR_quickjs, mp_module_quickjs);

