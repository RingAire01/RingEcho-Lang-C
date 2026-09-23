#include "backend/backend_c_internal.h"

/* C backend glue: prelude emission (c_begin) and epilogue (c_end),
 * plus the Re0Backend vtable instances. */


static void c_begin(Re0Codegen *c) {
    reset_c_state();
    if (c->backend == &re0_backend_c_freestanding) {
        re0_buffer_write_str(&c->output,
            "#include <stdint.h>\n#include <stdbool.h>\n"
            "#include <stdio.h>\n#include <stdlib.h>\n\n"
            /* Recursion depth guard for the freestanding backend (WASI-safe:
             * uses only stdio/stdlib, no pthread). Matches the full runtime's
             * guard so generated user functions compile in both backends. */
            "#ifndef __REO_MAX_CALL_DEPTH\n"
            "#define __REO_MAX_CALL_DEPTH 1024\n"
            "#endif\n"
            "static int64_t __reo_call_depth = 0;\n"
            "typedef struct { int64_t prev; } __reo_depth_guard_t;\n"
            "static __reo_depth_guard_t __reo_depth_enter(void) {\n"
            "    __reo_depth_guard_t g; g.prev = __reo_call_depth++;\n"
            "    if (__reo_call_depth > __REO_MAX_CALL_DEPTH) {\n"
            "        fprintf(stderr, \"runtime error: stack overflow (max call depth %d exceeded)\\n\", __REO_MAX_CALL_DEPTH);\n"
            "        abort();\n"
            "    }\n"
            "    return g;\n"
            "}\n"
            "static void __reo_depth_leave(__reo_depth_guard_t *g) { __reo_call_depth = g->prev; }\n"
            "#define __REO_DEPTH_GUARD __reo_depth_guard_t __reo_dg __attribute__((cleanup(__reo_depth_leave))) = __reo_depth_enter()\n\n"
            "typedef struct { int64_t tag; union { int64_t v0; } u; } Option;\n"
            "typedef struct { int64_t tag; union { int64_t v0; } u; } Result;\n"
            "typedef int64_t __reo_fn_ptr;\n\n");
        re0_buffer_write_str(&c->output, "#include <string.h>\n#define __REO_CONV_ALLOC malloc\n#define __REO_CONV_FREE free\n#define __REO_FREESTANDING 1\n");
        re0_runtime_conversion_emit(&c->output);
        g_fwd_insert_pos = c->output.len;
        return;
    }
    re0_runtime_c_emit(&c->output);
    re0_runtime_conversion_emit(&c->output);
    /* record prelude end position (for c_end to insert forward declarations) */
    g_fwd_insert_pos = c->output.len;
}

static void c_end(Re0Codegen *c) {
    /* generate all pending generic function bodies (before main) */
    flush_pending_instantiations(c);
    flush_generic_structs(c);
    flush_lambdas(c);

    if (c->backend == &re0_backend_c_freestanding) return;

    if (!c->emit_main) return;

    int mode = re0_gc_mode_to_int(c->gc_mode);
    re0_buffer_write_fmt(&c->output,
        "\nint main(int argc, char **argv) {\n"
        "    __reo_gc.mode = %d;\n"
        "    __reo_init_argv(argc, argv);\n"
        "    main_();\n"
        "    __reo_gc_collect();\n"
        "    return 0;\n}\n", mode);
}

Re0Backend re0_backend_c = { "c", c_begin, c_end, c_gen_expr, c_gen_stmt };
Re0Backend re0_backend_c_freestanding = {
    "c-freestanding", c_begin, c_end, c_gen_expr, c_gen_stmt
};
