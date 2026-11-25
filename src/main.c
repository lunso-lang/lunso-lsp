#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} StrBuf;

typedef struct {
    char *name;
    char *detail;
    int kind; // LSP SymbolKind: 12=Function, 23=Struct, 10=Enum, 14=Constant
    int line; // 0-based
    int col;  // 0-based
    int end_col;
} Symbol;

typedef struct {
    char *uri;
    char *path;
    char *text;
    size_t len;
    Symbol *symbols;
    size_t symbol_count;
} Document;

static Document *documents = NULL;
static size_t document_count = 0;
static bool should_exit = false;
static bool shutdown_requested = false;

static void sb_init(StrBuf *sb)
{
    sb->cap = 256;
    sb->len = 0;
    sb->data = malloc(sb->cap);
    if (sb->data) {
        sb->data[0] = '\0';
    }
}

static void sb_append_n(StrBuf *sb, const char *s, size_t n)
{
    if (!sb->data) {
        return;
    }
    if (sb->len + n + 1 > sb->cap) {
        size_t new_cap = sb->cap * 2;
        while (new_cap < sb->len + n + 1) {
            new_cap *= 2;
        }
        char *grown = realloc(sb->data, new_cap);
        if (!grown) {
            return;
        }
        sb->data = grown;
        sb->cap = new_cap;
    }
    memcpy(sb->data + sb->len, s, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
}

static void sb_append(StrBuf *sb, const char *s)
{
    sb_append_n(sb, s, strlen(s));
}

static void sb_append_char(StrBuf *sb, char c)
{
    sb_append_n(sb, &c, 1);
}

static void sb_append_int(StrBuf *sb, long long v)
{
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%lld", v);
    if (n > 0) {
        sb_append_n(sb, buf, (size_t)n);
    }
}

static void sb_append_json_str(StrBuf *sb, const char *s)
{
    sb_append_char(sb, '"');
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
        case '"': sb_append(sb, "\\\""); break;
        case '\\': sb_append(sb, "\\\\"); break;
        case '\b': sb_append(sb, "\\b"); break;
        case '\f': sb_append(sb, "\\f"); break;
        case '\n': sb_append(sb, "\\n"); break;
        case '\r': sb_append(sb, "\\r"); break;
        case '\t': sb_append(sb, "\\t"); break;
        default:
            if (*p < 0x20) {
                char esc[8];
                snprintf(esc, sizeof(esc), "\\u%04x", *p);
                sb_append(sb, esc);
            } else {
                sb_append_char(sb, (char)*p);
            }
            break;
        }
    }
    sb_append_char(sb, '"');
}

static void send_message(const char *json, size_t len)
{
    printf("Content-Length: %zu\r\n\r\n", len);
    fwrite(json, 1, len, stdout);
    fflush(stdout);
}

static void send_response(const char *id_raw, const char *result_json)
{
    StrBuf sb;
    sb_init(&sb);
    sb_append(&sb, "{\"jsonrpc\":\"2.0\",\"id\":");
    sb_append(&sb, id_raw ? id_raw : "null");
    sb_append(&sb, ",\"result\":");
    sb_append(&sb, result_json);
    sb_append(&sb, "}");
    send_message(sb.data, sb.len);
    free(sb.data);
}

static void send_notification(const char *method, const char *params_json)
{
    StrBuf sb;
    sb_init(&sb);
    sb_append(&sb, "{\"jsonrpc\":\"2.0\",\"method\":");
    sb_append_json_str(&sb, method);
    sb_append(&sb, ",\"params\":");
    sb_append(&sb, params_json);
    sb_append(&sb, "}");
    send_message(sb.data, sb.len);
    free(sb.data);
}

// --- Minimal JSON navigation helpers ---

static const char *skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
        p++;
    }
    return p;
}

static const char *skip_value(const char *p)
{
    p = skip_ws(p);
    if (*p == '"') {
        p++;
        while (*p && *p != '"') {
            if (*p == '\\' && p[1]) {
                p += 2;
            } else {
                p++;
            }
        }
        return *p == '"' ? p + 1 : p;
    }
    if (*p == '{' || *p == '[') {
        char open = *p;
        char close = open == '{' ? '}' : ']';
        int depth = 1;
        p++;
        while (*p && depth > 0) {
            if (*p == '"') {
                p = skip_value(p);
                continue;
            }
            if (*p == open) {
                depth++;
            } else if (*p == close) {
                depth--;
            }
            p++;
        }
        return p;
    }
    while (*p && *p != ',' && *p != '}' && *p != ']' &&
           *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') {
        p++;
    }
    return p;
}

static char *decode_json_string(const char *p, const char **end_out)
{
    p = skip_ws(p);
    if (*p != '"') {
        if (end_out) {
            *end_out = p;
        }
        return NULL;
    }
    p++;
    StrBuf sb;
    sb_init(&sb);
    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
            case '"': sb_append_char(&sb, '"'); break;
            case '\\': sb_append_char(&sb, '\\'); break;
            case '/': sb_append_char(&sb, '/'); break;
            case 'b': sb_append_char(&sb, '\b'); break;
            case 'f': sb_append_char(&sb, '\f'); break;
            case 'n': sb_append_char(&sb, '\n'); break;
            case 'r': sb_append_char(&sb, '\r'); break;
            case 't': sb_append_char(&sb, '\t'); break;
            case 'u':
                if (isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2]) &&
                    isxdigit((unsigned char)p[3]) && isxdigit((unsigned char)p[4])) {
                    unsigned int cp = 0;
                    sscanf(p + 1, "%4x", &cp);
                    if (cp < 0x80) {
                        sb_append_char(&sb, (char)cp);
                    }
                    p += 4;
                }
                break;
            default:
                sb_append_char(&sb, *p);
                break;
            }
            p++;
        } else {
            sb_append_char(&sb, *p++);
        }
    }
    if (*p == '"') {
        p++;
    }
    if (end_out) {
        *end_out = p;
    }
    return sb.data;
}

static const char *json_find_field(const char *obj, const char *key)
{
    if (!obj) {
        return NULL;
    }
    const char *p = skip_ws(obj);
    if (*p != '{') {
        return NULL;
    }
    p = skip_ws(p + 1);
    while (*p && *p != '}') {
        if (*p == ',') {
            p = skip_ws(p + 1);
            continue;
        }
        const char *after_key = NULL;
        char *k = decode_json_string(p, &after_key);
        if (!k) {
            break;
        }
        p = skip_ws(after_key);
        if (*p == ':') {
            p = skip_ws(p + 1);
        }
        bool match = strcmp(k, key) == 0;
        free(k);
        if (match) {
            return p;
        }
        p = skip_ws(skip_value(p));
    }
    return NULL;
}

static char *json_get_string(const char *obj, const char *key)
{
    const char *val = json_find_field(obj, key);
    return val ? decode_json_string(val, NULL) : NULL;
}

static bool json_get_int(const char *obj, const char *key, long long *out)
{
    const char *val = json_find_field(obj, key);
    if (!val) {
        return false;
    }
    char *end = NULL;
    long long v = strtoll(val, &end, 10);
    if (end == val) {
        return false;
    }
    *out = v;
    return true;
}

static char *json_get_raw(const char *obj, const char *key)
{
    const char *val = json_find_field(obj, key);
    if (!val) {
        return NULL;
    }
    const char *end = skip_value(val);
    size_t n = (size_t)(end - val);
    char *raw = malloc(n + 1);
    if (raw) {
        memcpy(raw, val, n);
        raw[n] = '\0';
    }
    return raw;
}

// --- URI & Symbol scanning ---

static char *uri_to_path(const char *uri)
{
    if (strncmp(uri, "file://", 7) != 0) {
        return strdup(uri);
    }
    const char *src = uri + 7;
    StrBuf sb;
    sb_init(&sb);
    while (*src) {
        if (*src == '%' && isxdigit((unsigned char)src[1]) && isxdigit((unsigned char)src[2])) {
            unsigned int ch = 0;
            sscanf(src + 1, "%2x", &ch);
            sb_append_char(&sb, (char)ch);
            src += 3;
        } else {
            sb_append_char(&sb, *src++);
        }
    }
    return sb.data;
}

static bool is_ident_start(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static bool is_ident_char(unsigned char c)
{
    return is_ident_start(c) || (c >= '0' && c <= '9');
}

static void free_symbols(Document *doc)
{
    for (size_t i = 0; i < doc->symbol_count; i++) {
        free(doc->symbols[i].name);
        free(doc->symbols[i].detail);
    }
    free(doc->symbols);
    doc->symbols = NULL;
    doc->symbol_count = 0;
}

static void add_symbol(Document *doc, const char *name, size_t name_len,
                       const char *detail, size_t detail_len,
                       int kind, int line, int col)
{
    Symbol *grown = realloc(doc->symbols, (doc->symbol_count + 1) * sizeof(*grown));
    if (!grown) {
        return;
    }
    doc->symbols = grown;
    Symbol *sym = &doc->symbols[doc->symbol_count++];
    sym->name = malloc(name_len + 1);
    memcpy(sym->name, name, name_len);
    sym->name[name_len] = '\0';
    sym->detail = malloc(detail_len + 1);
    memcpy(sym->detail, detail, detail_len);
    sym->detail[detail_len] = '\0';
    sym->kind = kind;
    sym->line = line;
    sym->col = col;
    sym->end_col = col + (int)name_len;
}

static void scan_symbols(Document *doc)
{
    free_symbols(doc);
    const char *p = doc->text;
    int line = 0;
    while (*p) {
        const char *line_start = p;
        const char *eol = strchr(p, '\n');
        size_t line_len = eol ? (size_t)(eol - p) : strlen(p);
        if (line_len > 0 && line_start[line_len - 1] == '\r') {
            line_len--;
        }

        const char *cur = line_start;
        const char *end = line_start + line_len;
        while (cur < end && (*cur == ' ' || *cur == '\t')) {
            cur++;
        }

        // Only index top-level declarations (unindented) for clean outline/defs
        if (cur == line_start) {
            if (end - cur >= 4 && (strncmp(cur, "pub ", 4) == 0)) {
                cur += 4;
                while (cur < end && (*cur == ' ' || *cur == '\t')) cur++;
            } else if (end - cur >= 5 && (strncmp(cur, "priv ", 5) == 0)) {
                cur += 5;
                while (cur < end && (*cur == ' ' || *cur == '\t')) cur++;
            }
            if (end - cur >= 4 && strncmp(cur, "ext ", 4) == 0) {
                cur += 4;
                while (cur < end && (*cur == ' ' || *cur == '\t')) cur++;
            }

            int kind = 0;
            if (end - cur >= 4 && strncmp(cur, "fun ", 4) == 0) {
                kind = 12;
                cur += 4;
            } else if (end - cur >= 7 && strncmp(cur, "struct ", 7) == 0) {
                kind = 23;
                cur += 7;
            } else if (end - cur >= 5 && strncmp(cur, "enum ", 5) == 0) {
                kind = 10;
                cur += 5;
            } else if (end - cur >= 6 && strncmp(cur, "const ", 6) == 0) {
                kind = 14;
                cur += 6;
            }

            if (kind != 0) {
                while (cur < end && (*cur == ' ' || *cur == '\t')) cur++;
                if (cur < end && is_ident_start((unsigned char)*cur)) {
                    const char *name_start = cur;
                    while (cur < end && is_ident_char((unsigned char)*cur)) cur++;
                    add_symbol(doc, name_start, (size_t)(cur - name_start),
                               line_start, line_len, kind, line,
                               (int)(name_start - line_start));
                }
            }
        }

        if (!eol) {
            break;
        }
        p = eol + 1;
        line++;
    }
}

static Document *find_document(const char *uri)
{
    for (size_t i = 0; i < document_count; i++) {
        if (strcmp(documents[i].uri, uri) == 0) {
            return &documents[i];
        }
    }
    return NULL;
}

static Document *upsert_document(const char *uri, const char *text)
{
    Document *doc = find_document(uri);
    if (!doc) {
        Document *grown = realloc(documents, (document_count + 1) * sizeof(*grown));
        if (!grown) {
            return NULL;
        }
        documents = grown;
        doc = &documents[document_count++];
        memset(doc, 0, sizeof(*doc));
        doc->uri = strdup(uri);
        doc->path = uri_to_path(uri);
    } else {
        free(doc->text);
    }
    doc->text = strdup(text ? text : "");
    doc->len = strlen(doc->text);
    scan_symbols(doc);
    return doc;
}

static void remove_document(const char *uri)
{
    for (size_t i = 0; i < document_count; i++) {
        if (strcmp(documents[i].uri, uri) == 0) {
            free(documents[i].uri);
            free(documents[i].path);
            free(documents[i].text);
            free_symbols(&documents[i]);
            documents[i] = documents[--document_count];
            return;
        }
    }
}

// --- Diagnostics via `lunso --check` ---

static const char *resolve_lunso_bin(void)
{
    const char *env = getenv("LUNSO");
    if (env && *env) {
        return env;
    }
    static char fallback[512];
    const char *home = getenv("HOME");
    if (home) {
        snprintf(fallback, sizeof(fallback), "%s/projects/lunso-boot/build/lunso", home);
        if (access(fallback, X_OK) == 0) {
            return fallback;
        }
    }
    return "lunso";
}

static void publish_diagnostics(Document *doc)
{
    // Write a temporary file next to the source file so relative imports resolve.
    const char *slash = strrchr(doc->path, '/');
    size_t dir_len = slash ? (size_t)(slash - doc->path + 1) : 0;
    char *tmp_path = malloc(dir_len + 32);
    if (!tmp_path) {
        return;
    }
    if (dir_len > 0) {
        memcpy(tmp_path, doc->path, dir_len);
    }
    snprintf(tmp_path + dir_len, 32, ".lunso-lsp-%ld.lunso", (long)getpid());

    FILE *f = fopen(tmp_path, "wb");
    if (!f) {
        free(tmp_path);
        return;
    }
    fwrite(doc->text, 1, doc->len, f);
    fclose(f);

    const char *lunso = resolve_lunso_bin();
    size_t cmd_cap = strlen(lunso) + strlen(tmp_path) + 32;
    char *cmd = malloc(cmd_cap);
    StrBuf diags;
    sb_init(&diags);
    sb_append(&diags, "{\"uri\":");
    sb_append_json_str(&diags, doc->uri);
    sb_append(&diags, ",\"diagnostics\":[");

    if (cmd) {
        snprintf(cmd, cmd_cap, "\"%s\" --check \"%s\" 2>&1", lunso, tmp_path);
        FILE *pipe = popen(cmd, "r");
        if (pipe) {
            char line_buf[1024];
            bool first = true;
            while (fgets(line_buf, sizeof(line_buf), pipe)) {
                size_t l = strlen(line_buf);
                while (l > 0 && (line_buf[l - 1] == '\n' || line_buf[l - 1] == '\r')) {
                    line_buf[--l] = '\0';
                }
                // Parse `<path>:<line>:<col>: <message>`
                size_t path_len = strlen(tmp_path);
                const char *after = NULL;
                if (strncmp(line_buf, tmp_path, path_len) == 0 && line_buf[path_len] == ':') {
                    after = line_buf + path_len + 1;
                } else {
                    continue;
                }
                char *end1 = NULL;
                long ln = strtol(after, &end1, 10);
                if (!end1 || *end1 != ':') {
                    continue;
                }
                char *end2 = NULL;
                long col = strtol(end1 + 1, &end2, 10);
                if (!end2 || *end2 != ':') {
                    continue;
                }
                const char *msg = end2 + 1;
                while (*msg == ' ') {
                    msg++;
                }
                long lsp_line = ln > 0 ? ln - 1 : 0;
                long lsp_col = col > 0 ? col - 1 : 0;

                if (!first) {
                    sb_append_char(&diags, ',');
                }
                first = false;
                sb_append(&diags, "{\"range\":{\"start\":{\"line\":");
                sb_append_int(&diags, lsp_line);
                sb_append(&diags, ",\"character\":");
                sb_append_int(&diags, lsp_col);
                sb_append(&diags, "},\"end\":{\"line\":");
                sb_append_int(&diags, lsp_line);
                sb_append(&diags, ",\"character\":");
                sb_append_int(&diags, lsp_col + 1);
                sb_append(&diags, "}},\"severity\":1,\"source\":\"lunso\",\"message\":");
                sb_append_json_str(&diags, msg);
                sb_append_char(&diags, '}');
            }
            pclose(pipe);
        }
        free(cmd);
    }

    unlink(tmp_path);
    free(tmp_path);

    sb_append(&diags, "]}");
    send_notification("textDocument/publishDiagnostics", diags.data);
    free(diags.data);
}

// --- Word lookup for hover / definition ---

static char *word_at(const Document *doc, long long target_line, long long target_col)
{
    const char *p = doc->text;
    long long line = 0;
    while (*p && line < target_line) {
        if (*p == '\n') {
            line++;
        }
        p++;
    }
    if (line != target_line) {
        return NULL;
    }
    const char *eol = strchr(p, '\n');
    size_t line_len = eol ? (size_t)(eol - p) : strlen(p);
    if (target_col < 0 || (size_t)target_col >= line_len) {
        return NULL;
    }
    size_t pos = (size_t)target_col;
    if (!is_ident_char((unsigned char)p[pos])) {
        if (pos > 0 && is_ident_char((unsigned char)p[pos - 1])) {
            pos--;
        } else {
            return NULL;
        }
    }
    size_t start = pos;
    while (start > 0 && is_ident_char((unsigned char)p[start - 1])) {
        start--;
    }
    size_t end = pos + 1;
    while (end < line_len && is_ident_char((unsigned char)p[end])) {
        end++;
    }
    char *word = malloc(end - start + 1);
    if (word) {
        memcpy(word, p + start, end - start);
        word[end - start] = '\0';
    }
    return word;
}

static const Symbol *find_symbol(const Document *doc, const char *name)
{
    for (size_t i = 0; i < doc->symbol_count; i++) {
        if (strcmp(doc->symbols[i].name, name) == 0) {
            return &doc->symbols[i];
        }
    }
    return NULL;
}

static const char *builtin_doc(const char *word)
{
    if (strcmp(word, "len") == 0) return "fun len(collection: []T | [N]T): i64";
    if (strcmp(word, "data") == 0) return "fun data(collection: []T | [N]T): *const T";
    if (strcmp(word, "size_of") == 0) return "fun size_of(value: T): u64";
    return NULL;
}

// --- Request handlers ---

static void handle_initialize(const char *id_raw)
{
    send_response(
        id_raw,
        "{\"capabilities\":{"
        "\"textDocumentSync\":{\"openClose\":true,\"change\":1,\"save\":{\"includeText\":true}},"
        "\"hoverProvider\":true,"
        "\"definitionProvider\":true,"
        "\"documentSymbolProvider\":true,"
        "\"completionProvider\":{\"triggerCharacters\":[\".\",\":\"]}"
        "},\"serverInfo\":{\"name\":\"lunso-lsp\",\"version\":\"0.1.0\"}}");
}

static void handle_document_symbol(const char *id_raw, const char *params)
{
    const char *td = json_find_field(params, "textDocument");
    char *uri = json_get_string(td, "uri");
    Document *doc = uri ? find_document(uri) : NULL;
    StrBuf sb;
    sb_init(&sb);
    sb_append_char(&sb, '[');
    if (doc) {
        for (size_t i = 0; i < doc->symbol_count; i++) {
            Symbol *sym = &doc->symbols[i];
            if (i > 0) sb_append_char(&sb, ',');
            sb_append(&sb, "{\"name\":");
            sb_append_json_str(&sb, sym->name);
            sb_append(&sb, ",\"kind\":");
            sb_append_int(&sb, sym->kind);
            sb_append(&sb, ",\"location\":{\"uri\":");
            sb_append_json_str(&sb, doc->uri);
            sb_append(&sb, ",\"range\":{\"start\":{\"line\":");
            sb_append_int(&sb, sym->line);
            sb_append(&sb, ",\"character\":");
            sb_append_int(&sb, sym->col);
            sb_append(&sb, "},\"end\":{\"line\":");
            sb_append_int(&sb, sym->line);
            sb_append(&sb, ",\"character\":");
            sb_append_int(&sb, sym->end_col);
            sb_append(&sb, "}}}}");
        }
    }
    sb_append_char(&sb, ']');
    send_response(id_raw, sb.data);
    free(sb.data);
    free(uri);
}

static void handle_hover(const char *id_raw, const char *params)
{
    const char *td = json_find_field(params, "textDocument");
    const char *pos = json_find_field(params, "position");
    char *uri = json_get_string(td, "uri");
    long long line = 0, col = 0;
    Document *doc = uri ? find_document(uri) : NULL;
    if (!doc || !json_get_int(pos, "line", &line) || !json_get_int(pos, "character", &col)) {
        send_response(id_raw, "null");
        free(uri);
        return;
    }

    char *word = word_at(doc, line, col);
    if (!word) {
        send_response(id_raw, "null");
        free(uri);
        return;
    }

    const Symbol *sym = find_symbol(doc, word);
    const char *bdoc = sym ? sym->detail : builtin_doc(word);
    if (!bdoc) {
        send_response(id_raw, "null");
        free(word);
        free(uri);
        return;
    }

    StrBuf md;
    sb_init(&md);
    sb_append(&md, "```lunso\n");
    sb_append(&md, bdoc);
    sb_append(&md, "\n```");

    StrBuf resp;
    sb_init(&resp);
    sb_append(&resp, "{\"contents\":{\"kind\":\"markdown\",\"value\":");
    sb_append_json_str(&resp, md.data);
    sb_append(&resp, "}}");
    send_response(id_raw, resp.data);

    free(resp.data);
    free(md.data);
    free(word);
    free(uri);
}

static void handle_definition(const char *id_raw, const char *params)
{
    const char *td = json_find_field(params, "textDocument");
    const char *pos = json_find_field(params, "position");
    char *uri = json_get_string(td, "uri");
    long long line = 0, col = 0;
    Document *doc = uri ? find_document(uri) : NULL;
    if (!doc || !json_get_int(pos, "line", &line) || !json_get_int(pos, "character", &col)) {
        send_response(id_raw, "null");
        free(uri);
        return;
    }
    char *word = word_at(doc, line, col);
    const Symbol *sym = word ? find_symbol(doc, word) : NULL;
    if (!sym) {
        send_response(id_raw, "null");
        free(word);
        free(uri);
        return;
    }
    StrBuf sb;
    sb_init(&sb);
    sb_append(&sb, "{\"uri\":");
    sb_append_json_str(&sb, doc->uri);
    sb_append(&sb, ",\"range\":{\"start\":{\"line\":");
    sb_append_int(&sb, sym->line);
    sb_append(&sb, ",\"character\":");
    sb_append_int(&sb, sym->col);
    sb_append(&sb, "},\"end\":{\"line\":");
    sb_append_int(&sb, sym->line);
    sb_append(&sb, ",\"character\":");
    sb_append_int(&sb, sym->end_col);
    sb_append(&sb, "}}}");
    send_response(id_raw, sb.data);
    free(sb.data);
    free(word);
    free(uri);
}

static void handle_completion(const char *id_raw, const char *params)
{
    static const char *const keywords[] = {
        "fun", "ext", "struct", "enum", "const", "import", "pub", "priv",
        "var", "as", "return", "if", "else", "match", "for", "break", "continue",
        "true", "false",
    };
    static const char *const types[] = {
        "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64", "f32", "f64", "bool",
    };
    static const char *const builtins[] = {"len", "data", "size_of"};

    const char *td = json_find_field(params, "textDocument");
    char *uri = json_get_string(td, "uri");
    Document *doc = uri ? find_document(uri) : NULL;

    StrBuf sb;
    sb_init(&sb);
    sb_append_char(&sb, '[');
    bool first = true;

    for (size_t i = 0; i < sizeof(keywords) / sizeof(keywords[0]); i++) {
        if (!first) sb_append_char(&sb, ',');
        first = false;
        sb_append(&sb, "{\"label\":");
        sb_append_json_str(&sb, keywords[i]);
        sb_append(&sb, ",\"kind\":14}"); // Keyword
    }
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
        sb_append(&sb, ",{\"label\":");
        sb_append_json_str(&sb, types[i]);
        sb_append(&sb, ",\"kind\":25}"); // TypeParameter
    }
    for (size_t i = 0; i < sizeof(builtins) / sizeof(builtins[0]); i++) {
        sb_append(&sb, ",{\"label\":");
        sb_append_json_str(&sb, builtins[i]);
        sb_append(&sb, ",\"kind\":3}"); // Function
    }
    if (doc) {
        for (size_t i = 0; i < doc->symbol_count; i++) {
            int comp_kind = doc->symbols[i].kind == 12 ? 3 :
                            doc->symbols[i].kind == 23 ? 22 :
                            doc->symbols[i].kind == 10 ? 13 : 21;
            sb_append(&sb, ",{\"label\":");
            sb_append_json_str(&sb, doc->symbols[i].name);
            sb_append(&sb, ",\"kind\":");
            sb_append_int(&sb, comp_kind);
            sb_append(&sb, ",\"detail\":");
            sb_append_json_str(&sb, doc->symbols[i].detail);
            sb_append_char(&sb, '}');
        }
    }
    sb_append_char(&sb, ']');
    send_response(id_raw, sb.data);
    free(sb.data);
    free(uri);
}

static void dispatch_message(const char *msg)
{
    char *method = json_get_string(msg, "method");
    if (!method) {
        return;
    }
    char *id_raw = json_get_raw(msg, "id");
    const char *params = json_find_field(msg, "params");

    if (strcmp(method, "initialize") == 0) {
        handle_initialize(id_raw);
    } else if (strcmp(method, "shutdown") == 0) {
        shutdown_requested = true;
        send_response(id_raw, "null");
    } else if (strcmp(method, "exit") == 0) {
        should_exit = true;
    } else if (strcmp(method, "textDocument/didOpen") == 0) {
        const char *td = json_find_field(params, "textDocument");
        char *uri = json_get_string(td, "uri");
        char *text = json_get_string(td, "text");
        if (uri && text) {
            Document *doc = upsert_document(uri, text);
            if (doc) publish_diagnostics(doc);
        }
        free(uri);
        free(text);
    } else if (strcmp(method, "textDocument/didChange") == 0) {
        const char *td = json_find_field(params, "textDocument");
        const char *changes = json_find_field(params, "contentChanges");
        char *uri = json_get_string(td, "uri");
        if (uri && changes) {
            const char *p = skip_ws(changes);
            if (*p == '[') {
                p = skip_ws(p + 1);
                char *text = json_get_string(p, "text");
                if (text) {
                    Document *doc = upsert_document(uri, text);
                    if (doc) publish_diagnostics(doc);
                    free(text);
                }
            }
        }
        free(uri);
    } else if (strcmp(method, "textDocument/didSave") == 0) {
        const char *td = json_find_field(params, "textDocument");
        char *uri = json_get_string(td, "uri");
        char *text = json_get_string(params, "text");
        if (uri) {
            Document *doc = text ? upsert_document(uri, text) : find_document(uri);
            if (doc) publish_diagnostics(doc);
        }
        free(uri);
        free(text);
    } else if (strcmp(method, "textDocument/didClose") == 0) {
        const char *td = json_find_field(params, "textDocument");
        char *uri = json_get_string(td, "uri");
        if (uri) remove_document(uri);
        free(uri);
    } else if (strcmp(method, "textDocument/documentSymbol") == 0) {
        handle_document_symbol(id_raw, params);
    } else if (strcmp(method, "textDocument/hover") == 0) {
        handle_hover(id_raw, params);
    } else if (strcmp(method, "textDocument/definition") == 0) {
        handle_definition(id_raw, params);
    } else if (strcmp(method, "textDocument/completion") == 0) {
        handle_completion(id_raw, params);
    } else if (id_raw) {
        send_response(id_raw, "null");
    }

    free(id_raw);
    free(method);
}

int main(void)
{
    while (!should_exit) {
        char header[256];
        size_t content_length = 0;
        bool has_header = false;
        while (fgets(header, sizeof(header), stdin)) {
            has_header = true;
            if (strcmp(header, "\r\n") == 0 || strcmp(header, "\n") == 0) {
                break;
            }
            if (strncmp(header, "Content-Length:", 15) == 0) {
                content_length = (size_t)strtoull(header + 15, NULL, 10);
            }
        }
        if (!has_header || feof(stdin)) {
            break;
        }
        if (content_length == 0) {
            continue;
        }
        char *body = malloc(content_length + 1);
        if (!body) {
            break;
        }
        size_t read_bytes = fread(body, 1, content_length, stdin);
        body[read_bytes] = '\0';
        if (read_bytes == content_length) {
            dispatch_message(body);
        }
        free(body);
    }

    for (size_t i = 0; i < document_count; i++) {
        free(documents[i].uri);
        free(documents[i].path);
        free(documents[i].text);
        free_symbols(&documents[i]);
    }
    free(documents);
    return shutdown_requested ? 0 : 0;
}
