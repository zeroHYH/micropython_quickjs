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
#define QUICKJS_DEFAULT_MEMORY_LIMIT (64 * 1024)
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

typedef struct _quickjs_ctx_t {
    JSRuntime *rt;
    JSContext *ctx;

    bool closed;

    size_t memory_limit;
    size_t max_stack_size;

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
} quickjs_ctx_t;

typedef struct _mp_obj_quickjs_context_t {
    mp_obj_base_t base;
    quickjs_ctx_t *state;
} mp_obj_quickjs_context_t;


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

static mp_obj_t quickjs_call_value_helper(
    quickjs_ctx_t *state,
    JSValueConst func,
    size_t argc,
    const mp_obj_t *mp_args
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

static void quickjs_raise_exception(
    JSContext *ctx,
    JSValue val
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
 *   - func_val 持有对 JS 函数的一个引用（JS_DupValue）。
 *     即使 JS 侧把全局变量覆盖/删除，wrapper 仍持有函数对象。
 *
 *   - 释放：__del__（finaliser）中，仅当 state->ctx 仍存在时
 *     JS_FreeValue(func_val)。若 Context 已被 close()（state->ctx==NULL），
 *     JS 堆已释放，func_val 是悬垂值，绝不触碰，直接跳过。
 *
 *   - 调用：mod_quickjs_function_call 先检查 context 是否关闭，
 *     关闭则抛 RuntimeError("context closed")，绝不触碰悬垂 func_val。
 */
typedef struct _mp_obj_quickjs_function_t {
    mp_obj_base_t base;

    quickjs_ctx_t *state;
    mp_obj_t ctx_obj;   /* 强引用：保证 Context 不被 GC（见上） */

    JSValue func_val;   /* 对 JS 函数对象的引用 */

    bool attached;      /* 当前是否持有 func_val，需在 __del__ 释放 */
} mp_obj_quickjs_function_t;


static mp_obj_t mod_quickjs_function_del(
    mp_obj_t self_in
) {
    mp_obj_quickjs_function_t *f =
        MP_OBJ_TO_PTR(self_in);

    if (f->attached &&
        f->state != NULL &&
        f->state->ctx != NULL) {

        JS_FreeValue(
            f->state->ctx,
            f->func_val
        );
    }

    f->attached = false;
    f->func_val = JS_UNDEFINED;

    return mp_const_none;
}

static MP_DEFINE_CONST_FUN_OBJ_1(
    mod_quickjs_function_del_obj,
    mod_quickjs_function_del
);


static const mp_rom_map_elem_t
quickjs_function_locals_dict_table[] = {

    {
        MP_ROM_QSTR(MP_QSTR___del__),
        MP_ROM_PTR(
            &mod_quickjs_function_del_obj
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
    f->func_val = JS_DupValue(ctx, val);
    f->attached = true;

    return MP_OBJ_FROM_PTR(f);
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
};


static void quickjs_cb_finalize(
    void *opaque
) {
    quickjs_callback_t *node =
        (quickjs_callback_t *)opaque;

    if (node != NULL) {

        node->opaque_active = false;
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

    } else {

        /* Python exception -> JS exception */
        if (mp_args != NULL) {

            m_del(
                mp_obj_t,
                mp_args,
                argc
            );
        }

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
 * 若启用了 timeout，设置 deadline 并清零 triggered。
 */
static void quickjs_ctx_arm_timeout(
    quickjs_ctx_t *state
) {
    if (state == NULL ||
        !state->timeout_enabled) {

        return;
    }

    state->deadline_ms =
        mp_hal_ticks_ms() +
        state->timeout_ms;

    state->timeout_triggered = false;
}


/*
 * JS 执行结束后调用：返回是否本次执行触发了超时，
 * 并清除 deadline / triggered（超时状态不污染后续调用）。
 */
static bool quickjs_ctx_finish_timeout(
    quickjs_ctx_t *state
) {
    if (state == NULL) {
        return false;
    }

    bool t =
        state->timeout_triggered;

    state->timeout_triggered = false;
    state->deadline_ms = 0;

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
static mp_obj_t mod_quickjs_ctx_add_callable(
    mp_obj_t self_in,
    mp_obj_t name_obj,
    mp_obj_t callable_obj
) {
    quickjs_ctx_t *state =
        quickjs_ctx_check_open(self_in);

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

        if (mp_obj_equal(
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
static void quickjs_raise_exception(
    JSContext *ctx,
    JSValue val
) {
    JSValue exception_val = JS_GetException(ctx);

    /*
     * val 是调用方传入的 JS_EXCEPTION 哨兵（或 JS_UNDEFINED）。
     * 这类值没有引用计数，JS_FreeValue 是无操作。
     */
    JS_FreeValue(ctx, val);

    if (JS_IsUninitialized(exception_val)) {

        mp_raise_msg(
            &mp_type_RuntimeError,
            MP_ERROR_TEXT("QuickJS exception")
        );
    }


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

    mp_obj_t result =
        mp_obj_new_str(
            str,
            len
        );

    JS_FreeCString(
        ctx,
        str
    );

    return result;
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


    mp_obj_t result =
        mp_obj_new_dict(
            prop_count
        );


    bool pushed = false;

    /*
     * 外层 nlr：任何属性转换抛异常时，
     * pop 循环检测栈 + 释放 props（JS_FreePropertyEnum）。
     * 内层按属性 nlr 负责释放 value / key / key_val。
     */
    nlr_buf_t outer;

    if (nlr_push(&outer) == 0) {

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
/* JS Uint8Array -> MicroPython bytes                                          */
/* -------------------------------------------------------------------------- */

/*
 * copy 语义，同 ArrayBuffer。
 * 只处理 Uint8Array（调用方已用 JS_GetTypedArrayType 判断）。
 */
static mp_obj_t quickjs_uint8array_to_mp(
    JSContext *ctx,
    JSValueConst val
) {
    size_t size = 0;

    uint8_t *data =
        JS_GetUint8Array(
            ctx,
            &size,
            val
        );

    if (data == NULL) {

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
             * TypedArray：只支持 Uint8Array -> bytes，
             * 其他 TypedArray 明确报错。
             */
            {
                int ta =
                    JS_GetTypedArrayType(val);

                if (ta >= 0) {

                    if (ta == JS_TYPED_ARRAY_UINT8) {

                        return quickjs_uint8array_to_mp(
                            ctx,
                            val
                        );
                    }

                    mp_raise_msg(
                        &mp_type_TypeError,
                        MP_ERROR_TEXT(
                            "unsupported typed array"
                        )
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

static JSValue mp_to_quickjs(
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
 * 返回值：mp_obj_t 直接返回（JS 结果已转换并释放）。
 */
static mp_obj_t quickjs_call_value_helper(
    quickjs_ctx_t *state,
    JSValueConst func,
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

        argv =
            m_new(
                JSValue,
                n
            );
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
            JS_UNDEFINED,
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


/* -------------------------------------------------------------------------- */
/* Function wrapper __call__                                                   */
/* -------------------------------------------------------------------------- */

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

    return quickjs_call_value_helper(
        state,
        f->func_val,
        n_args,
        args
    );
}


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
     */
    mp_obj_t mp_result =
        quickjs_call_value_helper(
            state,
            func,
            argc,
            mp_args
        );

    JS_FreeValue(
        qctx,
        func
    );

    return mp_result;
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
        state->memory_limit =
            QUICKJS_DEFAULT_MEMORY_LIMIT;
        state->max_stack_size =
            0; /* 0 = 使用 QuickJS 默认（1 MiB） */
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


    state->closed = true;


    /*
     * 阶段 3：释放回调注册表的 JSValue 引用（ctx 仍有效时）。
     * 闭包在 JS_FreeContext 里 finalize 时会触碰其 opaque 节点，
     * 因此节点必须先保持存活、再释放（见下方 m_del 循环）。
     */
    if (state->ctx != NULL) {

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


    const char *js_code =
        mp_obj_str_get_str(
            js_code_obj
        );


    return quickjs_eval_helper(
        state,
        js_code
    );
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


    const char *function_name =
        mp_obj_str_get_str(
            args[1]
        );


    return quickjs_call_helper(
        state,
        function_name,
        n_args - 2,
        args + 2
    );
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
static mp_obj_t mod_quickjs_ctx_get(
    mp_obj_t self_in,
    mp_obj_t name_obj
) {
    quickjs_ctx_t *state =
        quickjs_ctx_check_open(self_in);

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

static MP_DEFINE_CONST_FUN_OBJ_2(
    mod_quickjs_ctx_get_obj,
    mod_quickjs_ctx_get
);


/* -------------------------------------------------------------------------- */
/* Context.set()                                                              */
/* -------------------------------------------------------------------------- */

/*
 * 把 MicroPython 对象写入 JS global object。
 */
static mp_obj_t mod_quickjs_ctx_set(
    mp_obj_t self_in,
    mp_obj_t name_obj,
    mp_obj_t value_obj
) {
    quickjs_ctx_t *state =
        quickjs_ctx_check_open(self_in);

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

static MP_DEFINE_CONST_FUN_OBJ_3(
    mod_quickjs_ctx_set_obj,
    mod_quickjs_ctx_set
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


    state->memory_limit =
        (size_t)limit;

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


    state->max_stack_size =
        (size_t)limit;

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

        "  ctx.set_memory_limit(bytes)\n"
        "      Set the JS heap limit (0 = unlimited)\n"

        "  ctx.set_max_stack_size(bytes)\n"
        "      Set the JS stack limit (0 = unlimited)\n"

        "  ctx.set_time_limit(ms)\n"
        "      Set the JS execution timeout (0 = disabled)\n"

        "  ctx.close()\n"
        "      Free the runtime (idempotent; also runs on GC)\n"

        "  ctx.get('func') -> callable wrapper\n"
        "      JS functions convert to Python callables\n"

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
        "  Function  -> Python callable (Context)\n"

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
        MP_ROM_QSTR(MP_QSTR_version),
        MP_ROM_PTR(
            &mod_quickjs_version_obj
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

