#include "modquickjs.h"
#include "py/reader.h"
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
        mp_printf(&mp_plat_print,
                  "REPL Commands:\n"
                  "  .help         Show this help message\n"
                  "  .load <file>  Load and evaluate a JavaScript file\n"
                  "  .time <code...> Benchmark execution time of JavaScript "
                  "code\n"
                  "  .mem          Print QuickJS heap usage\n"
                  "  .gc           Run JavaScript garbage collector\n"
                  "  .clear        Clear context state\n"
                  "  .version      Show engine version\n"
                  "  .exit         Exit REPL\n");
      } else if (strcmp(line_str, ".version") == 0) {
        mp_printf(&mp_plat_print, "QuickJS-NG version: %s\n", JS_GetVersion());
      } else if (strncmp(line_str, ".load ", 6) == 0) {
        const char *filename = line_str + 6;
        while (*filename == ' ') {
          filename++;
        }
        if (*filename == '\0') {
          mp_printf(&mp_plat_print, "Usage: .load <filename.js>\n");
        } else {
          mp_reader_t reader;
          nlr_buf_t nlr_file;
          if (nlr_push(&nlr_file) == 0) {
            qstr q_name = qstr_from_str(filename);
            mp_reader_new_file(&reader, q_name);
            nlr_pop();

            vstr_t fbuf;
            vstr_init(&fbuf, 256);
            while (reader.readbyte != NULL) {
              int c = reader.readbyte(reader.data);
              if (c < 0) {
                break;
              }
              vstr_add_byte(&fbuf, c);
            }
            reader.close(reader.data);

            JSContext *qctx = state->ctx;
            quickjs_ctx_enter(state);
            quickjs_ctx_arm_timeout(state);

            JSValue val = JS_Eval(qctx, vstr_str(&fbuf), vstr_len(&fbuf),
                                  filename, JS_EVAL_TYPE_GLOBAL);
            (void)quickjs_ctx_finish_timeout(state);
            quickjs_ctx_leave(state);
            vstr_clear(&fbuf);

            if (JS_IsException(val)) {
              JSValue exc = JS_GetException(qctx);
              const char *exc_str = JS_ToCString(qctx, exc);
              mp_printf(&mp_plat_print, "Error in '%s': %s\n", filename,
                        (exc_str != NULL) ? exc_str : "exception");
              if (exc_str != NULL) {
                JS_FreeCString(qctx, exc_str);
              }
              JS_FreeValue(qctx, exc);
            } else {
              quickjs_run_jobs_helper(state);
              mp_printf(&mp_plat_print, "Loaded and executed '%s'\n", filename);
              JS_FreeValue(qctx, val);
            }
          } else {
            mp_printf(&mp_plat_print, "Failed to open file '%s'\n", filename);
          }
        }
      } else if (strncmp(line_str, ".time ", 6) == 0) {
        const char *code = line_str + 6;
        while (*code == ' ') {
          code++;
        }
        if (*code != '\0') {
          JSContext *qctx = state->ctx;
          quickjs_ctx_enter(state);
          quickjs_ctx_arm_timeout(state);

          mp_uint_t t_start = mp_hal_ticks_us();
          JSValue val =
              JS_Eval(qctx, code, strlen(code), "<time>", JS_EVAL_TYPE_GLOBAL);
          mp_uint_t t_elapsed_us = mp_hal_ticks_us() - t_start;

          (void)quickjs_ctx_finish_timeout(state);
          quickjs_ctx_leave(state);

          if (JS_IsException(val)) {
            JSValue exc = JS_GetException(qctx);
            const char *exc_str = JS_ToCString(qctx, exc);
            mp_printf(&mp_plat_print, "Uncaught %s\n",
                      (exc_str != NULL) ? exc_str : "error");
            if (exc_str != NULL) {
              JS_FreeCString(qctx, exc_str);
            }
            JS_FreeValue(qctx, exc);
          } else {
            quickjs_run_jobs_helper(state);
            const char *val_str = JS_ToCString(qctx, val);
            mp_printf(&mp_plat_print, "Result: %s\n",
                      (val_str != NULL) ? val_str : "undefined");
            if (val_str != NULL) {
              JS_FreeCString(qctx, val_str);
            }
            JS_FreeValue(qctx, val);
          }
          mp_printf(&mp_plat_print, "Execution time: %lu.%03lu ms (%lu us)\n",
                    (unsigned long)(t_elapsed_us / 1000),
                    (unsigned long)(t_elapsed_us % 1000),
                    (unsigned long)t_elapsed_us);
        }
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
