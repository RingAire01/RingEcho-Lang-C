#ifndef RE0_LAYOUT_H
#define RE0_LAYOUT_H
#include "analysis/model.h"
#include "front/ast.h"
#include "base/error.h"

/* System-layout contract. This manager owns all returned nodes until destroy;
 * model/AST inputs are borrowed and must outlive a query. No global state. */
enum { RE0_LAYOUT_MAX_DEPTH = 128, RE0_LAYOUT_MAX_NODES = 16384,
       RE0_LAYOUT_MAX_FIELDS = 1024, RE0_LAYOUT_MAX_TYPE_TEXT = 65536 };
typedef struct {
    unsigned pointer_size, pointer_align, int128_align;
    size_t max_object_size;
} Re0TargetLayout;
extern const Re0TargetLayout re0_target_x86_64_sysv;

typedef struct Re0Layout Re0Layout;
typedef struct { const char *name; size_t offset; Re0Layout *type; } Re0LayoutField;
typedef struct { const char *name; size_t tag; Re0Layout *payload; } Re0LayoutVariant;
struct Re0Layout {
    Re0TypeKind kind;
    size_t size, align;
    size_t allocation_size;
    bool inhabited;
    const char *name;
    Re0Layout *element;
    size_t length;
    Re0LayoutField *fields;
    size_t field_count;
    Re0LayoutVariant *variants;
    size_t variant_count, payload_offset;
};
typedef struct {
    Re0TargetLayout target;
    Re0SemanticModel *model;
    Re0StmtVec *declarations;
    Re0ErrorList *errors;
    Re0Layout **nodes;
    size_t count, capacity;
    bool failed;
} Re0LayoutManager;

bool re0_layout_init(Re0LayoutManager *m, const Re0TargetLayout *target,
                     Re0SemanticModel *model, Re0StmtVec *declarations, Re0ErrorList *errors);
void re0_layout_destroy(Re0LayoutManager *m);
Re0Layout *re0_layout_type(Re0LayoutManager *m, const Re0Type *type);
Re0Layout *re0_layout_parse(Re0LayoutManager *m, const char *type);
const Re0LayoutField *re0_layout_field(const Re0Layout *type, const char *name);

typedef enum { RE0_ABI_NONE, RE0_ABI_INTEGER, RE0_ABI_SSE, RE0_ABI_MEMORY } Re0AbiClass;
typedef struct { Re0AbiClass words[2]; unsigned word_count; bool indirect; } Re0SysvClass;
/* Classification of value types; argument register assignment is a separate step. */
bool re0_layout_sysv_classify(const Re0Layout *type, Re0SysvClass *result);
#endif
