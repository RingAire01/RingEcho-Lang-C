#ifndef RE0_NATIVE_BUILD_H
#define RE0_NATIVE_BUILD_H
#include "exec/build.h"
#include "base/buffer.h"
bool re0_native_build(Re0Build *build, const Re0Buffer *object,
                      const char *output, bool emit_object);
#endif
