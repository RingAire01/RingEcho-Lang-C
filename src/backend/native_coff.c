#include "backend/native_internal.h"
#include <stdio.h>
#include <string.h>

/* COFF (PE/COFF) relocatable object for Windows x86-64, x86 and arm64. One
 * .text section, external symbols, and PC-relative branch relocations. x86
 * symbols keep the C leading underscore; x86-64 and arm64 do not. */

#define IMAGE_FILE_MACHINE_AMD64 UINT16_C(0x8664)
#define IMAGE_FILE_MACHINE_I386  UINT16_C(0x014C)
#define IMAGE_FILE_MACHINE_ARM64 UINT16_C(0xAA64)
enum { COFF_HEADER = 20, COFF_SECTION = 40, COFF_SYMBOL = 18, COFF_RELOC = 10,
       REL_I386_REL32 = 0x0014, REL_AMD64_REL32 = 0x0004, REL_ARM64_BRANCH26 = 0x0003,
       SCN_TEXT = 0x60500020 };

static void coff_name(Re0Buffer *names, const char *name, unsigned char out[8]) {
    memset(out, 0, 8);
    if (strlen(name) <= 8) { memcpy(out, name, strlen(name)); return; }
    uint32_t offset = (uint32_t)names->len + 4; /* string table size prefix */
    re0_buffer_write_n(names, name, strlen(name) + 1);
    out[0] = out[1] = out[2] = out[3] = 0;
    out[4] = (unsigned char)(offset & 0xFF);
    out[5] = (unsigned char)((offset >> 8) & 0xFF);
    out[6] = (unsigned char)((offset >> 16) & 0xFF);
    out[7] = (unsigned char)((offset >> 24) & 0xFF);
}

bool n_coff(NModule *m) {
    Re0Buffer *b = &m->codegen->output, strings, symbols, relocs;
    bool arm64 = m->target->arch == RE0_ARCH_AARCH64;
    bool x86 = m->target->arch == RE0_ARCH_X86;
    uint16_t machine = arm64 ? IMAGE_FILE_MACHINE_ARM64 : x86 ? IMAGE_FILE_MACHINE_I386 : IMAGE_FILE_MACHINE_AMD64;
    uint16_t rel_type = arm64 ? REL_ARM64_BRANCH26 : x86 ? REL_I386_REL32 : REL_AMD64_REL32;
    re0_buffer_init(&strings); re0_buffer_init(&symbols); re0_buffer_init(&relocs);
    n_put(&strings, 0, 4);                     /* string table length prefix */

    size_t *symidx = calloc(N_MAX_FUNCTIONS, sizeof(*symidx));
    if (!symidx) { n_error(m, RE0_SPAN_ZERO, "cannot allocate COFF symbol map"); re0_buffer_free(&strings); re0_buffer_free(&symbols); re0_buffer_free(&relocs); return false; }
    size_t nsyms = 0;
    for (size_t i = 0; i < m->count; i++) {
        NFunction *f = &m->functions[i];
        bool defined = f->ast != NULL;
        symidx[i] = nsyms++;
        char name[256];
        snprintf(name, sizeof(name), x86 ? "_%s" : "%s", f->name);
        unsigned char nm[8]; coff_name(&strings, name, nm);
        re0_buffer_write_n(&symbols, (const char *)nm, 8);
        n_put(&symbols, defined ? f->offset : 0, 4);
        n_put(&symbols, defined ? 1 : 0, 2);   /* SectionNumber */
        n_put(&symbols, 0x20, 2);              /* Type: function */
        n_put(&symbols, 2, 1);                 /* StorageClass: external */
        n_put(&symbols, 0, 1);                 /* NumberOfAuxSymbols */
    }
    if (m->codegen->emit_main) {
        char name[16];
        snprintf(name, sizeof(name), x86 ? "_start" : "start");
        unsigned char nm[8]; coff_name(&strings, name, nm);
        re0_buffer_write_n(&symbols, (const char *)nm, 8);
        n_put(&symbols, m->entry_offset, 4);
        n_put(&symbols, 1, 2); n_put(&symbols, 0x20, 2); n_put(&symbols, 2, 1); n_put(&symbols, 0, 1);
        nsyms++;
    }
    for (size_t i = 0; i < m->reloc_count; i++) {
        NReloc *r = &m->relocs[i];
        if (r->symbol >= m->count) { n_error(m, RE0_SPAN_ZERO, "invalid COFF relocation"); break; }
        n_put(&relocs, r->offset, 4);
        n_put(&relocs, symidx[r->symbol], 4);
        n_put(&relocs, rel_type, 2);
    }

    size_t text_off = COFF_HEADER + COFF_SECTION;
    size_t reloc_off = text_off + m->text.len;
    size_t sym_off = reloc_off + relocs.len;

    n_put(b, machine, 2);
    n_put(b, 1, 2);                            /* NumberOfSections */
    n_put(b, 0, 4);                            /* TimeDateStamp */
    n_put(b, sym_off, 4);
    n_put(b, nsyms, 4);
    n_put(b, 0, 2);                            /* SizeOfOptionalHeader */
    n_put(b, 0, 2);                            /* Characteristics */

    /* .text section header */
    char sh[8] = { '.', 't', 'e', 'x', 't', 0, 0, 0 };
    re0_buffer_write_n(b, sh, 8);
    n_put(b, 0, 4);                            /* VirtualSize */
    n_put(b, 0, 4);                            /* VirtualAddress */
    n_put(b, m->text.len, 4);                  /* SizeOfRawData */
    n_put(b, text_off, 4);                     /* PointerToRawData */
    n_put(b, reloc_off, 4);                    /* PointerToRelocations */
    n_put(b, 0, 4);                            /* PointerToLinenumbers */
    n_put(b, m->reloc_count, 2);               /* NumberOfRelocations */
    n_put(b, 0, 2);                            /* NumberOfLinenumbers */
    n_put(b, SCN_TEXT, 4);

    re0_buffer_write_n(b, m->text.data, m->text.len);
    re0_buffer_write_n(b, relocs.data, relocs.len);
    re0_buffer_write_n(b, symbols.data, symbols.len);
    n_patch(&strings, 0, (uint32_t)strings.len, 4);
    re0_buffer_write_n(b, strings.data, strings.len);

    if (re0_buffer_failed(&strings) || re0_buffer_failed(&symbols) || re0_buffer_failed(&relocs))
        n_error(m, RE0_SPAN_ZERO, "cannot allocate COFF tables");
    free(symidx);
    re0_buffer_free(&strings); re0_buffer_free(&symbols); re0_buffer_free(&relocs);
    return !m->codegen->had_error && !re0_buffer_failed(b);
}
