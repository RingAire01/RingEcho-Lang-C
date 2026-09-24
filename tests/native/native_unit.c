#include "backend/native_internal.h"
#include <stdio.h>
#include <stdlib.h>

/* Link with --wrap=realloc: fail one allocation, then let error reporting and
 * cleanup allocate normally. Exercise every buffer-growth point in ELF output. */
void *__real_realloc(void *ptr, size_t size);
static unsigned allocation_count, fail_at;
void *__wrap_realloc(void *ptr, size_t size) {
    allocation_count++;
    if (fail_at && allocation_count == fail_at) return NULL;
    return __real_realloc(ptr, size);
}

static bool elf_allocation_case(unsigned failure, unsigned *allocations) {
    Re0ErrorList errors;
    re0_error_list_init(&errors);
    Re0Codegen c;
    re0_codegen_init(&c, &errors, NULL, &re0_backend_native);
    c.emit_main = false;
    Re0Stmt ast = {.kind = STMT_FUNCTION};
    NFunction function = {.name = "sample", .ast = &ast, .size = 3};
    NModule m = {.codegen = &c, .functions = &function, .count = 1};
    re0_buffer_init(&m.text);
    re0_buffer_write_str(&m.text, "abc");
    allocation_count = 0; fail_at = failure;
    bool ok = n_elf(&m);
    unsigned count = allocation_count;
    fail_at = 0;
    bool expected = failure == 0 || count < failure;
    bool passed = ok == expected;
    if (allocations) *allocations = count;
    re0_buffer_free(&m.text);
    re0_codegen_destroy(&c);
    re0_error_list_free(&errors);
    return passed;
}

static bool rejects_invalid_ir(NInst *instructions, size_t count, size_t labels) {
    Re0ErrorList errors;
    re0_error_list_init(&errors);
    Re0Codegen c;
    re0_codegen_init(&c, &errors, NULL, &re0_backend_native);
    Re0Stmt ast = {.kind = STMT_FUNCTION};
    NFunction f = {.ast = &ast, .result = N_UNIT, .ir = instructions,
                   .count = count, .labels = labels};
    NModule m = {.codegen = &c};
    bool passed = !n_verify(&m, &f) && c.had_error;
    re0_codegen_destroy(&c);
    re0_error_list_free(&errors);
    return passed;
}

int main(void) {
    unsigned allocations = 0;
    if (!elf_allocation_case(0, &allocations)) return 1;
    for (unsigned i = 1; i <= allocations; i++) {
        if (!elf_allocation_case(i, NULL)) {
            fprintf(stderr, "ELF allocation failure %u was not propagated\n", i);
            return 1;
        }
    }
    NInst underflow[] = {{N_RETURN, N_UNIT, 0, 0, -1}};
    NInst bad_jump[] = {{N_JUMP, N_UNIT, 0, 1, -1}};
    NInst bad_slot[] = {{N_LOAD, N_I64, 0, 1, -1}};
    NInst bad_call[] = {{N_CALL, N_I64, 0, 1, -1}};
    NInst join[] = {
        {N_CONST, N_BOOL, 1, 0, -1}, {N_JZ, N_UNIT, 0, 0, -1},
        {N_CONST, N_I64, 2, 0, -1}, {N_LABEL, N_UNIT, 0, 0, -1},
        {N_END, N_UNIT, 0, 0, -1}
    };
    if (!rejects_invalid_ir(underflow, 1, 0) || !rejects_invalid_ir(bad_jump, 1, 0) ||
        !rejects_invalid_ir(bad_slot, 1, 0) || !rejects_invalid_ir(bad_call, 1, 0) ||
        !rejects_invalid_ir(join, 5, 1)) return 1;
    printf("native unit checks passed (%u allocation failure points, 5 invalid IR cases)\n", allocations);
    return 0;
}
