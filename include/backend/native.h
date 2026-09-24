#ifndef RE0_NATIVE_H
#define RE0_NATIVE_H
#include "backend/backend.h"

/* Experimental x86-64 SysV / ELF64 backend. All state belongs to one compile. */
extern Re0Backend re0_backend_native;
bool re0_native_generate(Re0Codegen *c, Re0StmtVec *checked);
#endif
