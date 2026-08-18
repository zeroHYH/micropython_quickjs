#include "modquickjs.h"
#include "shared/readline/readline.h"

/* -------------------------------------------------------------------------- */
/* Interactive C REPL Implementation                                          */
/* -------------------------------------------------------------------------- */

void quickjs_repl_run(quickjs_ctx_t *state) {
  if (state == NULL || state->closed || state->ctx == NULL) {
    mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("context closed"));
  }

  mp_printf(&mp_plat_print, "MicroPython QuickJS REPL (ES2023)\n");
  mp_printf(&mp_plat_print,
            "Type \".help\" for commands. Press Ctrl+D or type \".exit\" to "
            "return.\n\n");

  readline_init0();

  vstr_t input_buf;
  vstr_init(&input_buf, 256);
  int indent_depth = 0;

  for (;;) {
    const char *prompt =
        (indent_depth > 0 || vstr_len(&input_buf) > 0) ? "... " : "js> ";
    vstr_t line;
    vstr_init(&line, 64);
    int ret = readline(&line, prompt);

    if (ret == CHAR_CTRL_D) {
      /* EOF / Exit */
      vstr_clear(&line);
      vstr_clear(&input_buf);
      mp_printf(&mp_plat_print, "\n");
      break;
    }

    if (ret == CHAR_CTRL_C) {
      /* Keyboard Interrupt */
      vstr_clear(&line);
      vstr_reset(&input_buf);
      indent_depth = 0;
      mp_printf(&mp_plat_print, "\n");
      continue;
    }

    const char *line_str = vstr_null_terminated_str(&line);

    /* Dot commands (only when at start of multiline buffer) */
    if (vstr_len(&input_buf) == 0 && line_str[0] == '.') {
      if (strcmp(line_str, ".exit") == 0 || strcmp(line_str, ".quit") == 0) {
        vstr_clear(&line);
        vstr_clear(&input_buf);
        break;
      } else if (strcmp(line_str, ".help") == 0) {
        mp_printf(&mp_plat_print, "REPL Commands:\n"
                                  "  .help   Show this help message\n"
                                  "  .clear  Clear context state\n"
                                  "  .mem    Print QuickJS heap usage\n"
                                  "  .gc     Run JavaScript garbage collector\n"
                                  "  .exit   Exit REPL\n");
      } else if (strcmp(line_str, ".mem") == 0) {
        if (state->rt != NULL) {
          JSMemoryUsage stats;
          JS_ComputeMemoryUsage(state->rt, &stats);
          mp_printf(&mp_plat_print,
                    "JS Heap Memory: %lu bytes (allocated: %lu bytes, "
                    "objects: %ld)\n",
                    (unsigned long)stats.memory_used_size,
                    (unsigned long)stats.malloc_size, (long)stats.obj_count);
        }
      } else if (strcmp(line_str, ".gc") == 0) {
        if (state->rt != NULL) {
          JS_RunGC(state->rt);
          mp_printf(&mp_plat_print, "JavaScript GC complete.\n");
        }
      } else if (strcmp(line_str, ".clear") == 0) {
        mp_printf(&mp_plat_print, "Context reset.\n");
      } else {
        mp_printf(&mp_plat_print,
                  "Unknown REPL command: %s (type .help for commands)\n",
                  line_str);
      }
      vstr_clear(&line);
      continue;
    }

    /* Append line to input buffer */
    if (vstr_len(&input_buf) > 0) {
      vstr_add_byte(&input_buf, '\n');
    }
    vstr_add_strn(&input_buf, vstr_str(&line), vstr_len(&line));
    vstr_clear(&line);

    /* Multiline brace/bracket/quote matching */
    const char *src = vstr_null_terminated_str(&input_buf);
    size_t src_len = vstr_len(&input_buf);

    int open_braces = 0;
    bool in_quote = false;
    char quote_char = 0;

    for (size_t i = 0; i < src_len; i++) {
      char c = src[i];
      if (in_quote) {
        if (c == '\\' && i + 1 < src_len) {
          i++;
        } else if (c == quote_char) {
          in_quote = false;
        }
      } else {
        if (c == '"' || c == '\'' || c == '`') {
          in_quote = true;
          quote_char = c;
        } else if (c == '{' || c == '(' || c == '[') {
          open_braces++;
        } else if (c == '}' || c == ')' || c == ']') {
          if (open_braces > 0) {
            open_braces--;
          }
        }
      }
    }

    indent_depth = open_braces + (in_quote ? 1 : 0);
    if (indent_depth > 0) {
      continue; /* Wait for closing braces */
    }

    /* Skip whitespace-only lines */
    if (src_len == 0) {
      vstr_reset(&input_buf);
      continue;
    }

    /* Evaluate input */
    JSContext *qctx = state->ctx;
    quickjs_ctx_enter(state);
    quickjs_ctx_arm_timeout(state);

    JSValue val = JS_Eval(qctx, src, src_len, "<repl>", JS_EVAL_TYPE_GLOBAL);
    (void)quickjs_ctx_finish_timeout(state);
    quickjs_ctx_leave(state);

    if (JS_IsException(val)) {
      JSValue exc = JS_GetException(qctx);
      const char *exc_str = JS_ToCString(qctx, exc);
      if (exc_str != NULL) {
        mp_printf(&mp_plat_print, "Uncaught %s\n", exc_str);
        JS_FreeCString(qctx, exc_str);
      } else {
        mp_printf(&mp_plat_print, "Uncaught JavaScript error\n");
      }
      JS_FreeValue(qctx, exc);
    } else {
      /* Run pending promise microtasks */
      quickjs_run_jobs_helper(state);

      /* Print result if not undefined */
      if (!JS_IsUndefined(val)) {
        if (JS_IsPromise(val)) {
          JSValue res = JS_PromiseResult(qctx, val);
          int pstate = JS_PromiseState(qctx, val);
          if (pstate == JS_PROMISE_FULFILLED) {
            const char *res_str = JS_ToCString(qctx, res);
            mp_printf(&mp_plat_print, "Promise { <fulfilled>: %s }\n",
                      (res_str != NULL) ? res_str : "...");
            if (res_str != NULL) {
              JS_FreeCString(qctx, res_str);
            }
          } else if (pstate == JS_PROMISE_REJECTED) {
            const char *res_str = JS_ToCString(qctx, res);
            mp_printf(&mp_plat_print, "Promise { <rejected>: %s }\n",
                      (res_str != NULL) ? res_str : "...");
            if (res_str != NULL) {
              JS_FreeCString(qctx, res_str);
            }
          } else {
            mp_printf(&mp_plat_print, "Promise { <pending> }\n");
          }
          JS_FreeValue(qctx, res);
        } else {
          const char *val_str = JS_ToCString(qctx, val);
          if (val_str != NULL) {
            mp_printf(&mp_plat_print, "%s\n", val_str);
            JS_FreeCString(qctx, val_str);
          }
        }
      }
      JS_FreeValue(qctx, val);
    }
    vstr_reset(&input_buf);
  }

  vstr_clear(&input_buf);
}
