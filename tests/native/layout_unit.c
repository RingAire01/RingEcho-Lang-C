#include "analysis/layout.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "layout check failed at %s:%d: %s\n", __FILE__, __LINE__, #x); return false; } } while (0)

static Re0StructFieldDecl packet_fields[] = {{"tag", "u8"}, {"items", "[i32;3]"}, {"value", "f64"}};
static Re0StructFieldDecl pair_fields[] = {{"first", "T"}, {"rest", "[T;3]"}};
static char *pair_params[] = {"T"};
static Re0StructFieldDecl node_fields[] = {{"next", "&Node"}, {"value", "u32"}};
static Re0StructFieldDecl recursive_fields[] = {{"value", "Recursive"}};
static char *small[] = {"u8"}, *pair[] = {"u8", "u64"}, *wide[] = {"i128"};
static Re0EnumVariantDecl variants[] = {{"Empty", NULL, 0}, {"Small", small, 1}, {"Pair", pair, 2}, {"Wide", wide, 1}};
static Re0Stmt packet = {.kind = STMT_STRUCT, .struct_decl = {.name = "Packet", .fields = packet_fields, .field_count = 3}};
static Re0Stmt generic = {.kind = STMT_STRUCT, .struct_decl = {.name = "Group", .fields = pair_fields, .field_count = 2, .type_params = pair_params, .type_param_count = 1}};
static Re0Stmt linked = {.kind = STMT_STRUCT, .struct_decl = {.name = "Node", .fields = node_fields, .field_count = 2}};
static Re0Stmt recursive = {.kind = STMT_STRUCT, .struct_decl = {.name = "Recursive", .fields = recursive_fields, .field_count = 1}};
static Re0Stmt typed_enum = {.kind = STMT_ENUM, .enum_decl = {.name = "Message", .variants = variants, .variant_count = 4}};
static Re0Stmt alias = {.kind = STMT_TYPE_ALIAS, .type_alias = {.name = "Alias", .target = "Group<u16>"}};
static Re0Stmt cycle = {.kind = STMT_TYPE_ALIAS, .type_alias = {.name = "Cycle", .target = "Cycle"}};
static Re0Stmt *definitions[] = {&packet, &generic, &linked, &recursive, &typed_enum, &alias, &cycle};
static Re0StmtVec declarations = {.data = definitions, .len = sizeof(definitions) / sizeof(definitions[0]), .cap = sizeof(definitions) / sizeof(definitions[0])};

static bool primitives(Re0LayoutManager *m) {
    const struct { const char *name; size_t size, align; } cases[] = {
        {"i8",1,1},{"u8",1,1},{"i16",2,2},{"u16",2,2},{"i32",4,4},{"u32",4,4},
        {"i64",8,8},{"u64",8,8},{"i128",16,16},{"u128",16,16},{"isize",8,8},{"usize",8,8},
        {"f32",4,4},{"f64",8,8},{"bool",1,1},{"char",1,1},{"str",8,8},{"ptr",8,8},
        {"&Node",8,8},{"&mut Node",8,8},{"fn(i32,u8)->f64",8,8},{"fn ( i32 , u8 ) -> f64",8,8},
        {"( i64 )",8,8},{"unit",0,1},{"never",0,1},
        {"[u16;3]",6,2},{"[i128;0]",0,16},{"[[u8;3];4]",12,1},{"[[u8;3]]",16,8},
        {"Vec<Node>",8,8},{"(u8,u64,u16)",24,8},{"Group<u16>",8,2},
        {"Group<Group<u16>>",32,2},{"Alias",8,2},{"Node",16,8},
        {"Option<u8>",16,8},{"Option<i128>",32,16},{"Result<f32,u16>",16,8}
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        Re0Layout *t = re0_layout_parse(m, cases[i].name);
        if (!t || t->size != cases[i].size || t->align != cases[i].align) {
            fprintf(stderr, "unexpected layout for %s\n", cases[i].name); return false;
        }
    }
    Re0Layout *never = re0_layout_parse(m, "never");
    Re0Layout *empty = re0_layout_parse(m, "[never;0]");
    Re0Layout *nonempty = re0_layout_parse(m, "[never;1]");
    REQUIRE(never && !never->inhabited && empty && empty->inhabited && nonempty && !nonempty->inhabited);
    return true;
}

static bool c_layout_and_abi(Re0LayoutManager *m) {
    struct Packet { uint8_t tag; int32_t items[3]; double value; };
    Re0Layout *t = re0_layout_parse(m, "Packet");
    REQUIRE(t && t->size == sizeof(struct Packet) && t->align == _Alignof(struct Packet));
    REQUIRE(re0_layout_field(t, "items")->offset == offsetof(struct Packet, items));
    REQUIRE(re0_layout_field(t, "value")->offset == offsetof(struct Packet, value));
    Re0Layout *e = re0_layout_parse(m, "Message");
    REQUIRE(e && e->size == 32 && e->align == 16 && e->payload_offset == 16);
    REQUIRE(e->variant_count == 4 && e->variants[2].payload->fields[1].offset == 8);
    const struct { const char *name; Re0AbiClass a, b; bool indirect; } cases[] = {
        {"i128", RE0_ABI_INTEGER, RE0_ABI_INTEGER, false},
        {"f64", RE0_ABI_SSE, RE0_ABI_NONE, false},
        {"(f32,f32)", RE0_ABI_SSE, RE0_ABI_NONE, false},
        {"(f64,i64)", RE0_ABI_SSE, RE0_ABI_INTEGER, false},
        {"(u8,f32)", RE0_ABI_INTEGER, RE0_ABI_NONE, false},
        {"[f64;2]", RE0_ABI_SSE, RE0_ABI_SSE, false},
        {"[u8;17]", RE0_ABI_MEMORY, RE0_ABI_NONE, true},
        {"Vec<i64>", RE0_ABI_INTEGER, RE0_ABI_NONE, false},
        {"Result<f64,i64>", RE0_ABI_MEMORY, RE0_ABI_NONE, true}
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        Re0SysvClass abi;
        REQUIRE(re0_layout_sysv_classify(re0_layout_parse(m, cases[i].name), &abi));
        REQUIRE(abi.words[0] == cases[i].a && abi.words[1] == cases[i].b && abi.indirect == cases[i].indirect);
    }
    return true;
}

static bool rejection_cases(void) {
    const char *cases[] = {"unknown", "T", "Group", "Group<i32,u32>", "Recursive", "Cycle", "[u64;18446744073709551615]"};
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        Re0LayoutManager m;
        REQUIRE(re0_layout_init(&m, &re0_target_x86_64_sysv, NULL, &declarations, NULL));
        bool rejected = !re0_layout_parse(&m, cases[i]) && m.failed;
        re0_layout_destroy(&m);
        REQUIRE(rejected);
    }
    const char *bad[] = {"[u8;-1]", "[u8;18446744073709551616]", "[u8;2;3]", "[u8;3x]", "[[u8;2]",
                        "(i32,,u8)", "fn(i32)garbage", "fn(i32)->[u8;-1]", "(i32,[u8;-1])"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        Re0Type *t = re0_type_parse(bad[i]);
        bool rejected = t == NULL;
        re0_type_free_tree(t);
        REQUIRE(rejected);
    }
    char nested[300]; memset(nested, '&', sizeof(nested) - 3);
    memcpy(nested + sizeof(nested) - 3, "u8", 3);
    Re0Type *t = re0_type_parse(nested);
    bool rejected = t == NULL;
    re0_type_free_tree(t);
    REQUIRE(rejected);
    char functions[1603];
    for (unsigned i = 0; i < 200; i++) memcpy(functions + 8 * i, "fn()->  ", 8);
    memcpy(functions + 1600, "u8", 3);
    t = re0_type_parse(functions);
    rejected = t == NULL;
    re0_type_free_tree(t);
    REQUIRE(rejected);
    Re0TargetLayout limited = re0_target_x86_64_sysv;
    limited.max_object_size = 16;
    Re0LayoutManager manager;
    REQUIRE(re0_layout_init(&manager, &limited, NULL, &declarations, NULL));
    Re0Layout *exact = re0_layout_parse(&manager, "(i128,)");
    bool exact_ok = exact && exact->size == 16;
    re0_layout_destroy(&manager);
    REQUIRE(exact_ok);
    return true;
}

int main(void) {
    Re0LayoutManager m;
    if (!re0_layout_init(&m, &re0_target_x86_64_sysv, NULL, &declarations, NULL)) return 1;
    bool ok = primitives(&m) && c_layout_and_abi(&m);
    re0_layout_destroy(&m);
    if (!ok || !rejection_cases()) return 1;
    puts("system type layouts and SysV classification passed");
    return 0;
}
