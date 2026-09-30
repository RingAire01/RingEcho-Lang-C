#include "backend/native_internal.h"
#include <string.h>

/* ELF32 little-endian, i386. Relocations use SHT_REL, so the addend lives in
 * the relocated field (a PC-relative call is -4). Constants are file-format
 * values, not host ABI values. */
enum { ELF_HEADER = 52, ELF_SECTION = 40, ELF_SYMBOL = 16, ELF_REL = 8,
       SECTION_COUNT = 7, S_TEXT = 1, S_RELA = 2, S_SYM = 3, S_STR = 4,
       S_NAMES = 5, S_STACK = 6, EM_386 = 3, R_386_PC32 = 2,
       EM_ARM = 40, R_ARM_CALL = 28 };

static void align4(Re0Buffer *b) {
    unsigned padding = (unsigned)((4 - b->len % 4) % 4);
    for (unsigned i = 0; i < padding; i++) n_put(b, 0, 1);
}
static void symbol(Re0Buffer *b, size_t name, bool defined, size_t value, size_t size) {
    n_put(b, name, 4); n_put(b, value, 4); n_put(b, size, 4);
    n_put(b, 0x12, 1); n_put(b, 0, 1); n_put(b, defined ? S_TEXT : 0, 2);
}
static void section(Re0Buffer *b, size_t name, unsigned type, unsigned flags,
                    size_t offset, size_t size, unsigned link, unsigned info,
                    unsigned alignment, unsigned entry_size) {
    n_put(b, name, 4); n_put(b, type, 4); n_put(b, flags, 4); n_put(b, 0, 4);
    n_put(b, offset, 4); n_put(b, size, 4); n_put(b, link, 4); n_put(b, info, 4);
    n_put(b, alignment, 4); n_put(b, entry_size, 4);
}

bool n_elf32(NModule *m) {
    Re0Buffer *b = &m->codegen->output, strings, symbols;
    bool arm = m->target && m->target->arch == RE0_ARCH_ARM;
    unsigned machine = arm ? EM_ARM : EM_386;
    unsigned reloc_type = arm ? R_ARM_CALL : R_386_PC32;
    /* REL addend lives in the field: -4 for i386 `call rel32`, and the ARM BL
     * instruction with imm24 = -2 to account for the PC bias of 8. */
    uint32_t addend = arm ? UINT32_C(0xEBFFFFFE) : (uint32_t)(UINT32_MAX - 3);
    re0_buffer_init(&strings); re0_buffer_init(&symbols);
    n_put(&strings, 0, 1);
    for (unsigned i = 0; i < ELF_SYMBOL; i++) n_put(&symbols, 0, 1);
    for (size_t i = 0; i < m->count; i++) {
        NFunction *f = &m->functions[i];
        symbol(&symbols, strings.len, f->ast != NULL, f->offset, f->size);
        re0_buffer_write_n(&strings, f->name, strlen(f->name) + 1);
    }
    if (m->codegen->emit_main) {
        symbol(&symbols, strings.len, true, m->entry_offset, m->entry_size);
        re0_buffer_write_n(&strings, "_start", sizeof("_start"));
    }
    for (unsigned i = 0; i < ELF_HEADER; i++) n_put(b, 0, 1);
    n_patch(b, 0, UINT64_C(0x00010101464c457f), 8); /* ELF32, little endian */
    n_patch(b, 16, 1, 2); n_patch(b, 18, machine, 2); n_patch(b, 20, 1, 4);
    n_patch(b, 36, arm ? 0x05000000u : 0, 4); /* EF_ARM_EABI_VER5 for ARM */
    if (m->codegen->emit_main) n_patch(b, 24, m->entry_offset, 4);
    n_patch(b, 40, ELF_HEADER, 2); n_patch(b, 46, ELF_SECTION, 2);
    n_patch(b, 48, SECTION_COUNT, 2); n_patch(b, 50, S_NAMES, 2);
    size_t text = b->len; re0_buffer_write_n(b, m->text.data, m->text.len); align4(b);
    for (size_t i = 0; i < m->reloc_count; i++) {
        NReloc *r = &m->relocs[i];
        if (r->symbol >= m->count || r->offset > m->text.len || m->text.len - r->offset < 4) {
            n_error(m, RE0_SPAN_ZERO, "invalid ELF relocation"); break;
        }
        n_patch(b, text + r->offset, addend, 4); /* implicit addend */
    }
    size_t rela = b->len;
    for (size_t i = 0; i < m->reloc_count; i++) {
        NReloc *r = &m->relocs[i];
        n_put(b, r->offset, 4);
        n_put(b, ((uint32_t)(r->symbol + 1) << 8) | reloc_type, 4);
    }
    size_t sym = b->len; re0_buffer_write_n(b, symbols.data, symbols.len);
    size_t str = b->len; re0_buffer_write_n(b, strings.data, strings.len);
    static const char names[] = "\0.text\0.rel.text\0.symtab\0.strtab\0.shstrtab\0.note.GNU-stack\0";
    size_t names_offset = b->len; re0_buffer_write_n(b, names, sizeof(names)); align4(b);
    size_t sections = b->len;
    section(b, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    section(b, 1, 1, 6, text, m->text.len, 0, 0, 16, 0);
    section(b, 7, 9, 0, rela, m->reloc_count * ELF_REL, S_SYM, S_TEXT, 4, ELF_REL);
    section(b, 17, 2, 0, sym, symbols.len, S_STR, 1, 4, ELF_SYMBOL);
    section(b, 25, 3, 0, str, strings.len, 0, 0, 1, 0);
    section(b, 33, 3, 0, names_offset, sizeof(names), 0, 0, 1, 0);
    section(b, 43, 1, 0, names_offset, 0, 0, 0, 1, 0);
    n_patch(b, 32, sections, 4);
    if (re0_buffer_failed(&strings) || re0_buffer_failed(&symbols))
        n_error(m, RE0_SPAN_ZERO, "cannot allocate ELF tables");
    re0_buffer_free(&strings); re0_buffer_free(&symbols);
    return !m->codegen->had_error && !re0_buffer_failed(b);
}
