#ifndef RE0_LINT_H
#define RE0_LINT_H
#include "front/ast.h"
#include "base/error.h"

void re0_lint_run(Re0StmtVec *stmts, Re0ErrorList *errors);

#endif
