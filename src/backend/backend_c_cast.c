#include "backend/backend_c_internal.h"

Re0TypeKind c_expr_scalar_kind(Re0Expr *e) {
    if (e && e->resolved_type && e->resolved_type->kind <= RE0_TYPE_STR)
        return e->resolved_type->kind;
    char type[128];
    if (!infer_expr_c_type(e, type, sizeof(type))) return RE0_TYPE_UNKNOWN;
    for (int k = RE0_TYPE_I8; k <= RE0_TYPE_STR; k++)
        if (strcmp(type, reo_type_to_c(re0_type_kind_name((Re0TypeKind)k))) == 0)
            return (Re0TypeKind)k;
    return RE0_TYPE_UNKNOWN;
}

static void write_bits(Re0Buffer *out, Re0TypeKind kind) {
    if (kind == RE0_TYPE_ISIZE || kind == RE0_TYPE_USIZE)
        re0_buffer_write_str(out, "(sizeof(uintptr_t) * CHAR_BIT)");
    else re0_buffer_write_fmt(out, "%zuU", re0_type_sizeof(kind) * 8);
}

static bool c_gen_array_conversion(Re0Codegen *c, Re0Expr *e) {
    Re0Type *result = e->resolved_type;
    Re0Type *source = e->cast.inner ? e->cast.inner->resolved_type : NULL;
    if (!e->cast.checked || !result || result->kind != RE0_TYPE_GENERIC || !source || source->kind != RE0_TYPE_ARRAY)
        return false;
    Re0Type *target = result->generic.args[0];
    if (target->kind != RE0_TYPE_ARRAY || !target->array.inner || !source->array.inner ||
        target->array.inner->kind >= RE0_TYPE_STR || source->array.inner->kind >= RE0_TYPE_STR) return false;
    Re0TypeKind sk = source->array.inner->kind;
    const char *box = re0_type_is_float(sk) ? "float" : sk == RE0_TYPE_BOOL ? "bool" :
        sk == RE0_TYPE_CHAR ? "char" : re0_type_is_signed(sk) ? "signed" : "unsigned";
    Re0TypeKind tk=target->array.inner->kind;
    const char *kind=re0_type_is_float(tk)?"__REO_VALUE_FLOAT":tk==RE0_TYPE_BOOL?"__REO_VALUE_BOOL":
        tk==RE0_TYPE_CHAR?"__REO_VALUE_CHAR":re0_type_is_signed(tk)?"__REO_VALUE_SIGNED":"__REO_VALUE_UNSIGNED";
    const char *field=re0_type_is_float(tk)?"floating":re0_type_is_signed(tk)?"signed_":"unsigned_";
    int temporary = c->temp_counter++;
    char source_type[128];
    if (!infer_expr_c_type(e->cast.inner, source_type, sizeof(source_type))) return false;
    re0_buffer_write_fmt(&c->output, "({ %s __source%d = (", source_type, temporary);
    c_gen_expr(c, e->cast.inner);
    const char *result_type=c_storage_type(result), *target_type=c_storage_type(target);
    re0_buffer_write_fmt(&c->output,"); %s __converted%d={0}; __converted%d.index=-1; ",result_type,temporary,temporary);
    if(source->array.size!=target->array.size)
        re0_buffer_write_fmt(&c->output,"__converted%d.tag=1; __converted%d.error=__REO_CONV_LENGTH; ",temporary,temporary);
    else {
        re0_buffer_write_fmt(&c->output,"for(size_t __ci%d=0; __ci%d<%zu; __ci%d++) { __reo_value __cv%d=__reo_checked_value(__reo_box_%s(__source%d.data[__ci%d]), %s, ",temporary,temporary,target->array.size,temporary,temporary,box,temporary,temporary,kind);
        write_bits(&c->output,tk);
        re0_buffer_write_fmt(&c->output,"); if(__cv%d.error) { __converted%d.tag=1; __converted%d.error=__cv%d.error; __converted%d.index=(int64_t)__ci%d; __converted%d.value=(%s){0}; break; } __converted%d.value.data[__ci%d]=(%s)__cv%d.data.%s; } ",temporary,temporary,temporary,temporary,temporary,temporary,temporary,target_type,temporary,temporary,c_storage_type(target->array.inner),temporary,field);
    }
    re0_buffer_write_fmt(&c->output,"(void)__source%d; __converted%d; })",temporary,temporary);
    return true;
}

int c_gen_cast(Re0Codegen *c, Re0Expr *e) {
    Re0Buffer *out = &c->output;
    if (c_gen_array_conversion(c, e)) return 0;
    Re0TypeKind source = c_expr_scalar_kind(e->cast.inner);
    Re0TypeKind target = e->resolved_type ? e->resolved_type->kind : RE0_TYPE_UNKNOWN;
    if (e->cast.checked && e->resolved_type && e->resolved_type->kind == RE0_TYPE_GENERIC)
        target = e->resolved_type->generic.args[0]->kind;
    const char *ctype = reo_type_to_c(e->cast.target_type);
    if (c->backend != &re0_backend_c && c->backend != &re0_backend_c_freestanding) {
        re0_error_append(c->errors, RE0_ERR_SEMANTIC, e->span, NULL,
                         "conversion runtime is unavailable for this backend");
        c->had_error = true;
        return 0;
    }
    if (source == RE0_TYPE_UNKNOWN || target > RE0_TYPE_STR) {
        if (e->resolved_type && e->cast.inner->resolved_type &&
            re0_type_equal(e->resolved_type, e->cast.inner->resolved_type))
            return c_gen_expr(c, e->cast.inner);
        re0_error_append(c->errors, RE0_ERR_SEMANTIC, e->span, NULL,
                         "cannot lower conversion with unresolved operand type");
        c->had_error = true;
        return 0;
    }
    if (e->cast.checked) {
        const char *box = source == RE0_TYPE_STR ? "string" : source == RE0_TYPE_BOOL ? "bool" :
            source == RE0_TYPE_CHAR ? "char" : re0_type_is_float(source) ? "float" :
            re0_type_is_signed(source) ? "signed" : "unsigned";
        re0_buffer_write_fmt(out, "__reo_try_%s(__reo_box_%s(", re0_type_kind_name(target), box);
        c_gen_expr(c, e->cast.inner);
        re0_buffer_write_str(out, "))");
        return 0;
    }
    if (source == target && target != RE0_TYPE_CHAR) return c_gen_expr(c, e->cast.inner);
    bool source_float = re0_type_is_float(source);
    bool signed_target = re0_type_is_signed(target) && target != RE0_TYPE_CHAR;
    re0_buffer_write_fmt(out, "((%s)(", ctype);
    if (target == RE0_TYPE_STR) {
        re0_buffer_write_str(out, "__reo_conv_require_string(");
        const char *helper = source_float ? "__reo_conv_float_string" :
            source == RE0_TYPE_BOOL ? NULL :
            source == RE0_TYPE_CHAR ? "__reo_conv_char_string" :
            re0_type_is_signed(source) ? "__reo_conv_signed_string" : "__reo_conv_unsigned_string";
        if (helper) re0_buffer_write_fmt(out, "%s(", helper);
        c_gen_expr(c, e->cast.inner);
        re0_buffer_write_str(out, helper ? ")" : " ? \"true\" : \"false\"");
        re0_buffer_write_char(out, ')');
    } else if (source == RE0_TYPE_STR) {
        if (target == RE0_TYPE_BOOL || target == RE0_TYPE_CHAR || re0_type_is_float(target)) {
            if (target == RE0_TYPE_F32) re0_buffer_write_str(out, "__reo_conv_f32(");
            re0_buffer_write_fmt(out, "%s(", target == RE0_TYPE_BOOL ? "__reo_conv_string_bool" :
                target == RE0_TYPE_CHAR ? "__reo_conv_string_char" : "__reo_conv_string_float");
            c_gen_expr(c, e->cast.inner);
            re0_buffer_write_str(out, target == RE0_TYPE_F32 ? "))" : ")");
        } else {
            if (signed_target) re0_buffer_write_str(out, "__reo_conv_signed(");
            re0_buffer_write_str(out, "__reo_conv_string_integer(");
            c_gen_expr(c, e->cast.inner);
            re0_buffer_write_str(out, ", "); write_bits(out, target);
            re0_buffer_write_fmt(out, ", %s)", signed_target ? "true" : "false");
            if (signed_target) { re0_buffer_write_str(out, ", "); write_bits(out, target); re0_buffer_write_char(out, ')'); }
        }
    } else if (target == RE0_TYPE_BOOL) {
        re0_buffer_write_char(out, '('); c_gen_expr(c, e->cast.inner); re0_buffer_write_str(out, ") != 0");
    } else if (re0_type_is_float(target)) {
        if (target == RE0_TYPE_F32 && source_float) re0_buffer_write_str(out, "__reo_conv_f32(");
        c_gen_expr(c, e->cast.inner);
        if (target == RE0_TYPE_F32 && source_float) re0_buffer_write_char(out, ')');
    } else if (source_float) {
        re0_buffer_write_fmt(out, "%s(", signed_target ? "__reo_conv_float_signed" : "__reo_conv_float_unsigned");
        c_gen_expr(c, e->cast.inner);
        re0_buffer_write_str(out, ", "); write_bits(out, target); re0_buffer_write_char(out, ')');
    } else {
        if (signed_target) re0_buffer_write_str(out, "__reo_conv_signed((__reo_u128)(");
        c_gen_expr(c, e->cast.inner);
        if (signed_target) { re0_buffer_write_str(out, "), "); write_bits(out, target); re0_buffer_write_char(out, ')'); }
    }
    re0_buffer_write_str(out, "))");
    return 0;
}
