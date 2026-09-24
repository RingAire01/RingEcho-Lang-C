#include "backend/native_internal.h"
#include <string.h>

/* ELF64 constants are specified by the file format, not by the host ABI.
 * Encode fields explicitly instead of serializing host structs. */
enum { ELF_HEADER = 64, ELF_SECTION = 64, ELF_SYMBOL = 24, ELF_RELA = 24,
       SECTION_COUNT = 7, S_TEXT = 1, S_RELA = 2, S_SYM = 3, S_STR = 4,
       S_NAMES = 5, S_STACK = 6, R_X86_64_PLT32 = 4 };

static void align8(Re0Buffer *b) {
    /* Bounded even if buffer growth fails and len stops advancing. */
    unsigned padding = (unsigned)((8 - b->len % 8) % 8);
    for (unsigned i = 0; i < padding; i++) n_put(b, 0, 1);
}
static void symbol(Re0Buffer *b, size_t name, bool defined, size_t value, size_t size) {
    n_put(b, name, 4); n_put(b, 0x12, 1); /* STB_GLOBAL | STT_FUNC */
    n_put(b, 0, 1); n_put(b, defined ? S_TEXT : 0, 2);
    n_put(b, value, 8); n_put(b, size, 8);
}
static void section(Re0Buffer *b, size_t name, unsigned type, unsigned flags,
                    size_t offset, size_t size, unsigned link, unsigned info,
                    unsigned alignment, unsigned entry_size) {
    n_put(b, name, 4); n_put(b, type, 4); n_put(b, flags, 8); n_put(b, 0, 8);
    n_put(b, offset, 8); n_put(b, size, 8); n_put(b, link, 4); n_put(b, info, 4);
    n_put(b, alignment, 8); n_put(b, entry_size, 8);
}

bool n_elf(NModule *m) {
    Re0Buffer *b = &m->codegen->output, strings, symbols;
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
    n_patch(b, 0, UINT64_C(0x00010102464c457f), 8); /* ELF64, little endian */
    n_patch(b, 16, 1, 2); n_patch(b, 18, 62, 2); n_patch(b, 20, 1, 4); /* REL, X86_64, CURRENT */
    n_patch(b, 52, ELF_HEADER, 2); n_patch(b, 58, ELF_SECTION, 2);
    n_patch(b, 60, SECTION_COUNT, 2); n_patch(b, 62, S_NAMES, 2);
    size_t text = b->len; re0_buffer_write_n(b, m->text.data, m->text.len); align8(b);
    size_t rela = b->len;
    for (size_t i = 0; i < m->reloc_count; i++) {
        NReloc *r = &m->relocs[i];
        if (r->symbol >= m->count || r->offset > m->text.len || m->text.len - r->offset < 4) {
            n_error(m, RE0_SPAN_ZERO, "invalid ELF relocation"); break;
        }
        n_put(b, r->offset, 8);
        n_put(b, ((uint64_t)(r->symbol + 1) << 32) | R_X86_64_PLT32, 8);
        n_put(b, UINT64_MAX - 3, 8); /* explicit addend -4 */
    }
    size_t sym = b->len; re0_buffer_write_n(b, symbols.data, symbols.len);
    size_t str = b->len; re0_buffer_write_n(b, strings.data, strings.len);
    static const char names[] = "\0.text\0.rela.text\0.symtab\0.strtab\0.shstrtab\0.note.GNU-stack\0";
    size_t names_offset = b->len; re0_buffer_write_n(b, names, sizeof(names)); align8(b);
    size_t sections = b->len;
    section(b, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    section(b, 1, 1, 6, text, m->text.len, 0, 0, 16, 0);
    section(b, 7, 4, 0, rela, m->reloc_count * ELF_RELA, S_SYM, S_TEXT, 8, ELF_RELA);
    section(b, 18, 2, 0, sym, symbols.len, S_STR, 1, 8, ELF_SYMBOL);
    section(b, 26, 3, 0, str, strings.len, 0, 0, 1, 0);
    section(b, 34, 3, 0, names_offset, sizeof(names), 0, 0, 1, 0);
    section(b, 44, 1, 0, names_offset, 0, 0, 0, 1, 0);
    n_patch(b, 40, sections, 8);
    if (re0_buffer_failed(&strings) || re0_buffer_failed(&symbols))
        n_error(m, RE0_SPAN_ZERO, "cannot allocate ELF tables");
    re0_buffer_free(&strings); re0_buffer_free(&symbols);
    return !m->codegen->had_error && !re0_buffer_failed(b);
}
