# compiler-reo

A RingEcho implementation of the RingEcho compiler (self-hosting), written in
`.reo` and built with the C reference compiler (`rev0`) during bootstrap.
See [BOOTSTRAP.md](../BOOTSTRAP.md) and `.agent/lang-c/selfhost-gap.md`.

Stage: seed — `lexer.reo` tokenizes a source string and self-checks the result.
Run: `target/Release/rev run compiler-reo/lexer.reo`.
