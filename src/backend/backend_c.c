#include "backend/backend_c_internal.h"

/* C backend glue: prelude emission (c_begin) and epilogue (c_end),
 * plus the Re0Backend vtable instances. */


static void c_begin(Re0Codegen *c) {
    reset_c_state();
    if (c->backend == &re0_backend_c_freestanding) {
        re0_buffer_write_str(&c->output,
            "#include <stdint.h>\n#include <stdbool.h>\n\n"
            "typedef struct { int64_t tag; union { int64_t v0; } u; } Option;\n"
            "typedef struct { int64_t tag; union { int64_t v0; } u; } Result;\n"
            "typedef int64_t __reo_fn_ptr;\n\n");
        g_fwd_insert_pos = c->output.len;
        return;
    }
    re0_runtime_c_emit(&c->output);
    /* record prelude end position (for c_end to insert forward declarations) */
    g_fwd_insert_pos = c->output.len;
}

static void c_end(Re0Codegen *c) {
    /* generate all pending generic function bodies (before main) */
    flush_pending_instantiations(c);
    flush_generic_structs(c);
    flush_lambdas(c);

    if (c->backend == &re0_backend_c_freestanding) return;

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
