/*
 * lsp_server.c — RingEcho LSP 服务器
 *
 * 协议：JSON-RPC 2.0 over stdin/stdout (Content-Length framing)
 *
 * 支持的方法：
 *   - initialize: 返回服务器能力
 *   - shutdown: 准备关闭
 *   - textDocument/didOpen: 接收文件内容 → 诊断
 *   - textDocument/didChange: 接收变更 → 诊断
 *   - textDocument/hover: 返回类型信息
 *
 * 发送的通知：
 *   - textDocument/publishDiagnostics: 推送错误/警告
 */

#include "lsp_json.h"
#include "re0.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ── 工具：JSON 转义 ── */
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

/* ── 发送 JSON-RPC 消息 ── */
static void lsp_send(const char *json) {
    size_t len = strlen(json);
    fprintf(stdout, "Content-Length: %zu\r\n\r\n%s", len, json);
    fflush(stdout);
}

/* ── 发送响应（有 id 的请求） ── */
static void lsp_send_response(int id, const char *result_json) {
    char buf[8192];
    snprintf(buf, sizeof(buf),
        "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":%s}", id, result_json);
    lsp_send(buf);
}

/* ── 发送诊断通知 ── */
static void lsp_send_diagnostics(const char *uri, Re0ErrorList *errors) {
    FILE *f = tmpfile();
    if (!f) return;

    fprintf(f, "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\",");
    fprintf(f, "\"params\":{\"uri\":");
    json_escape(f, uri);
    fprintf(f, ",\"diagnostics\":[");

    bool first = true;
    for (size_t i = 0; i < Re0ErrorVec_len(&errors->errors); i++) {
        Re0Error *e = &errors->errors.data[i];
        if (e->level == RE0_WARN) continue; /* 只报 error，不报 warning */
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

    fprintf(f, "]}}");
    fflush(f);

    long len = ftell(f);
    char *buf = (char*)malloc((size_t)len + 1);
    fseek(f, 0, SEEK_SET);
    fread(buf, 1, (size_t)len, f);
    buf[len] = '\0';
    fclose(f);
    lsp_send(buf);
    free(buf);
}

/* ── 编译并获取诊断 ── */
static void run_diagnostics(const char *uri, const char *source) {
    Re0Compiler comp;
    re0_compiler_init(&comp, &re0_backend_c);

    bool ok = re0_lexer_tokenize(&comp.lexer, source, uri);
    if (ok) ok = re0_parser_parse(&comp.parser, &comp.lexer.stream);
    if (ok) re0_sema_check(&comp.sema, &comp.parser.stmts);
    /* lint 不运行（LSP 不需要 style 检查） */

    lsp_send_diagnostics(uri, &comp.errors);
    re0_compiler_destroy(&comp);
}

/* ── 从 params 提取文本内容 ── */
static const char *extract_text(JVal *params) {
    JVal *td = json_get(params, "textDocument");
    if (!td) return NULL;
    return json_str(json_get(td, "text"), NULL);
}

/* ── 从 params 提取 URI ── */
static const char *extract_uri(JVal *params) {
    JVal *td = json_get(params, "textDocument");
    if (!td) return NULL;
    return json_str(json_get(td, "uri"), NULL);
}

/* ── 读取一条 JSON-RPC 消息 ── */
static char *read_message(size_t *out_len) {
    /* 读取 Content-Length 头 */
    size_t content_len = 0;
    char header[256];
    while (fgets(header, sizeof(header), stdin)) {
        /* 去除 \r\n */
        size_t hlen = strlen(header);
        while (hlen > 0 && (header[hlen-1] == '\r' || header[hlen-1] == '\n'))
            header[--hlen] = '\0';
        if (hlen == 0) break; /* 空行 = 头结束 */
        if (strncmp(header, "Content-Length:", 15) == 0)
            content_len = (size_t)atol(header + 15);
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
    body[total] = '\0';
    *out_len = total;
    return body;
}

/* ── LSP 主循环 ── */
int lsp_server_run(void) {
    bool initialized = false;
    bool shutdown_req = false;

    while (!shutdown_req) {
        size_t msg_len = 0;
        char *raw = read_message(&msg_len);
        if (!raw) break; /* stdin closed */

        JVal *msg = json_parse(raw, msg_len);
        free(raw);
        if (!msg) continue;

        const char *method = json_str(json_get(msg, "method"), "");
        JVal *params = json_get(msg, "params");
        JVal *id_val = json_get(msg, "id");
        int id = (int)json_num_val(id_val, -1);

        if (strcmp(method, "initialize") == 0) {
            initialized = true;
            lsp_send_response(id,
                "{\"capabilities\":{"
                "\"textDocumentSync\":1,"  /* full sync */
                "\"hoverProvider\":true"
                "},"
                "\"serverInfo\":{\"name\":\"reoc-lsp\",\"version\":\"0.2.0\"}"
                "}");
        } else if (strcmp(method, "initialized") == 0) {
            /* notification: 客户端确认初始化，无需响应 */
        } else if (strcmp(method, "shutdown") == 0) {
            shutdown_req = true;
            lsp_send_response(id, "null");
        } else if (strcmp(method, "exit") == 0) {
            json_free(msg);
            break;
        } else if (strcmp(method, "textDocument/didOpen") == 0) {
            const char *uri = extract_uri(params);
            const char *text = extract_text(params);
            if (uri && text) run_diagnostics(uri, text);
        } else if (strcmp(method, "textDocument/didChange") == 0) {
            const char *uri = extract_uri(params);
            /* didChange 的 text 在 changes[0].text 中 */
            JVal *changes = json_get(params, "contentChanges");
            if (changes && changes->type == J_ARR && changes->arr.count > 0) {
                const char *text = json_str(
                    json_get(changes->arr.items[0], "text"), NULL);
                if (uri && text) run_diagnostics(uri, text);
            }
        } else if (strcmp(method, "textDocument/hover") == 0) {
            /* MVP: 返回空 hover */
            lsp_send_response(id, "null");
        } else if (id >= 0) {
            /* 未知请求，返回 method not found */
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
