#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <dirent.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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
    bool is_pub;
} Symbol;

typedef struct {
    char *module_path; // e.g. "std.io" or "nested.ops"
    char *alias;       // bound name, e.g. "io" or "ops" or "nops"
    bool wildcard;
    bool selective;
    char **items;      // imported symbol names (for selective)
    char **item_aliases;
    size_t item_count;
} Import;

typedef struct {
    char *uri;
    char *path;
    char *text;
    size_t len;
    Symbol *symbols;
    size_t symbol_count;
    Import *imports;
    size_t import_count;
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
    if (!sb->data || n == 0) {
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

// --- URI, Paths, & File Loading ---

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

static char *path_to_uri(const char *path)
{
    StrBuf sb;
    sb_init(&sb);
    sb_append(&sb, "file://");
    for (const unsigned char *p = (const unsigned char *)path; *p; p++) {
        if (*p == ' ' || *p == '%' || *p == '#') {
            char esc[4];
            snprintf(esc, sizeof(esc), "%%%02X", *p);
            sb_append(&sb, esc);
        } else {
            sb_append_char(&sb, (char)*p);
        }
    }
    return sb.data;
}

static bool is_dir(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool is_file(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static char *dir_of(const char *path)
{
    const char *slash = strrchr(path, '/');
    if (!slash) {
        return strdup(".");
    }
    if (slash == path) {
        return strdup("/");
    }
    size_t n = (size_t)(slash - path);
    char *dir = malloc(n + 1);
    if (dir) {
        memcpy(dir, path, n);
        dir[n] = '\0';
    }
    return dir;
}

static char *resolve_std_dir(const char *doc_path)
{
    const char *env = getenv("LUNSO_STD_DIR");
    if (env && *env && is_dir(env)) {
        return strdup(env);
    }
    if (doc_path) {
        char *doc_dir = dir_of(doc_path);
        if (doc_dir) {
            StrBuf cand;
            sb_init(&cand);
            sb_append(&cand, doc_dir);
            sb_append(&cand, "/std");
            free(doc_dir);
            if (is_dir(cand.data)) {
                return cand.data;
            }
            free(cand.data);
        }
    }
    const char *home = getenv("HOME");
    if (home) {
        StrBuf cand;
        sb_init(&cand);
        sb_append(&cand, home);
        sb_append(&cand, "/projects/lunso-boot/std");
        if (is_dir(cand.data)) {
            return cand.data;
        }
        free(cand.data);
    }
    return NULL;
}

static char *resolve_module_file(const char *doc_path, const char *mod_path)
{
    if (!mod_path || !*mod_path) {
        return NULL;
    }
    StrBuf rel;
    sb_init(&rel);
    const char *rest = mod_path;
    char *base_dir = NULL;

    if (strncmp(mod_path, "std.", 4) == 0) {
        char *std_dir = resolve_std_dir(doc_path);
        if (std_dir) {
            base_dir = std_dir;
            rest = mod_path + 4;
        }
    }
    if (!base_dir) {
        base_dir = dir_of(doc_path ? doc_path : ".");
    }

    sb_append(&rel, base_dir);
    sb_append_char(&rel, '/');
    for (const char *p = rest; *p; p++) {
        sb_append_char(&rel, *p == '.' ? '/' : *p);
    }
    sb_append(&rel, ".lunso");
    free(base_dir);

    if (is_file(rel.data)) {
        return rel.data;
    }
    free(rel.data);
    return NULL;
}

static char *resolve_module_dir(const char *doc_path, const char *mod_prefix)
{
    if (!mod_prefix || !*mod_prefix) {
        return dir_of(doc_path ? doc_path : ".");
    }
    if (strcmp(mod_prefix, "std") == 0) {
        char *std_dir = resolve_std_dir(doc_path);
        if (std_dir) {
            return std_dir;
        }
    }
    const char *rest = mod_prefix;
    char *base_dir = NULL;
    if (strncmp(mod_prefix, "std.", 4) == 0) {
        char *std_dir = resolve_std_dir(doc_path);
        if (std_dir) {
            base_dir = std_dir;
            rest = mod_prefix + 4;
        }
    }
    if (!base_dir) {
        base_dir = dir_of(doc_path ? doc_path : ".");
    }
    StrBuf dir;
    sb_init(&dir);
    sb_append(&dir, base_dir);
    sb_append_char(&dir, '/');
    for (const char *p = rest; *p; p++) {
        sb_append_char(&dir, *p == '.' ? '/' : *p);
    }
    free(base_dir);
    if (is_dir(dir.data)) {
        return dir.data;
    }
    free(dir.data);
    return NULL;
}

static char *read_file_text(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long size = ftell(f);
    if (size < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    char *buf = malloc((size_t)size + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t n = fread(buf, 1, (size_t)size, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

// --- Symbol & Import Scanning ---

static bool is_ident_start(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static bool is_ident_char(unsigned char c)
{
    return is_ident_start(c) || (c >= '0' && c <= '9');
}

static void free_symbol_list(Symbol *syms, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        free(syms[i].name);
        free(syms[i].detail);
    }
    free(syms);
}

static void free_imports(Document *doc)
{
    for (size_t i = 0; i < doc->import_count; i++) {
        free(doc->imports[i].module_path);
        free(doc->imports[i].alias);
        for (size_t j = 0; j < doc->imports[i].item_count; j++) {
            free(doc->imports[i].items[j]);
            free(doc->imports[i].item_aliases[j]);
        }
        free(doc->imports[i].items);
        free(doc->imports[i].item_aliases);
    }
    free(doc->imports);
    doc->imports = NULL;
    doc->import_count = 0;
}

static void add_symbol_to(Symbol **list, size_t *count, const char *name, size_t name_len,
                          const char *detail, size_t detail_len,
                          int kind, int line, int col, bool is_pub)
{
    Symbol *grown = realloc(*list, (*count + 1) * sizeof(*grown));
    if (!grown) {
        return;
    }
    *list = grown;
    Symbol *sym = &(*list)[(*count)++];
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
    sym->is_pub = is_pub;
}

static void scan_symbols_in_text(const char *text, Symbol **out_syms, size_t *out_count)
{
    *out_syms = NULL;
    *out_count = 0;
    const char *p = text;
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
        if (cur < end && *cur != ' ' && *cur != '\t') {
            bool is_pub = false;
            if (end - cur >= 4 && strncmp(cur, "pub ", 4) == 0) {
                is_pub = true;
                cur += 4;
                while (cur < end && (*cur == ' ' || *cur == '\t')) cur++;
            } else if (end - cur >= 5 && strncmp(cur, "priv ", 5) == 0) {
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
                    add_symbol_to(out_syms, out_count, name_start, (size_t)(cur - name_start),
                                  line_start, line_len, kind, line,
                                  (int)(name_start - line_start), is_pub);
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

static void scan_imports(Document *doc)
{
    free_imports(doc);
    const char *p = doc->text;
    while (*p) {
        const char *line_start = p;
        const char *eol = strchr(p, '\n');
        size_t line_len = eol ? (size_t)(eol - p) : strlen(p);
        if (line_len > 0 && line_start[line_len - 1] == '\r') {
            line_len--;
        }
        const char *cur = line_start;
        const char *end = line_start + line_len;
        while (cur < end && (*cur == ' ' || *cur == '\t')) cur++;
        if (end - cur >= 7 && strncmp(cur, "import ", 7) == 0) {
            cur += 7;
            while (cur < end && (*cur == ' ' || *cur == '\t')) cur++;
            StrBuf path;
            sb_init(&path);
            bool wildcard = false;
            const char *last_seg = cur;
            size_t last_len = 0;
            while (cur < end) {
                if (is_ident_start((unsigned char)*cur)) {
                    const char *seg = cur;
                    while (cur < end && is_ident_char((unsigned char)*cur)) cur++;
                    if (path.len > 0) {
                        sb_append_char(&path, '.');
                    }
                    sb_append_n(&path, seg, (size_t)(cur - seg));
                    last_seg = seg;
                    last_len = (size_t)(cur - seg);
                    if (cur < end && *cur == '.') {
                        cur++;
                        if (cur < end && *cur == '*') {
                            wildcard = true;
                            cur++;
                            break;
                        }
                        continue;
                    }
                }
                break;
            }
            if (path.len > 0) {
                Import imp = {0};
                imp.module_path = path.data;
                imp.wildcard = wildcard;
                while (cur < end && (*cur == ' ' || *cur == '\t')) cur++;
                if (!wildcard && end - cur >= 3 && strncmp(cur, "as ", 3) == 0) {
                    cur += 3;
                    while (cur < end && (*cur == ' ' || *cur == '\t')) cur++;
                    const char *a = cur;
                    while (cur < end && is_ident_char((unsigned char)*cur)) cur++;
                    if (cur > a) {
                        imp.alias = malloc((size_t)(cur - a) + 1);
                        memcpy(imp.alias, a, (size_t)(cur - a));
                        imp.alias[cur - a] = '\0';
                    }
                } else if (!wildcard && cur < end && *cur == '{') {
                    imp.selective = true;
                    cur++;
                    while (cur < end && *cur != '}') {
                        while (cur < end && (*cur == ' ' || *cur == '\t' || *cur == ',')) cur++;
                        if (cur >= end || *cur == '}') break;
                        const char *n0 = cur;
                        while (cur < end && is_ident_char((unsigned char)*cur)) cur++;
                        if (cur == n0) break;
                        size_t nlen = (size_t)(cur - n0);
                        const char *a0 = n0;
                        size_t alen = nlen;
                        while (cur < end && (*cur == ' ' || *cur == '\t')) cur++;
                        if (end - cur >= 3 && strncmp(cur, "as ", 3) == 0) {
                            cur += 3;
                            while (cur < end && (*cur == ' ' || *cur == '\t')) cur++;
                            const char *aa = cur;
                            while (cur < end && is_ident_char((unsigned char)*cur)) cur++;
                            if (cur > aa) {
                                a0 = aa;
                                alen = (size_t)(cur - aa);
                            }
                        }
                        imp.items = realloc(imp.items, (imp.item_count + 1) * sizeof(char *));
                        imp.item_aliases = realloc(imp.item_aliases, (imp.item_count + 1) * sizeof(char *));
                        imp.items[imp.item_count] = malloc(nlen + 1);
                        memcpy(imp.items[imp.item_count], n0, nlen);
                        imp.items[imp.item_count][nlen] = '\0';
                        imp.item_aliases[imp.item_count] = malloc(alen + 1);
                        memcpy(imp.item_aliases[imp.item_count], a0, alen);
                        imp.item_aliases[imp.item_count][alen] = '\0';
                        imp.item_count++;
                    }
                }
                if (!imp.alias && !wildcard && !imp.selective && last_len > 0) {
                    imp.alias = malloc(last_len + 1);
                    memcpy(imp.alias, last_seg, last_len);
                    imp.alias[last_len] = '\0';
                }
                Import *grown = realloc(doc->imports, (doc->import_count + 1) * sizeof(*grown));
                if (grown) {
                    doc->imports = grown;
                    doc->imports[doc->import_count++] = imp;
                } else {
                    free(imp.module_path);
                    free(imp.alias);
                }
            } else {
                free(path.data);
            }
        }
        if (!eol) {
            break;
        }
        p = eol + 1;
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
        free_symbol_list(doc->symbols, doc->symbol_count);
        doc->symbols = NULL;
        doc->symbol_count = 0;
    }
    doc->text = strdup(text ? text : "");
    doc->len = strlen(doc->text);
    scan_symbols_in_text(doc->text, &doc->symbols, &doc->symbol_count);
    scan_imports(doc);
    return doc;
}

static void remove_document(const char *uri)
{
    for (size_t i = 0; i < document_count; i++) {
        if (strcmp(documents[i].uri, uri) == 0) {
            free(documents[i].uri);
            free(documents[i].path);
            free(documents[i].text);
            free_symbol_list(documents[i].symbols, documents[i].symbol_count);
            free_imports(&documents[i]);
            documents[i] = documents[--document_count];
            return;
        }
    }
}

static bool load_module_symbols(const char *doc_path, const char *mod_path,
                                char **out_uri, Symbol **out_syms, size_t *out_count)
{
    char *file_path = resolve_module_file(doc_path, mod_path);
    if (!file_path) {
        return false;
    }
    char *uri = path_to_uri(file_path);
    Document *open_doc = uri ? find_document(uri) : NULL;
    if (open_doc) {
        *out_uri = uri;
        *out_syms = NULL;
        *out_count = 0;
        for (size_t i = 0; i < open_doc->symbol_count; i++) {
            Symbol *s = &open_doc->symbols[i];
            add_symbol_to(out_syms, out_count, s->name, strlen(s->name),
                          s->detail, strlen(s->detail), s->kind, s->line, s->col, s->is_pub);
        }
        free(file_path);
        return true;
    }
    char *text = read_file_text(file_path);
    free(file_path);
    if (!text) {
        free(uri);
        return false;
    }
    scan_symbols_in_text(text, out_syms, out_count);
    free(text);
    *out_uri = uri;
    return true;
}

static const char *resolve_imported_module(const Document *doc, const char *qualifier)
{
    for (size_t i = 0; i < doc->import_count; i++) {
        const Import *imp = &doc->imports[i];
        if (imp->alias && strcmp(imp->alias, qualifier) == 0) {
            return imp->module_path;
        }
        // Handle `import std` followed by `std.io.println` -> qualifier "std.io"
        if (imp->alias && strncmp(qualifier, imp->alias, strlen(imp->alias)) == 0 &&
            qualifier[strlen(imp->alias)] == '.') {
            return qualifier;
        }
    }
    return NULL;
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
                size_t path_len = strlen(tmp_path);
                const char *after = NULL;
                if (strncmp(line_buf, tmp_path, path_len) == 0 && line_buf[path_len] == ':') {
                    after = line_buf + path_len + 1;
                } else {
                    continue;
                }
                char *end1 = NULL;
                long ln = strtol(after, &end1, 10);
                if (!end1 || *end1 != ':') continue;
                char *end2 = NULL;
                long col = strtol(end1 + 1, &end2, 10);
                if (!end2 || *end2 != ':') continue;
                const char *msg = end2 + 1;
                while (*msg == ' ') msg++;
                long lsp_line = ln > 0 ? ln - 1 : 0;
                long lsp_col = col > 0 ? col - 1 : 0;

                if (!first) sb_append_char(&diags, ',');
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

// --- Symbol Lookup Across Current & Imported Modules ---

static const char *line_at(const Document *doc, long long target_line, size_t *out_len)
{
    const char *p = doc->text;
    long long line = 0;
    while (*p && line < target_line) {
        if (*p == '\n') line++;
        p++;
    }
    if (line != target_line) {
        return NULL;
    }
    const char *eol = strchr(p, '\n');
    size_t len = eol ? (size_t)(eol - p) : strlen(p);
    if (len > 0 && p[len - 1] == '\r') len--;
    *out_len = len;
    return p;
}

static bool ident_and_qualifier_at(const Document *doc, long long target_line,
                                   long long target_col, char **out_word, char **out_qual)
{
    *out_word = NULL;
    *out_qual = NULL;
    size_t line_len = 0;
    const char *p = line_at(doc, target_line, &line_len);
    if (!p || target_col < 0 || (size_t)target_col >= line_len) {
        return false;
    }
    size_t pos = (size_t)target_col;
    if (!is_ident_char((unsigned char)p[pos])) {
        if (pos > 0 && is_ident_char((unsigned char)p[pos - 1])) {
            pos--;
        } else {
            return false;
        }
    }
    size_t start = pos;
    while (start > 0 && is_ident_char((unsigned char)p[start - 1])) start--;
    size_t end = pos + 1;
    while (end < line_len && is_ident_char((unsigned char)p[end])) end++;

    *out_word = malloc(end - start + 1);
    memcpy(*out_word, p + start, end - start);
    (*out_word)[end - start] = '\0';

    if (start >= 2 && p[start - 1] == '.' && is_ident_char((unsigned char)p[start - 2])) {
        size_t qend = start - 1;
        size_t qstart = qend;
        while (qstart > 0 && (is_ident_char((unsigned char)p[qstart - 1]) || p[qstart - 1] == '.')) {
            qstart--;
        }
        *out_qual = malloc(qend - qstart + 1);
        memcpy(*out_qual, p + qstart, qend - qstart);
        (*out_qual)[qend - qstart] = '\0';
    }
    return true;
}

static const Symbol *find_symbol_in(const Symbol *syms, size_t count, const char *name, bool pub_only)
{
    for (size_t i = 0; i < count; i++) {
        if ((!pub_only || syms[i].is_pub) && strcmp(syms[i].name, name) == 0) {
            return &syms[i];
        }
    }
    return NULL;
}

static bool resolve_symbol_target(const Document *doc, const char *word, const char *qual,
                                  char **out_uri, Symbol *out_sym)
{
    if (qual) {
        const char *mod_path = resolve_imported_module(doc, qual);
        if (!mod_path) {
            return false;
        }
        char *mod_uri = NULL;
        Symbol *mod_syms = NULL;
        size_t mod_count = 0;
        if (!load_module_symbols(doc->path, mod_path, &mod_uri, &mod_syms, &mod_count)) {
            return false;
        }
        const Symbol *found = find_symbol_in(mod_syms, mod_count, word, true);
        if (found) {
            *out_uri = mod_uri;
            *out_sym = *found;
            out_sym->name = strdup(found->name);
            out_sym->detail = strdup(found->detail);
            free_symbol_list(mod_syms, mod_count);
            return true;
        }
        free(mod_uri);
        free_symbol_list(mod_syms, mod_count);
        return false;
    }

    const Symbol *local = find_symbol_in(doc->symbols, doc->symbol_count, word, false);
    if (local) {
        *out_uri = strdup(doc->uri);
        *out_sym = *local;
        out_sym->name = strdup(local->name);
        out_sym->detail = strdup(local->detail);
        return true;
    }

    for (size_t i = 0; i < doc->import_count; i++) {
        const Import *imp = &doc->imports[i];
        const char *lookup = NULL;
        if (imp->wildcard) {
            lookup = word;
        } else if (imp->selective) {
            for (size_t j = 0; j < imp->item_count; j++) {
                if (strcmp(imp->item_aliases[j], word) == 0) {
                    lookup = imp->items[j];
                    break;
                }
            }
        }
        if (!lookup) {
            continue;
        }
        char *mod_uri = NULL;
        Symbol *mod_syms = NULL;
        size_t mod_count = 0;
        if (load_module_symbols(doc->path, imp->module_path, &mod_uri, &mod_syms, &mod_count)) {
            const Symbol *found = find_symbol_in(mod_syms, mod_count, lookup, true);
            if (found) {
                *out_uri = mod_uri;
                *out_sym = *found;
                out_sym->name = strdup(found->name);
                out_sym->detail = strdup(found->detail);
                free_symbol_list(mod_syms, mod_count);
                return true;
            }
            free(mod_uri);
            free_symbol_list(mod_syms, mod_count);
        }
    }
    return false;
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
        "},\"serverInfo\":{\"name\":\"lunso-lsp\",\"version\":\"0.2.0\"}}");
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

    char *word = NULL;
    char *qual = NULL;
    if (!ident_and_qualifier_at(doc, line, col, &word, &qual)) {
        send_response(id_raw, "null");
        free(uri);
        return;
    }

    char *target_uri = NULL;
    Symbol sym = {0};
    const char *detail = NULL;
    bool resolved = resolve_symbol_target(doc, word, qual, &target_uri, &sym);
    if (resolved) {
        detail = sym.detail;
    } else if (!qual) {
        detail = builtin_doc(word);
    }

    if (!detail) {
        send_response(id_raw, "null");
    } else {
        StrBuf md;
        sb_init(&md);
        sb_append(&md, "```lunso\n");
        sb_append(&md, detail);
        sb_append(&md, "\n```");

        StrBuf resp;
        sb_init(&resp);
        sb_append(&resp, "{\"contents\":{\"kind\":\"markdown\",\"value\":");
        sb_append_json_str(&resp, md.data);
        sb_append(&resp, "}}");
        send_response(id_raw, resp.data);
        free(resp.data);
        free(md.data);
    }

    free(target_uri);
    free(sym.name);
    free(sym.detail);
    free(word);
    free(qual);
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
    char *word = NULL;
    char *qual = NULL;
    char *target_uri = NULL;
    Symbol sym = {0};
    if (!ident_and_qualifier_at(doc, line, col, &word, &qual) ||
        !resolve_symbol_target(doc, word, qual, &target_uri, &sym)) {
        send_response(id_raw, "null");
    } else {
        StrBuf sb;
        sb_init(&sb);
        sb_append(&sb, "{\"uri\":");
        sb_append_json_str(&sb, target_uri);
        sb_append(&sb, ",\"range\":{\"start\":{\"line\":");
        sb_append_int(&sb, sym.line);
        sb_append(&sb, ",\"character\":");
        sb_append_int(&sb, sym.col);
        sb_append(&sb, "},\"end\":{\"line\":");
        sb_append_int(&sb, sym.line);
        sb_append(&sb, ",\"character\":");
        sb_append_int(&sb, sym.end_col);
        sb_append(&sb, "}}}");
        send_response(id_raw, sb.data);
        free(sb.data);
    }
    free(target_uri);
    free(sym.name);
    free(sym.detail);
    free(word);
    free(qual);
    free(uri);
}

static void append_completion_item(StrBuf *sb, bool *first, const char *label,
                                   int kind, const char *detail)
{
    if (!*first) sb_append_char(sb, ',');
    *first = false;
    sb_append(sb, "{\"label\":");
    sb_append_json_str(sb, label);
    sb_append(sb, ",\"kind\":");
    sb_append_int(sb, kind);
    if (detail) {
        sb_append(sb, ",\"detail\":");
        sb_append_json_str(sb, detail);
    }
    sb_append_char(sb, '}');
}

static int symbol_to_completion_kind(int sym_kind)
{
    return sym_kind == 12 ? 3 : sym_kind == 23 ? 22 : sym_kind == 10 ? 13 : 21;
}

static void append_module_dir_completions(StrBuf *sb, bool *first, const char *doc_path,
                                          const char *prefix)
{
    char *dir_path = resolve_module_dir(doc_path, prefix);
    if (!dir_path) {
        return;
    }
    DIR *d = opendir(dir_path);
    if (!d) {
        free(dir_path);
        return;
    }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        StrBuf full;
        sb_init(&full);
        sb_append(&full, dir_path);
        sb_append_char(&full, '/');
        sb_append(&full, ent->d_name);
        size_t nlen = strlen(ent->d_name);
        if (is_dir(full.data)) {
            append_completion_item(sb, first, ent->d_name, 19, "module folder"); // Folder
        } else if (nlen > 6 && strcmp(ent->d_name + nlen - 6, ".lunso") == 0) {
            char *mod_name = malloc(nlen - 5);
            memcpy(mod_name, ent->d_name, nlen - 6);
            mod_name[nlen - 6] = '\0';
            append_completion_item(sb, first, mod_name, 9, "lunso module"); // Module
            free(mod_name);
        }
        free(full.data);
    }
    closedir(d);
    free(dir_path);
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
    const char *pos = json_find_field(params, "position");
    char *uri = json_get_string(td, "uri");
    Document *doc = uri ? find_document(uri) : NULL;
    long long line = 0, col = 0;
    bool has_pos = doc && json_get_int(pos, "line", &line) && json_get_int(pos, "character", &col);

    StrBuf sb;
    sb_init(&sb);
    sb_append_char(&sb, '[');
    bool first = true;

    if (has_pos) {
        size_t line_len = 0;
        const char *lp = line_at(doc, line, &line_len);
        size_t cursor = (lp && col >= 0) ? ((size_t)col < line_len ? (size_t)col : line_len) : 0;
        const char *trimmed = lp;
        if (lp) {
            while (trimmed < lp + cursor && (*trimmed == ' ' || *trimmed == '\t')) trimmed++;
        }

        // Case 1: `import ...` completion
        if (lp && (size_t)(lp + cursor - trimmed) >= 7 && strncmp(trimmed, "import ", 7) == 0) {
            const char *after_imp = trimmed + 7;
            while (after_imp < lp + cursor && (*after_imp == ' ' || *after_imp == '\t')) after_imp++;
            const char *end = lp + cursor;
            size_t rem = (after_imp < end) ? (size_t)(end - after_imp) : 0;
            const char *brace = rem ? memchr(after_imp, '{', rem) : NULL;
            if (brace) {
                // `import mod { ...` -> list pub symbols of `mod`
                const char *mend = brace;
                while (mend > after_imp && (mend[-1] == ' ' || mend[-1] == '\t')) mend--;
                char *mod_path = malloc((size_t)(mend - after_imp) + 1);
                memcpy(mod_path, after_imp, (size_t)(mend - after_imp));
                mod_path[mend - after_imp] = '\0';
                char *mod_uri = NULL;
                Symbol *mod_syms = NULL;
                size_t mod_count = 0;
                if (load_module_symbols(doc->path, mod_path, &mod_uri, &mod_syms, &mod_count)) {
                    for (size_t i = 0; i < mod_count; i++) {
                        if (mod_syms[i].is_pub) {
                            append_completion_item(&sb, &first, mod_syms[i].name,
                                                   symbol_to_completion_kind(mod_syms[i].kind),
                                                   mod_syms[i].detail);
                        }
                    }
                    free(mod_uri);
                    free_symbol_list(mod_syms, mod_count);
                }
                free(mod_path);
            } else {
                // `import std.` or `import folder.` or `import `
                const char *dot = NULL;
                for (const char *q = lp + cursor; q > after_imp; q--) {
                    if (q[-1] == '.') {
                        dot = q - 1;
                        break;
                    }
                }
                if (dot) {
                    size_t plen = (size_t)(dot - after_imp);
                    char *prefix = malloc(plen + 1);
                    memcpy(prefix, after_imp, plen);
                    prefix[plen] = '\0';
                    append_module_dir_completions(&sb, &first, doc->path, prefix);
                    char *mod_file = resolve_module_file(doc->path, prefix);
                    if (mod_file) {
                        append_completion_item(&sb, &first, "*", 14, "import all public symbols");
                        free(mod_file);
                    }
                    free(prefix);
                } else {
                    char *std_dir = resolve_std_dir(doc->path);
                    if (std_dir) {
                        append_completion_item(&sb, &first, "std", 9, "Lunso standard library");
                        free(std_dir);
                    }
                    append_module_dir_completions(&sb, &first, doc->path, "");
                }
            }
            sb_append_char(&sb, ']');
            send_response(id_raw, sb.data);
            free(sb.data);
            free(uri);
            return;
        }

        // Case 2: member completion `<module>.`
        if (lp && cursor > 0) {
            size_t scan = cursor;
            while (scan > 0 && is_ident_char((unsigned char)lp[scan - 1])) scan--;
            if (scan > 0 && lp[scan - 1] == '.') {
                size_t qend = scan - 1;
                size_t qstart = qend;
                while (qstart > 0 && (is_ident_char((unsigned char)lp[qstart - 1]) || lp[qstart - 1] == '.')) {
                    qstart--;
                }
                if (qend > qstart) {
                    char *qual = malloc(qend - qstart + 1);
                    memcpy(qual, lp + qstart, qend - qstart);
                    qual[qend - qstart] = '\0';
                    const char *mod_path = resolve_imported_module(doc, qual);
                    if (mod_path) {
                        char *mod_uri = NULL;
                        Symbol *mod_syms = NULL;
                        size_t mod_count = 0;
                        if (load_module_symbols(doc->path, mod_path, &mod_uri, &mod_syms, &mod_count)) {
                            for (size_t i = 0; i < mod_count; i++) {
                                if (mod_syms[i].is_pub) {
                                    append_completion_item(&sb, &first, mod_syms[i].name,
                                                           symbol_to_completion_kind(mod_syms[i].kind),
                                                           mod_syms[i].detail);
                                }
                            }
                            free(mod_uri);
                            free_symbol_list(mod_syms, mod_count);
                        }
                        free(qual);
                        sb_append_char(&sb, ']');
                        send_response(id_raw, sb.data);
                        free(sb.data);
                        free(uri);
                        return;
                    }
                    free(qual);
                }
            }
        }
    }

    // Case 3: General identifier completion
    for (size_t i = 0; i < sizeof(keywords) / sizeof(keywords[0]); i++) {
        append_completion_item(&sb, &first, keywords[i], 14, NULL);
    }
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
        append_completion_item(&sb, &first, types[i], 25, NULL);
    }
    for (size_t i = 0; i < sizeof(builtins) / sizeof(builtins[0]); i++) {
        append_completion_item(&sb, &first, builtins[i], 3, builtin_doc(builtins[i]));
    }
    if (doc) {
        for (size_t i = 0; i < doc->symbol_count; i++) {
            append_completion_item(&sb, &first, doc->symbols[i].name,
                                   symbol_to_completion_kind(doc->symbols[i].kind),
                                   doc->symbols[i].detail);
        }
        for (size_t i = 0; i < doc->import_count; i++) {
            const Import *imp = &doc->imports[i];
            if (imp->alias) {
                append_completion_item(&sb, &first, imp->alias, 9, imp->module_path);
            } else if (imp->wildcard || imp->selective) {
                char *mod_uri = NULL;
                Symbol *mod_syms = NULL;
                size_t mod_count = 0;
                if (load_module_symbols(doc->path, imp->module_path, &mod_uri, &mod_syms, &mod_count)) {
                    for (size_t j = 0; j < mod_count; j++) {
                        if (!mod_syms[j].is_pub) continue;
                        if (imp->wildcard) {
                            append_completion_item(&sb, &first, mod_syms[j].name,
                                                   symbol_to_completion_kind(mod_syms[j].kind),
                                                   mod_syms[j].detail);
                        } else {
                            for (size_t k = 0; k < imp->item_count; k++) {
                                if (strcmp(imp->items[k], mod_syms[j].name) == 0) {
                                    append_completion_item(&sb, &first, imp->item_aliases[k],
                                                           symbol_to_completion_kind(mod_syms[j].kind),
                                                           mod_syms[j].detail);
                                }
                            }
                        }
                    }
                    free(mod_uri);
                    free_symbol_list(mod_syms, mod_count);
                }
            }
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
        free_symbol_list(documents[i].symbols, documents[i].symbol_count);
        free_imports(&documents[i]);
    }
    free(documents);
    return shutdown_requested ? 0 : 0;
}
