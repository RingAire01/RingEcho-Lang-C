#ifndef RE0_RUNTIME_C_H
#define RE0_RUNTIME_C_H

#include "base/buffer.h"

/* Emits the embedded C runtime library (safe arithmetic, string/vec/array
 * helpers, GC glue, task runtime) into `out` as the prelude of a generated
 * program. Called by the C backend's c_begin(). */
void re0_runtime_c_emit(Re0Buffer *out);

#endif
