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
/* QuickJS Runtime / Context                                                  */
/* -------------------------------------------------------------------------- */

static JSRuntime *rt = NULL;
static JSContext *ctx = NULL;


/* -------------------------------------------------------------------------- */
/* Forward declarations                                                       */
/* -------------------------------------------------------------------------- */

static mp_obj_t quickjs_to_mp_obj(
    JSContext *ctx,
    JSValueConst val
);

static JSValue mp_to_quickjs(
    JSContext *ctx,
    mp_obj_t obj
);

static mp_obj_t quickjs_array_to_mp(
    JSContext *ctx,
    JSValueConst val
);

static mp_obj_t quickjs_object_to_mp(
    JSContext *ctx,
    JSValueConst val
);


/* -------------------------------------------------------------------------- */
/* QuickJS exception -> MicroPython exception                                 */
/* -------------------------------------------------------------------------- */

static void quickjs_raise_exception(
    JSContext *ctx,
    JSValue val
) {
    JSValue exception_val = JS_GetException(ctx);

    const char *err_msg =
        JS_ToCString(ctx, exception_val);

    if (err_msg != NULL) {

        size_t len = strlen(err_msg);

        char *msg = m_new(char, len + 1);

        memcpy(msg, err_msg, len + 1);

        JS_FreeCString(
            ctx,
            err_msg
        );

        JS_FreeValue(
            ctx,
            exception_val
        );

        JS_FreeValue(
            ctx,
            val
        );

        mp_raise_msg(
            &mp_type_RuntimeError,
            msg
        );
    }

    JS_FreeValue(
        ctx,
        exception_val
    );

    JS_FreeValue(
        ctx,
        val
    );

    mp_raise_msg(
        &mp_type_RuntimeError,
        MP_ERROR_TEXT("QuickJS exception")
    );
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
    JSValueConst val
) {
    uint32_t len = 0;

    JSValue length_val =
        JS_GetPropertyStr(
            ctx,
            val,
            "length"
        );

    if (JS_IsException(length_val)) {

        mp_raise_msg(
            &mp_type_RuntimeError,
            MP_ERROR_TEXT(
                "failed to get JS array length"
            )
        );
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

        mp_raise_msg(
            &mp_type_RuntimeError,
            MP_ERROR_TEXT(
                "invalid JS array length"
            )
        );
    }

    JS_FreeValue(
        ctx,
        length_val
    );


    mp_obj_t *items = NULL;

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

            if (items != NULL) {
                m_del(
                    mp_obj_t,
                    items,
                    len
                );
            }

            mp_raise_msg(
                &mp_type_RuntimeError,
                MP_ERROR_TEXT(
                    "failed to get JS array element"
                )
            );
        }


        items[i] =
            quickjs_to_mp_obj(
                ctx,
                item
            );


        JS_FreeValue(
            ctx,
            item
        );
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
    JSValueConst val
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

        mp_raise_msg(
            &mp_type_RuntimeError,
            MP_ERROR_TEXT(
                "failed to enumerate JS object"
            )
        );
    }


    mp_obj_t result =
        mp_obj_new_dict(
            prop_count
        );


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

        if (!JS_IsException(value)) {

            mp_obj_t mp_key =
                mp_obj_new_str(
                    key,
                    key_len
                );


            mp_obj_t mp_value =
                quickjs_to_mp_obj(
                    ctx,
                    value
                );


            mp_obj_dict_store(
                MP_OBJ_TO_PTR(result),
                mp_key,
                mp_value
            );


            JS_FreeValue(
                ctx,
                value
            );
        }


        JS_FreeCString(
            ctx,
            key
        );

        JS_FreeValue(
            ctx,
            key_val
        );
    }


    js_free(
        ctx,
        props
    );

    return result;
}


/* -------------------------------------------------------------------------- */
/* JSValue -> MicroPython                                                     */
/* -------------------------------------------------------------------------- */

static mp_obj_t quickjs_to_mp_obj(
    JSContext *ctx,
    JSValueConst val
) {
    int tag =
        JS_VALUE_GET_TAG(val);


    switch (tag) {

        /* ---------------------------------------------------------------- */
        /* undefined                                                        */
        /* ---------------------------------------------------------------- */

        case JS_TAG_UNDEFINED:
            return mp_const_none;


        /* ---------------------------------------------------------------- */
        /* null                                                              */
        /* ---------------------------------------------------------------- */

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
        /* string                                                            */
        /* ---------------------------------------------------------------- */

        case JS_TAG_STRING:

            return quickjs_string_to_mp(
                ctx,
                val
            );


        /* ---------------------------------------------------------------- */
        /* object                                                            */
        /* ---------------------------------------------------------------- */

        case JS_TAG_OBJECT: {

            if (JS_IsArray(val)) {

                return quickjs_array_to_mp(
                    ctx,
                    val
                );
            }

            return quickjs_object_to_mp(
                ctx,
                val
            );
        }


        default:
            break;
    }


    /*
     * QuickJS-NG Number
     */
    if (JS_IsNumber(val)) {

        double value;

        if (JS_ToFloat64(
                ctx,
                &value,
                val
            ) < 0) {

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
    mp_obj_t obj
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


    for (size_t i = 0; i < len; i++) {

        JSValue value =
            mp_to_quickjs(
                ctx,
                items[i]
            );


        if (JS_IsException(value)) {

            JS_FreeValue(
                ctx,
                array
            );

            return JS_EXCEPTION;
        }


        if (JS_SetPropertyUint32(
                ctx,
                array,
                i,
                value
            ) < 0) {

            /*
             * JS_SetPropertyUint32()
             * 在成功时接管 value 的所有权。
             *
             * 失败时释放它。
             */
            JS_FreeValue(
                ctx,
                value
            );

            JS_FreeValue(
                ctx,
                array
            );

            return JS_EXCEPTION;
        }
    }


    return array;
}


/* -------------------------------------------------------------------------- */
/* Python tuple -> JS Array                                                   */
/* -------------------------------------------------------------------------- */

static JSValue mp_tuple_to_quickjs(
    JSContext *ctx,
    mp_obj_t obj
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


    for (size_t i = 0; i < len; i++) {

        JSValue value =
            mp_to_quickjs(
                ctx,
                items[i]
            );


        if (JS_IsException(value)) {

            JS_FreeValue(
                ctx,
                array
            );

            return JS_EXCEPTION;
        }


        if (JS_SetPropertyUint32(
                ctx,
                array,
                i,
                value
            ) < 0) {

            JS_FreeValue(
                ctx,
                value
            );

            JS_FreeValue(
                ctx,
                array
            );

            return JS_EXCEPTION;
        }
    }


    return array;
}


/* -------------------------------------------------------------------------- */
/* Python dict -> JS Object                                                   */
/* -------------------------------------------------------------------------- */

static JSValue mp_dict_to_quickjs(
    JSContext *ctx,
    mp_obj_t obj
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


    for (size_t i = 0;
         i < map->used;
         i++) {

        if (map->table[i].value ==
            MP_OBJ_NULL) {

            continue;
        }


        mp_obj_t key =
            map->table[i].key;

        mp_obj_t value_obj =
            map->table[i].value;


        /*
         * 当前第一版要求 dict key 是 str。
         */
        if (!mp_obj_is_str(key)) {

            JS_FreeValue(
                ctx,
                object
            );

            mp_raise_msg(
                &mp_type_TypeError,
                MP_ERROR_TEXT(
                    "JS object keys must be str"
                )
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
                value_obj
            );


        if (JS_IsException(value)) {

            JS_FreeValue(
                ctx,
                object
            );

            return JS_EXCEPTION;
        }


        if (JS_SetPropertyStr(
                ctx,
                object,
                key_str,
                value
            ) < 0) {

            JS_FreeValue(
                ctx,
                value
            );

            JS_FreeValue(
                ctx,
                object
            );

            return JS_EXCEPTION;
        }
    }


    return object;
}


/* -------------------------------------------------------------------------- */
/* MicroPython object -> JSValue                                              */
/* -------------------------------------------------------------------------- */

static JSValue mp_to_quickjs(
    JSContext *ctx,
    mp_obj_t obj
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
            obj
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
            obj
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
            obj
        );
    }


    /* ---------------------------------------------------------------------- */
    /* unsupported                                                             */
    /* ---------------------------------------------------------------------- */

    mp_raise_msg(
        &mp_type_TypeError,
        MP_ERROR_TEXT(
            "unsupported MicroPython type"
        )
    );

    return JS_EXCEPTION;
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
     * 64 KiB QuickJS heap
     */
    JS_SetMemoryLimit(
        rt,
        64 * 1024
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


    JSValue val =
        JS_Eval(
            ctx,
            js_code,
            strlen(js_code),
            "<eval>",
            JS_EVAL_TYPE_GLOBAL
        );


    if (JS_IsException(val)) {

        quickjs_raise_exception(
            ctx,
            val
        );

        return mp_const_none;
    }


    mp_obj_t result =
        quickjs_to_mp_obj(
            ctx,
            val
        );


    JS_FreeValue(
        ctx,
        val
    );


    return result;
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
 */

static mp_obj_t mod_quickjs_call(
    size_t n_args,
    const mp_obj_t *args
) {
    if (ctx == NULL) {
        mod_quickjs_init();
    }


    /*
     * 第一个参数：
     *
     * function name
     */
    const char *function_name =
        mp_obj_str_get_str(
            args[0]
        );


    /*
     * 从 global object 获取函数
     */
    JSValue global =
        JS_GetGlobalObject(ctx);


    JSValue func =
        JS_GetPropertyStr(
            ctx,
            global,
            function_name
        );


    JS_FreeValue(
        ctx,
        global
    );


    if (JS_IsException(func)) {

        quickjs_raise_exception(
            ctx,
            func
        );

        return mp_const_none;
    }


    /*
     * 确认是 callable
     */
    if (!JS_IsFunction(
            ctx,
            func
        )) {

        JS_FreeValue(
            ctx,
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
     * 参数数量：
     *
     * n_args - 1
     *
     * 因为 args[0] 是函数名。
     */
    int argc =
        (int)n_args - 1;


    JSValue *argv = NULL;

    if (argc > 0) {

        argv =
            m_new(
                JSValue,
                argc
            );
    }


    /*
     * MicroPython -> JS
     */
    for (int i = 0; i < argc; i++) {

        argv[i] =
            mp_to_quickjs(
                ctx,
                args[i + 1]
            );


        if (JS_IsException(argv[i])) {

            /*
             * 释放已经创建的参数
             */
            for (int j = 0;
                 j < i;
                 j++) {

                JS_FreeValue(
                    ctx,
                    argv[j]
                );
            }


            if (argv != NULL) {

                m_del(
                    JSValue,
                    argv,
                    argc
                );
            }


            JS_FreeValue(
                ctx,
                func
            );

            return mp_const_none;
        }
    }


    /*
     * JS Function call
     */
    JSValue result =
        JS_Call(
            ctx,
            func,
            JS_UNDEFINED,
            argc,
            argv
        );


    /*
     * 参数所有权
     *
     * JS_Call 不会替我们释放 argv 中的 JSValue。
     */
    for (int i = 0; i < argc; i++)_FreeValue(
            ctx,
            argv[i]
        );
    }


    if (argv != NULL) {

        m_del(
            JSValue,
            argv,
            argc
        );
    }


    JS_FreeValue(
        ctx,
        func
    );


    /*
     * JS exception
     */
    if (JS_IsException(result)) {

        quickjs_raise_exception(
            ctx,
            result
        );

        return mp_const_none;
    }


    /*
     * JS -> MicroPython
     */
    mp_obj_t mp_result =
        quickjs_to_mp_obj(
            ctx,
            result
        );


    JS_FreeValue(
        ctx,
        result
    );


    return mp_result;
}

static MP_DEFINE_CONST_FUN_OBJ_VAR(
    mod_quickjs_call_obj,
    1,
    mod_quickjs_call
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

        "JS -> MicroPython:\n"

        "  undefined -> None\n"
        "  null      -> None\n"
        "  boolean   -> bool\n"
        "  integer   -> int\n"
        "  number    -> float\n"
        "  string    -> str\n"
        "  Array     -> list\n"
        "  Object    -> dict\n"

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


MP_REGISTER_MODULE(
    MP_QSTR_quickjs,
    mp_module_quickjs
);

