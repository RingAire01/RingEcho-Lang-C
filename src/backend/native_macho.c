#include "backend/native_internal.h"
#include <stdio.h>
#include <string.h>

/* Mach-O 64-bit relocatable object (MH_OBJECT) for macOS x86-64 and arm64.
 * One __TEXT,__text section, a symbol table and the matching dynamic symbol
 * table. Relocations are external branch fixups (X86_64_RELOC_BRANCH /
 * ARM64_RELOC_BRANCH26, both type 2). Symbol names use the Mach-O leading
 * underscore. */

#define MH_MAGIC_64    UINT32_C(0xFEEDFACF)
#define CPU_TYPE_X86_64 UINT32_C(0x01000007)
#define CPU_TYPE_ARM64  UINT32_C(0x0100000C)
enum { MH_OBJECT = 1, LC_SEGMENT_64 = 0x19, LC_SYMTAB = 0x2, LC_DYSYMTAB = 0xB,
       SEG_CMD = 72, SEC_CMD = 80, SYMTAB_CMD = 24, DYSYMTAB_CMD = 80,
       NLIST = 16, RELOC = 8, HEADER = 32 };

static void cstr16(Re0Buffer *b, const char *s) {
    char buf[16] = {0};
    for (unsigned i = 0; s[i] && i < 15; i++) buf[i] = s[i];
    re0_buffer_write_n(b, buf, sizeof(buf));
}
static void nlist(Re0Buffer *b, size_t strx, unsigned type, unsigned sect, uint64_t value) {
    n_put(b, strx, 4); n_put(b, type, 1); n_put(b, sect, 1); n_put(b, 0, 2); n_put(b, value, 8);
}

bool n_macho(NModule *m) {
    Re0Buffer *b = &m->codegen->output, strings, symbols, relocs;
    bool aarch64 = m->target->arch == RE0_ARCH_AARCH64;
    re0_buffer_init(&strings); re0_buffer_init(&symbols); re0_buffer_init(&relocs);
    n_put(&strings, 0, 1);

    /* Symbol indices: external-defined functions, _start, then undefined. */
    size_t *symidx = calloc(N_MAX_FUNCTIONS, sizeof(*symidx));
    if (!symidx) { n_error(m, RE0_SPAN_ZERO, "cannot allocate Mach-O symbol map"); re0_buffer_free(&strings); re0_buffer_free(&symbols); re0_buffer_free(&relocs); return false; }
    size_t nsyms = 0;
    for (size_t i = 0; i < m->count; i++) {
        NFunction *f = &m->functions[i];
        if (!f->ast) continue;
        symidx[i] = nsyms++;
        char name[256];
        snprintf(name, sizeof(name), "_%s", f->name);
        nlist(&symbols, strings.len, 0x0F, 1, f->offset);
        re0_buffer_write_n(&strings, name, strlen(name) + 1);
    }
    if (m->codegen->emit_main) {
        nlist(&symbols, strings.len, 0x0F, 1, m->entry_offset);
        re0_buffer_write_n(&strings, "_start", sizeof("_start"));
        nsyms++;
    }
    size_t nextdef = nsyms;
    for (size_t i = 0; i < m->count; i++) {
        NFunction *f = &m->functions[i];
        if (f->ast) continue;
        symidx[i] = nsyms++;
        char name[256];
        snprintf(name, sizeof(name), "_%s", f->name);
        nlist(&symbols, strings.len, 0x01, 0, 0);
        re0_buffer_write_n(&strings, name, strlen(name) + 1);
    }
    size_t nundef = nsyms - nextdef;

    /* Relocations (external branch fixups). */
    for (size_t i = 0; i < m->reloc_count; i++) {
        NReloc *r = &m->relocs[i];
        if (r->symbol >= m->count) { n_error(m, RE0_SPAN_ZERO, "invalid Mach-O relocation"); break; }
        n_put(&relocs, r->offset, 4);
        uint32_t info = (uint32_t)(symidx[r->symbol] & 0xFFFFFF)
                      | (1u << 24)   /* r_pcrel */
                      | (2u << 25)   /* r_length = 4 bytes */
                      | (1u << 27)   /* r_extern */
                      | (2u << 28);  /* r_type = BRANCH / BRANCH26 */
        n_put(&relocs, info, 4);
    }

    size_t sizeofcmds = SEG_CMD + SEC_CMD + SYMTAB_CMD + DYSYMTAB_CMD;
    size_t text_off = HEADER + sizeofcmds;
    size_t reloc_off = text_off + m->text.len;
    size_t sym_off = reloc_off + relocs.len;
    size_t str_off = sym_off + symbols.len;

    n_put(b, MH_MAGIC_64, 4);
    n_put(b, aarch64 ? CPU_TYPE_ARM64 : CPU_TYPE_X86_64, 4);
    n_put(b, aarch64 ? 0 : 3, 4);              /* cpusubtype */
    n_put(b, MH_OBJECT, 4);
    n_put(b, 3, 4);                            /* ncmds */
    n_put(b, sizeofcmds, 4);
    n_put(b, 0, 4);                            /* flags */
    n_put(b, 0, 4);                            /* reserved */

    /* LC_SEGMENT_64 __TEXT,__text */
    n_put(b, LC_SEGMENT_64, 4); n_put(b, SEG_CMD + SEC_CMD, 4);
    cstr16(b, "__TEXT");
    n_put(b, 0, 8); n_put(b, m->text.len, 8);
    n_put(b, text_off, 8); n_put(b, m->text.len, 8);
    n_put(b, 7, 4); n_put(b, 5, 4); n_put(b, 1, 4); n_put(b, 0, 4); /* maxprot rwx, initprot r-x, 1 sect */
    cstr16(b, "__text"); cstr16(b, "__TEXT");
    n_put(b, 0, 8);                            /* addr */
    n_put(b, m->text.len, 8);                  /* size */
    n_put(b, text_off, 4);                     /* offset */
    n_put(b, 2, 4);                            /* align = 2^2 */
    n_put(b, reloc_off, 4);                    /* reloff */
    n_put(b, m->reloc_count, 4);               /* nreloc */
    n_put(b, 0x80000400u, 4);                  /* S_ATTR_PURE|SOME_INSTRUCTIONS */
    n_put(b, 0, 4); n_put(b, 0, 4); n_put(b, 0, 4);

    /* LC_SYMTAB */
    n_put(b, LC_SYMTAB, 4); n_put(b, SYMTAB_CMD, 4);
    n_put(b, sym_off, 4); n_put(b, nsyms, 4); n_put(b, str_off, 4); n_put(b, strings.len, 4);

    /* LC_DYSYMTAB */
    n_put(b, LC_DYSYMTAB, 4); n_put(b, DYSYMTAB_CMD, 4);
    n_put(b, 0, 4); n_put(b, 0, 4);            /* ilocalsym, nlocalsym */
    n_put(b, 0, 4); n_put(b, nextdef, 4);      /* iextdefsym, nextdefsym */
    n_put(b, nextdef, 4); n_put(b, nundef, 4); /* iundefsym, nundefsym */
    for (int i = 0; i < 12; i++) n_put(b, 0, 4); /* tocoff..nlocrel */

    re0_buffer_write_n(b, m->text.data, m->text.len);
    re0_buffer_write_n(b, relocs.data, relocs.len);
    re0_buffer_write_n(b, symbols.data, symbols.len);
    re0_buffer_write_n(b, strings.data, strings.len);

    if (re0_buffer_failed(&strings) || re0_buffer_failed(&symbols) || re0_buffer_failed(&relocs))
        n_error(m, RE0_SPAN_ZERO, "cannot allocate Mach-O tables");
    free(symidx);
    re0_buffer_free(&strings); re0_buffer_free(&symbols); re0_buffer_free(&relocs);
    return !m->codegen->had_error && !re0_buffer_failed(b);
}
