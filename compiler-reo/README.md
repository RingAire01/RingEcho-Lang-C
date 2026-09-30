# compiler-reo

A RingEcho implementation of the RingEcho compiler (self-hosting), written in
`.reo` and built with the C reference compiler (`rev0`) during bootstrap.
See [BOOTSTRAP.md](../BOOTSTRAP.md) and `.agent/lang-c/selfhost-gap.md`.

Stage: seed. Files:
- `lexer.reo` — tokenizes a source string and self-checks the result.
- `parser.reo` — `ident = number;` parsing seed.
- `expr.reo` — expression AST, precedence climbing, evaluator.
- `frontend.reo` — statements/blocks, `if`/`else`, function declarations, a
  deterministic AST dump, lexical name resolution, and initial type checking.

Run one: `target/Release/rev run compiler-reo/frontend.reo`.
Run all: `make test-selfhost`.
