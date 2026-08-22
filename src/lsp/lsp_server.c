#include "base/safe.h"
/*
 * lsp_server.c — RingEcho LSP server
 *
 * Protocol: JSON-RPC 2.0 over stdin/stdout (Content-Length framing)
 *
 * Supported methods:
 *   - initialize: return server capabilities
 *   - shutdown: prepare to shut down
 *   - textDocument/didOpen: receive file content → diagnostics
 *   - textDocument/didChange: receive changes → diagnostics
 *   - textDocument/hover: return type information
 *
 * Notifications sent:
 *   - textDocument/publishDiagnostics: push errors/warnings
 */

#include "lsp/lsp_json.h"
#include "lsp/lsp_server.h"
#include "re0.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#if !defined(RE0_PLATFORM_WINDOWS)
#include <unistd.h>
#endif

#if defined(RE0_PLATFORM_WINDOWS)
#include <fcntl.h>
#include <io.h>
#endif

/* ── utility: JSON escaping ── */
static void json_escape(FILE *f, const char *s) {
    fputc('"', f);
    while (*s) {
        switch (*s) {
            case '"': fputs("\\\"", f); break;
            case '\\': fputs("\\\\", f); break;
            case '\n': fputs("\\n", f); break;
            case '\r': fputs("\\r", f); break;
            case '\t': fputs("\\t", f); break;
            default:
                if ((unsigned char)*s < 0x20)
                    fprintf(f, "\\u%04x", (unsigned char)*s);
                else
                    fputc(*s, f);
        }
        s++;
    }
    fputc('"', f);
}

/* ── send a JSON-RPC message ── */
static void lsp_send(const char *json) {
    size_t len = strlen(json);
    fprintf(stdout, "Content-Length: %zu\r\n\r\n%s", len, json);
    fflush(stdout);
}

/* ── send a response (request with id) ── */
static void lsp_send_response(int id, const char *result_json) {
    size_t need = strlen(result_json) + 128;
    char *buf = (char*)malloc(need);
    if (!buf) return;
    snprintf(buf, need,
        "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":%s}", id, result_json);
    lsp_send(buf);
    free(buf);
}

/* ── send diagnostics notification ── */
static void lsp_send_diagnostics(const char *uri, Re0ErrorList *errors) {
    FILE *f = tmpfile();
    if (!f) return;

    fprintf(f, "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\",");
    fprintf(f, "\"params\":{\"uri\":");
    json_escape(f, uri);
    fprintf(f, ",\"diagnostics\":[");

    bool first = true;
    if (errors) {
        for (size_t i = 0; i < Re0ErrorVec_len(&errors->errors); i++) {
            Re0Error *e = &errors->errors.data[i];
            if (e->level == RE0_WARN) continue; /* report errors only, not warnings */
            if (!first) fputc(',', f);
            first = false;
            fprintf(f, "{\"range\":{\"start\":{\"line\":%zu,\"character\":%zu},"
                       "\"end\":{\"line\":%zu,\"character\":%zu}},"
                       "\"severity\":1,\"source\":\"reoc\",\"message\":",
                    e->span.start.line > 0 ? e->span.start.line - 1 : 0,
                    e->span.start.column > 0 ? e->span.start.column - 1 : 0,
                    e->span.end.line > 0 ? e->span.end.line - 1 : 0,
                    e->span.end.column > 0 ? e->span.end.column - 1 : 0);
            json_escape(f, e->msg ? e->msg : "unknown error");
            fputc('}', f);
        }
    }

    fprintf(f, "]}}");
    fflush(f);

    long len = ftell(f);
    if (len < 0) { fclose(f); return; }
    char *buf = (char*)malloc((size_t)len + 1);
    if (!buf) { fclose(f); return; }
    fseek(f, 0, SEEK_SET);
    size_t rd = fread(buf, 1, (size_t)len, f);
    buf[rd] = '\0';
    fclose(f);
    lsp_send(buf);
    free(buf);
}

/* ── compile and collect diagnostics ── */
static void run_diagnostics(const char *uri, const char *source) {
    Re0Compiler comp;
    re0_compiler_init(&comp, &re0_backend_c);
    if (comp.arena == NULL) {
        /* arena allocation failed: lexer/parser/sema/codegen are
         * uninitialized (destroying them would be UB). errors/model/
         * builtins/bus are initialized, so report the OOM diagnostic. */
        lsp_send_diagnostics(uri, &comp.errors);
        re0_error_list_free(&comp.errors);
        re0_model_free(&comp.model);
        re0_builtin_free(&comp.builtins);
        re0_event_bus_free(&comp.bus);
        return;
    }

    bool ok = re0_lexer_tokenize(&comp.lexer, source, uri);
    if (ok) ok = re0_parser_parse(&comp.parser, &comp.lexer.stream);
    if (ok) re0_sema_check(&comp.sema, &comp.parser.stmts);
    /* lint is not run (LSP does not need style checks) */

    lsp_send_diagnostics(uri, &comp.errors);
    re0_compiler_destroy(&comp);
}

/* ── extract text content from params ── */
static const char *extract_text(JVal *params) {
    JVal *td = json_get(params, "textDocument");
    if (!td) return NULL;
    return json_str(json_get(td, "text"), NULL);
}

/* ── extract URI from params ── */
static const char *extract_uri(JVal *params) {
    JVal *td = json_get(params, "textDocument");
    if (!td) return NULL;
    return json_str(json_get(td, "uri"), NULL);
}

/* Case-insensitive header-name match (LSP headers are case-insensitive,
 * and hand-rolled clients get this wrong surprisingly often). */
static bool header_matches(const char *header, const char *name) {
    while (*name) {
        if (tolower((unsigned char)*header) != tolower((unsigned char)*name))
            return false;
        header++;
        name++;
    }
    return true;
}

/* ── read one JSON-RPC message ── */
static char *read_message(size_t *out_len) {
    /* read Content-Length header */
    size_t content_len = 0;
    char header[256];
    while (fgets(header, sizeof(header), stdin)) {
        /* strip \r\n */
        size_t hlen = strlen(header);
        while (hlen > 0 && (header[hlen-1] == '\r' || header[hlen-1] == '\n'))
            header[--hlen] = '\0';
        if (hlen == 0) break; /* empty line = end of headers */
        if (header_matches(header, "Content-Length:")) {
            long cl = atol(header + 15);
            if (cl <= 0 || cl > (long)RE0_MAX_LSP_MESSAGE) {
                fprintf(stderr, "[lsp] invalid Content-Length '%s'\n", header);
                return NULL; /* unframeable stream: stop */
            }
            content_len = (size_t)cl;
        }
    }
    if (content_len == 0) return NULL;

    char *body = (char*)malloc(content_len + 1);
    if (!body) return NULL;
    size_t total = 0;
    while (total < content_len) {
        size_t n = fread(body + total, 1, content_len - total, stdin);
        if (n == 0) break;
        total += n;
    }
    if (total < content_len) {
        /* truncated body: EOF mid-message. A partial JSON would produce
         * garbage diagnostics — reject it. */
        free(body);
        return NULL;
    }
    body[total] = '\0';
    *out_len = total;
    return body;
}

/* ── LSP main loop ── */
int lsp_server_run(void) {
#if defined(RE0_PLATFORM_WINDOWS)
    /* Text-mode stdin/stdout translate \r\n ↔ \n and treat 0x1A as EOF,
     * corrupting the byte-exact Content-Length framing. */
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    bool shutdown_req = false;

    while (!shutdown_req) {
        size_t msg_len = 0;
        char *raw = read_message(&msg_len);
        if (!raw) break; /* stdin closed or unframeable */

        JVal *msg = json_parse(raw, msg_len);
        free(raw);
        if (!msg) continue;

        const char *method = json_str(json_get(msg, "method"), "");
        JVal *params = json_get(msg, "params");
        JVal *id_val = json_get(msg, "id");
        double id_num = json_num_val(id_val, -1.0);
        int id = (id_num >= 0.0 && id_num <= 2147483647.0) ? (int)id_num : -1;

        if (strcmp(method, "initialize") == 0) {
            lsp_send_response(id,
                "{\"capabilities\":{"
                "\"textDocumentSync\":1,"  /* full sync */
                "\"hoverProvider\":true"
                "},"
                "\"serverInfo\":{\"name\":\"reoc-lsp\",\"version\":\"0.2.0\"}"
                "}");
        } else if (strcmp(method, "initialized") == 0) {
            /* notification: client acknowledges initialization, no response needed */
        } else if (strcmp(method, "shutdown") == 0) {
            shutdown_req = true;
            lsp_send_response(id, "null");
        } else if (strcmp(method, "exit") == 0) {
            json_free(msg);
            /* LSP spec: exit without a prior shutdown must return 1 */
            return shutdown_req ? 0 : 1;
        } else if (strcmp(method, "textDocument/didOpen") == 0) {
            const char *uri = extract_uri(params);
            const char *text = extract_text(params);
            if (uri && text) run_diagnostics(uri, text);
        } else if (strcmp(method, "textDocument/didChange") == 0) {
            const char *uri = extract_uri(params);
            /* didChange text lives in changes[0].text */
            JVal *changes = json_get(params, "contentChanges");
            if (changes && changes->type == J_ARR && changes->arr.count > 0) {
                const char *text = json_str(
                    json_get(changes->arr.items[0], "text"), NULL);
                if (uri && text) run_diagnostics(uri, text);
            }
        } else if (strcmp(method, "textDocument/hover") == 0) {
            /* MVP: return empty hover */
            lsp_send_response(id, "null");
        } else if (id >= 0) {
            /* unknown request, return method not found */
            char buf[256];
            snprintf(buf, sizeof(buf),
                "{\"jsonrpc\":\"2.0\",\"id\":%d,\"error\":"
                "{\"code\":-32601,\"message\":\"Method not found\"}}", id);
            lsp_send(buf);
        }

        json_free(msg);
    }

    return 0;
}
