#ifndef RE0_BACKEND_H
#define RE0_BACKEND_H
#include "front/ast.h"
#include "base/buffer.h"
#include "analysis/model.h"
#include "base/error.h"
#include "extra/re0_gc.h"
#include <stdbool.h>

typedef struct Re0Codegen Re0Codegen;

struct Re0Codegen {
    Re0Buffer         output;
    Re0ErrorList     *errors;
    Re0SemanticModel *model;
    const struct Re0Backend *backend;
    int temp_counter;
    int label_counter;
    int reg_counter;
    int last_reg;
    Re0GcMode gc_mode;
    bool had_error;
    /* When false (library mode), the backend emits no `int main()` entry
     * point so the generated C can be linked into a shared library. */
    bool emit_main;
    bool c_return_void;
};

typedef struct Re0Backend {
    const char *name;
    void (*begin)(Re0Codegen *c);
    void (*end)(Re0Codegen *c);
    int  (*gen_expr)(Re0Codegen *c, Re0Expr *e);
    void (*gen_stmt)(Re0Codegen *c, Re0Stmt *s, int depth);
} Re0Backend;

extern Re0Backend re0_backend_c;
/* Emits C for a freestanding environment; the caller owns compilation and linking. */
extern Re0Backend re0_backend_c_freestanding;
extern Re0Backend re0_backend_reo;

void re0_codegen_init(Re0Codegen *c, Re0ErrorList *errors,
                      Re0SemanticModel *model, Re0Backend *backend);
bool re0_codegen_generate(Re0Codegen *c, Re0StmtVec *checked);
const char *re0_codegen_output(Re0Codegen *c);
void re0_codegen_destroy(Re0Codegen *c);

int  re0_codegen_new_label(Re0Codegen *c);
int  re0_codegen_new_temp(Re0Codegen *c);
int  re0_codegen_new_reg(Re0Codegen *c);

#endif
