# compiler-reo

A RingEcho implementation of the RingEcho compiler (self-hosting), written in
`.reo` and built with the C reference compiler (`rev0`) during bootstrap.
See [BOOTSTRAP.md](../BOOTSTRAP.md) and `.agent/lang-c/selfhost-gap.md`.

Stage: seed. Files:
- `lexer.reo` — tokenizes a source string and self-checks the result.
- `parser.reo` — `ident = number;` parsing seed.
- `expr.reo` — expression AST, precedence climbing, evaluator.
- `frontend.reo` — statements/blocks, `if`/`else`, `while`, assignments, enums,
  function declarations with parameter/return types, a deterministic AST dump,
  scoped lexical name resolution, and initial type checking. Its self-check
  includes a first self-consumption probe: it parses and resolves `lexer.reo`
  with zero errors.

Run one: `target/Release/rev run compiler-reo/frontend.reo`.
Run all: `make test-selfhost`.
