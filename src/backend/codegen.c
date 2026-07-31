#include "backend.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

void re0_codegen_init(Re0Codegen *c, Re0ErrorList *errors,
                      Re0SemanticModel *model, Re0Backend *backend) {
    re0_buffer_init(&c->output);
    c->errors = errors;
    c->model = model;
    c->backend = backend;
    c->temp_counter = 0;
    c->label_counter = 0;
    c->reg_counter = 0;
    c->last_reg = 0;
    c->gc_mode = RE0_GC_NONE;
    c->had_error = false;
}

static Re0GcMode scan_gc_mode(Re0StmtVec *checked) {
    for (size_t i = 0; i < Re0StmtVec_len(checked); i++) {
        Re0Stmt *s = checked->data[i];
        if (s && s->kind == STMT_ATTRIBUTE &&
            s->attribute.attr_name &&
            strcmp(s->attribute.attr_name, "gc") == 0 &&
            s->attribute.attr_arg) {
            return re0_gc_mode_from_str(s->attribute.attr_arg);
        }
    }
    return RE0_GC_NONE;
}

bool re0_codegen_generate(Re0Codegen *c, Re0StmtVec *checked) {
    if (!c || !c->backend || !checked) return false;
    c->gc_mode = scan_gc_mode(checked);
    c->backend->begin(c);
    for (size_t i = 0; i < Re0StmtVec_len(checked); i++) {
        c->backend->gen_stmt(c, checked->data[i], 0);
    }
    c->backend->end(c);
    if (re0_buffer_failed(&c->output)) {
        re0_error_append(c->errors, RE0_ERR_INTERNAL, RE0_SPAN_ZERO, NULL,
                         "out of memory while generating output");
        c->had_error = true;
    }
    return !c->had_error;
}

const char *re0_codegen_output(Re0Codegen *c) {
    if (!c || re0_buffer_failed(&c->output)) return NULL;
    re0_buffer_write_char(&c->output, '\0');
    return re0_buffer_failed(&c->output) ? NULL : c->output.data;
}

void re0_codegen_destroy(Re0Codegen *c) {
    re0_buffer_free(&c->output);
}

int re0_codegen_new_label(Re0Codegen *c) { return c->label_counter++; }
int re0_codegen_new_temp(Re0Codegen *c) { return c->temp_counter++; }
int re0_codegen_new_reg(Re0Codegen *c) {
    int r = c->reg_counter % 20 + 1;
    c->reg_counter++;
    return r;
}
