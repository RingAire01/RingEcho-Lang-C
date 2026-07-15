#ifndef RE0_LINT_H
#define RE0_LINT_H
#include "ast.h"
#include "error.h"

void re0_lint_run(Re0StmtVec *stmts, Re0ErrorList *errors);

#endif
