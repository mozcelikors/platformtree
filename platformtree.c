/*
 * platformtree.c — Linux Device Tree  Visualiser
 *
 * Recursively resolves #include directives, parses all DTS/DTSI nodes
 * and properties, and emits a self-contained interactive HTML tree.
 *
 * Usage:  ./platformtree <dts-folder> <main.dts> [devicetree-doc-folder]
 * Output: devicetree_viz.html
 * Build:  gcc -O2 -Wall -o platformtree platformtree.c
 *
 * Supports:
 *   - Recursive #include "..." resolution
 *   - Node labels (label: node@addr { })
 *   - Multiple labels per node
 *   - &label { } overlay merging
 *   - Boolean and valued properties
 *   - /delete-node/ and /delete-property/
 *   - Per-file colour coding in the output
 *   - Interactive &label reference navigation
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>
#include <dirent.h>

/* ═══════════════════════════════════════════════════════════
 *  Configuration[cite: 1]
 * ═════════════════════════════════════════════════════════*/

#define MAX_FILES   256
#define MAX_PATH    512
#define MAX_NAME    256
#define MAX_VALUE  8192
#define MAX_LABELS 4096
#define INIT_KIDS     8
#define INIT_BUF  (512 * 1024)

/* ═══════════════════════════════════════════════════════════
 *  Data Structures[cite: 1]
 * ═════════════════════════════════════════════════════════*/

typedef struct Prop {
    char         name[MAX_NAME];
    char        *value;       /* NULL = boolean property[cite: 1] */
    struct Prop *next;
} Prop;

typedef struct Node {
    char         name[MAX_NAME];
    char         unit_addr[64];
    char         label[MAX_NAME]; /* first label (most common case)[cite: 1] */
    int          file_idx;
    Prop        *props;
    int          prop_count;
    struct Node **kids;
    int          nkids, capkids;
    struct Node *parent;
    int          id;
} Node;

typedef struct {
    /* File registry[cite: 1] */
    char  fpaths[MAX_FILES][MAX_PATH];
    char  fnames[MAX_FILES][MAX_NAME];
    int   nfiles;
    char  base_dir[MAX_PATH];

    /* Parse results[cite: 1] */
    Node *root;
    int   next_id;
    int   total_props;

    /* Label lookup table[cite: 1] */
    char  lbl_key[MAX_LABELS][MAX_NAME];
    Node *lbl_val[MAX_LABELS];
    int   nlabels;

    /* Optional devicetree documentation folder for driver descriptions */
    char  doc_dir[MAX_PATH];
} Ctx;

/* ═══════════════════════════════════════════════════════════
 *  Utilities[cite: 1]
 * ═════════════════════════════════════════════════════════*/

static void die(const char *msg)
{
    fprintf(stderr, "platformtree: fatal: %s\n", msg);
    exit(1);
}

static void *xmalloc(size_t n)
{
    void *p = malloc(n);
    if (!p) die("out of memory");
    return p;
}

static void *xrealloc(void *p, size_t n)
{
    p = realloc(p, n);
    if (!p) die("out of memory");
    return p;
}

static char *xstrdup(const char *s)
{
    char *p = strdup(s);
    if (!p) die("out of memory");
    return p;
}

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    if (len < 0) { fclose(f); return NULL; }
    rewind(f);
    char *buf = xmalloc((size_t)len + 1);
    fread(buf, 1, (size_t)len, f);
    buf[len] = '\0';
    fclose(f);
    return buf;
}

/* ═══════════════════════════════════════════════════════════
 *  Context helpers[cite: 1]
 * ═════════════════════════════════════════════════════════*/

static int ctx_add_file(Ctx *ctx, const char *path)
{
    for (int i = 0; i < ctx->nfiles; i++)
        if (strcmp(ctx->fpaths[i], path) == 0) return i;
    if (ctx->nfiles >= MAX_FILES) return 0;
    int idx = ctx->nfiles++;
    strncpy(ctx->fpaths[idx], path, MAX_PATH - 1);
    const char *s = strrchr(path, '/');
    strncpy(ctx->fnames[idx], s ? s + 1 : path, MAX_NAME - 1);
    return idx;
}

static void ctx_find_file(Ctx *ctx, const char *inc, char *out)
{
    struct stat st;
    /* Try base_dir/inc  (handles relative and ../relative)[cite: 1] */
    snprintf(out, MAX_PATH, "%s/%s", ctx->base_dir, inc);
    if (stat(out, &st) == 0) return;
    /* Try just basename in base_dir[cite: 1] */
    const char *b = strrchr(inc, '/');
    if (b) {
        snprintf(out, MAX_PATH, "%s/%s", ctx->base_dir, b + 1);
        if (stat(out, &st) == 0) return;
    }
    /* Fallback – callers will warn on open failure[cite: 1] */
    snprintf(out, MAX_PATH, "%s/%s", ctx->base_dir, inc);
}

static void ctx_add_label(Ctx *ctx, const char *label, Node *node)
{
    if (ctx->nlabels >= MAX_LABELS) return;
    strncpy(ctx->lbl_key[ctx->nlabels], label, MAX_NAME - 1);
    ctx->lbl_val[ctx->nlabels] = node;
    ctx->nlabels++;
}

static Node *ctx_find_label(Ctx *ctx, const char *label)
{
    for (int i = 0; i < ctx->nlabels; i++)
        if (strcmp(ctx->lbl_key[i], label) == 0) return ctx->lbl_val[i];
    return NULL;
}

/* ═══════════════════════════════════════════════════════════
 *  Growing string buffer[cite: 1]
 * ═════════════════════════════════════════════════════════*/

typedef struct { char *buf; size_t len, cap; } SBuf;

static void sb_init(SBuf *sb)
{
    sb->cap = INIT_BUF;
    sb->buf = xmalloc(sb->cap);
    sb->buf[0] = '\0';
    sb->len = 0;
}

static void sb_append(SBuf *sb, const char *s, size_t n)
{
    while (sb->len + n + 1 >= sb->cap) {
        sb->cap *= 2;
        sb->buf = xrealloc(sb->buf, sb->cap);
    }
    memcpy(sb->buf + sb->len, s, n);
    sb->len += n;
    sb->buf[sb->len] = '\0';
}

static void sb_appendc(SBuf *sb, char c) { sb_append(sb, &c, 1); }
static void sb_appends(SBuf *sb, const char *s) { sb_append(sb, s, strlen(s)); }

/* ═══════════════════════════════════════════════════════════
 *  Preprocessor[cite: 1]
 * ═════════════════════════════════════════════════════════*/

static void preprocess(Ctx *ctx, const char *path, SBuf *out, int depth)
{
    if (depth > 64) {
        fprintf(stderr, "Warning: include depth limit exceeded at %s\n", path);
        return;
    }

    char *content = read_file(path);
    if (!content) {
        fprintf(stderr, "Warning: cannot read '%s'\n", path);
        return;
    }

    int fidx = ctx_add_file(ctx, path);
    char marker[32];
    snprintf(marker, sizeof(marker), "\x01%d\x01\n", fidx);
    sb_appends(out, marker);

    const char *p = content;
    while (*p) {
        /* C++ single-line comment[cite: 1] */
        if (p[0] == '/' && p[1] == '/') {
            while (*p && *p != '\n') p++;
            continue;
        }
        /* C block comment – preserve newlines for line tracking[cite: 1] */
        if (p[0] == '/' && p[1] == '*') {
            p += 2;
            while (*p && !(p[0] == '*' && p[1] == '/')) {
                if (*p == '\n') sb_appendc(out, '\n');
                p++;
            }
            if (*p) p += 2;
            continue;
        }
        /* Preprocessor directive[cite: 1] */
        if (*p == '#') {
            p++;
            while (*p == ' ' || *p == '\t') p++;
            if (strncmp(p, "include", 7) == 0) {
                p += 7;
                while (*p == ' ' || *p == '\t') p++;
                char inc[MAX_PATH] = {0};
                int  angle = 0;
                if (*p == '"') {
                    p++;
                    int i = 0;
                    while (*p && *p != '"' && i < MAX_PATH - 1) inc[i++] = *p++;
                    if (*p == '"') p++;
                } else if (*p == '<') {
                    angle = 1; p++;
                    int i = 0;
                    while (*p && *p != '>' && i < MAX_PATH - 1) inc[i++] = *p++;
                    if (*p == '>') p++;
                }
                while (*p && *p != '\n') p++; /* consume rest of line[cite: 1] */

                if (!angle && inc[0]) {
                    char inc_path[MAX_PATH];
                    ctx_find_file(ctx, inc, inc_path);
                    preprocess(ctx, inc_path, out, depth + 1);
                    /* Re-emit marker so parser tracks return to this file[cite: 1] */
                    snprintf(marker, sizeof(marker), "\x01%d\x01\n", fidx);
                    sb_appends(out, marker);
                }
                /* Skip <dt-bindings/...> angle includes[cite: 1] */
            } else {
                /* Other directive (#define, #ifdef, …) – skip line[cite: 1] */
                while (*p && *p != '\n') p++;
            }
            continue;
        }
        sb_appendc(out, *p++);
    }

    free(content);
}

/* ═══════════════════════════════════════════════════════════
 *  Node factory[cite: 1]
 * ═════════════════════════════════════════════════════════*/

static Node *make_node(Ctx *ctx, const char *name, const char *addr, int fidx)
{
    Node *n = xmalloc(sizeof(Node));
    memset(n, 0, sizeof(Node));
    strncpy(n->name, name, MAX_NAME - 1);
    if (addr && addr[0]) strncpy(n->unit_addr, addr, 63);
    n->file_idx = fidx;
    n->id       = ctx->next_id++;
    n->capkids  = INIT_KIDS;
    n->kids     = xmalloc(n->capkids * sizeof(Node *));
    return n;
}

static void node_add_kid(Node *parent, Node *child)
{
    if (parent->nkids >= parent->capkids) {
        parent->capkids *= 2;
        parent->kids = xrealloc(parent->kids, parent->capkids * sizeof(Node *));
    }
    parent->kids[parent->nkids++] = child;
    child->parent = parent;
}

static void node_add_prop(Ctx *ctx, Node *n, const char *name, const char *value)
{
    Prop *pr = xmalloc(sizeof(Prop));
    memset(pr, 0, sizeof(Prop));
    strncpy(pr->name, name, MAX_NAME - 1);
    pr->value = (value && *value) ? xstrdup(value) : NULL;
    /* Append to end of list to preserve order[cite: 1] */
    if (!n->props) {
        n->props = pr;
    } else {
        Prop *tail = n->props;
        while (tail->next) tail = tail->next;
        tail->next = pr;
    }
    n->prop_count++;
    ctx->total_props++;
}

/* ═══════════════════════════════════════════════════════════
 *  Recursive-descent parser[cite: 1]
 * ═════════════════════════════════════════════════════════*/

typedef struct {
    const char *p, *end;
    int         cur_file;
    Ctx        *ctx;
} Parser;

/* Skip whitespace and embedded file markers[cite: 1] */
static void ps_skip(Parser *ps)
{
    while (ps->p < ps->end) {
        if ((unsigned char)*ps->p <= ' ') { ps->p++; continue; }
        if (*ps->p == '\x01') {
            ps->p++;
            int f = 0;
            while (ps->p < ps->end && *ps->p >= '0' && *ps->p <= '9')
                f = f * 10 + (*ps->p++ - '0');
            ps->cur_file = f;
            if (ps->p < ps->end && *ps->p == '\x01') ps->p++;
            if (ps->p < ps->end && *ps->p == '\n')   ps->p++;
            continue;
        }
        break;
    }
}

/*
 * Read a DTS identifier: letters, digits, _ - , . + # @
 * We read @unit-address as part of the node name and split later.[cite: 1]
 */
static int ps_ident(Parser *ps, char *out, int maxlen)
{
    ps_skip(ps);
    if (ps->p >= ps->end) return 0;
    char c0 = *ps->p;
    if (!isalnum((unsigned char)c0) && c0 != '_' && c0 != '#')
        return 0;
    const char *s = ps->p;
    while (ps->p < ps->end) {
        char c = *ps->p;
        if (isalnum((unsigned char)c) || c=='_' || c=='-' ||
            c==',' || c=='.' || c=='+' || c=='#' || c=='@')
            ps->p++;
        else
            break;
    }
    int n = (int)(ps->p - s);
    if (!n) return 0;
    if (n >= maxlen) n = maxlen - 1;
    memcpy(out, s, n);
    out[n] = '\0';
    return 1;
}

static int ps_eat(Parser *ps, char c)
{
    ps_skip(ps);
    if (ps->p < ps->end && *ps->p == c) { ps->p++; return 1; }
    return 0;
}

static int ps_peekc(Parser *ps)
{
    ps_skip(ps);
    return (ps->p < ps->end) ? (unsigned char)*ps->p : 0;
}

/* Read property value: everything up to ';' at brace depth 0[cite: 1] */
static char *ps_read_value(Parser *ps)
{
    SBuf sb; sb_init(&sb);
    ps_skip(ps);
    int depth = 0;
    while (ps->p < ps->end) {
        char c = *ps->p;
        /* Skip embedded file markers[cite: 1] */
        if (c == '\x01') {
            ps->p++;
            int f = 0;
            while (ps->p < ps->end && *ps->p >= '0' && *ps->p <= '9')
                f = f * 10 + (*ps->p++ - '0');
            ps->cur_file = f;
            if (ps->p < ps->end && *ps->p == '\x01') ps->p++;
            if (ps->p < ps->end && *ps->p == '\n')   ps->p++;
            continue;
        }
        if (c == '<' || c == '[') depth++;
        if (c == '>' || c == ']') { if (depth > 0) depth--; }
        if (c == ';' && depth == 0) break;
        if (c == '{' && depth == 0) break;
        sb_appendc(&sb, c);
        ps->p++;
    }
    /* Trim trailing whitespace[cite: 1] */
    while (sb.len > 0 && isspace((unsigned char)sb.buf[sb.len - 1]))
        sb.buf[--sb.len] = '\0';
    char *r = xstrdup(sb.buf);
    free(sb.buf);
    return r;
}

/* Forward declaration[cite: 1] */
static void parse_body(Parser *ps, Node *node);

static void parse_body(Parser *ps, Node *node)
{
    if (!ps_eat(ps, '{')) return;

    while (ps->p < ps->end) {
        int c = ps_peekc(ps);
        if (!c) break;
        if (c == '}') { ps->p++; ps_eat(ps, ';'); break; }

        /* /delete-node/ name;[cite: 1] */
        if (strncmp(ps->p, "/delete-node/", 13) == 0) {
            ps->p += 13;
            char tmp[MAX_NAME]; ps_ident(ps, tmp, MAX_NAME);
            ps_eat(ps, ';'); continue;
        }
        /* /delete-property/ name;[cite: 1] */
        if (strncmp(ps->p, "/delete-property/", 17) == 0) {
            ps->p += 17;
            char tmp[MAX_NAME]; ps_ident(ps, tmp, MAX_NAME);
            ps_eat(ps, ';'); continue;
        }

        /* Read first token[cite: 1] */
        char tok[MAX_NAME];
        if (!ps_ident(ps, tok, MAX_NAME)) { ps->p++; continue; }

        /*
         * Collect label(s) and the final name.
         * DTS allows:  lbl1: lbl2: node@addr { }[cite: 1]
         */
        char label[MAX_NAME] = "";
        char name[MAX_NAME];
        strncpy(name, tok, MAX_NAME - 1);

        ps_skip(ps);
        int ok = 1;
        while (ps->p < ps->end && *ps->p == ':') {
            ps->p++; /* consume ':'[cite: 1] */
            if (!label[0]) strncpy(label, name, MAX_NAME - 1);
            ps_skip(ps);
            if (!ps_ident(ps, name, MAX_NAME)) { ok = 0; break; }
            ps_skip(ps);
        }
        if (!ok) { ps->p++; continue; }

        /* Split name@unit-address[cite: 1] */
        char addr[64] = "";
        char *at = strchr(name, '@');
        if (at) { strncpy(addr, at + 1, 63); *at = '\0'; }

        c = ps_peekc(ps);

        if (c == '{') {
            /* ── Child node ──[cite: 1] */
            Node *child = make_node(ps->ctx, name, addr, ps->cur_file);
            if (label[0]) {
                strncpy(child->label, label, MAX_NAME - 1);
                ctx_add_label(ps->ctx, label, child);
            }
            node_add_kid(node, child);
            parse_body(ps, child);

        } else if (c == '=') {
            /* ── Property with value ──[cite: 1] */
            ps->p++;
            char *val = ps_read_value(ps);
            ps_eat(ps, ';');
            node_add_prop(ps->ctx, node, name, val);
            free(val);

        } else if (c == ';') {
            /* ── Boolean / empty property ──[cite: 1] */
            ps->p++;
            node_add_prop(ps->ctx, node, name, NULL);

        } else {
            /* Unknown – skip to next semicolon or closing brace[cite: 1] */
            while (ps->p < ps->end && *ps->p != ';' && *ps->p != '}')
                ps->p++;
            ps_eat(ps, ';');
        }
    }
}

static void parse_toplevel(Parser *ps)
{
    Ctx *ctx = ps->ctx;
    while (ps->p < ps->end) {
        ps_skip(ps);
        if (ps->p >= ps->end) break;

        /* /dts-v1/;[cite: 1] */
        if (strncmp(ps->p, "/dts-v1/", 8) == 0) {
            ps->p += 8; ps_eat(ps, ';'); continue;
        }
        /* /memreserve/ addr size;[cite: 1] */
        if (strncmp(ps->p, "/memreserve/", 12) == 0) {
            ps->p += 12;
            while (ps->p < ps->end && *ps->p != ';') ps->p++;
            ps_eat(ps, ';'); continue;
        }
        /* Root node:  / { ... };[cite: 1] */
        if (*ps->p == '/') {
            ps->p++;
            ps_skip(ps);
            if (ps->p < ps->end && *ps->p == '{') {
                if (!ctx->root)
                    ctx->root = make_node(ctx, "/", NULL, ps->cur_file);
                parse_body(ps, ctx->root);
                continue;
            }
            /* Other /xxx/ directive – skip[cite: 1] */
            while (ps->p < ps->end && *ps->p != ';') ps->p++;
            ps_eat(ps, ';'); continue;
        }
        /* &label { ... }; – overlay / node extension[cite: 1] */
        if (*ps->p == '&') {
            ps->p++;
            char label[MAX_NAME];
            if (!ps_ident(ps, label, MAX_NAME)) { ps->p++; continue; }
            ps_skip(ps);
            if (ps->p < ps->end && *ps->p == '{') {
                Node *target = ctx_find_label(ctx, label);
                if (target) {
                    /* Merge properties/children into the existing node[cite: 1] */
                    parse_body(ps, target);
                } else {
                    /* Forward reference – create a placeholder[cite: 1] */
                    char oname[MAX_NAME + 2];
                    snprintf(oname, sizeof(oname), "&%s", label);
                    Node *ov = make_node(ctx, oname, NULL, ps->cur_file);
                    if (ctx->root) node_add_kid(ctx->root, ov);
                    parse_body(ps, ov);
                }
            }
            continue;
        }
        /* Unknown top-level token[cite: 1] */
        ps->p++;
    }
}

/* ═══════════════════════════════════════════════════════════
 *  Tree statistics[cite: 1]
 * ═════════════════════════════════════════════════════════*/

static int count_nodes(Node *n)
{
    int c = 1;
    for (int i = 0; i < n->nkids; i++) c += count_nodes(n->kids[i]);
    return c;
}

/* ═══════════════════════════════════════════════════════════
 *  HTML output helpers[cite: 1]
 * ═════════════════════════════════════════════════════════*/

static void html_esc(FILE *f, const char *s)
{
    if (!s) return;
    for (; *s; s++) {
        switch (*s) {
        case '<':  fputs("&lt;",   f); break;
        case '>':  fputs("&gt;",   f); break;
        case '&':  fputs("&amp;",  f); break;
        case '"':  fputs("&quot;", f); break;
        case '\'': fputs("&#39;",  f); break;
        default:   fputc(*s,       f); break;
        }
    }
}


/* ── JSON helpers for diagram data ── */

static void json_str(FILE *f, const char *s)
{
    if (!s) { fputs("\"\"", f); return; }
    fputc('"', f);
    for (; *s; s++) {
        if      (*s == '"')  fputs("\\\"", f);
        else if (*s == '\\') fputs("\\\\", f);
        else if (*s == '\n' || *s == '\r') fputc(' ', f);
        else if ((unsigned char)*s < 0x20) {}
        else fputc(*s, f);
    }
    fputc('"', f);
}

/* Emit array of node IDs this node's properties reference via &label */
static void emit_refs_json(FILE *f, Ctx *ctx, Node *n)
{
    int first = 1;
    for (Prop *pr = n->props; pr; pr = pr->next) {
        if (!pr->value) continue;
        const char *p = pr->value;
        while (*p) {
            if (*p == '&') {
                p++;
                char lbl[MAX_NAME]; int i = 0;
                while (*p && (isalnum((unsigned char)*p) || *p == '_'))
                    lbl[i++] = *p++;
                lbl[i] = '\0';
                if (lbl[0]) {
                    Node *tgt = ctx_find_label(ctx, lbl);
                    if (tgt && tgt != n && tgt->parent != n && n->parent != tgt) {
                        if (!first) fputc(',', f);
                        fprintf(f, "%d", tgt->id);
                        first = 0;
                    }
                }
            } else { p++; }
        }
    }
}

/* Recursively emit all nodes as JSON objects into a flat array */
static void emit_node_json(FILE *f, Ctx *ctx, Node *n, int *first)
{
    const char *compat = "";
    for (Prop *pr = n->props; pr; pr = pr->next)
        if (strcmp(pr->name, "compatible") == 0 && pr->value) compat = pr->value;

    int disabled = 0;
    for (Prop *pr = n->props; pr; pr = pr->next) {
        if (strcmp(pr->name, "status") == 0 && pr->value) {
            const char *v = pr->value;
            while (*v == '"' || *v == ' ') v++;
            disabled = (strncmp(v, "disabled", 8) == 0);
        }
    }

    if (!*first) fputs(",\n", f);
    *first = 0;

    fprintf(f, "{\"id\":%d,\"name\":", n->id);
    json_str(f, n->name);
    fprintf(f, ",\"addr\":");
    json_str(f, n->unit_addr);
    fprintf(f, ",\"lbl\":");
    json_str(f, n->label);
    fprintf(f, ",\"file\":%d,\"par\":%d,\"dis\":%d,\"nprops\":%d,\"nkids\":%d,\"compat\":",
            n->file_idx, n->parent ? n->parent->id : -1,
            disabled, n->prop_count, n->nkids);
    json_str(f, compat);
    fputs(",\"refs\":[", f);
    emit_refs_json(f, ctx, n);
    fputs("]}", f);

    for (int i = 0; i < n->nkids; i++)
        emit_node_json(f, ctx, n->kids[i], first);
}

/* NEW: Property value emitter that detects and wraps labels */
static void emit_prop_value(FILE *f, const char *val)
{
    if (!val) return;
    const char *p = val;
    while (*p) {
        if (*p == '&') {
            fputs("&amp;", f);
            p++;
            char lbl[MAX_NAME];
            int i = 0;
            /* Read identifier characters: alnum or underscore */
            while (*p && (isalnum((unsigned char)*p) || *p == '_')) {
                if (i < MAX_NAME - 1) lbl[i++] = *p;
                p++;
            }
            lbl[i] = '\0';
            fprintf(f, "<span class=\"ref\" onclick=\"jumpTo('%s')\">%s</span>", lbl, lbl);
        } else {
            /* Standard escaping for other characters */
            if (*p == '<') fputs("&lt;", f);
            else if (*p == '>') fputs("&gt;", f);
            else if (*p == '&') fputs("&amp;", f);
            else if (*p == '"') fputs("&quot;", f);
            else fputc(*p, f);
            p++;
        }
    }
}

/* ═══════════════════════════════════════════════════════════
 *  Devicetree documentation driver-description lookup
 * ═════════════════════════════════════════════════════════*/

/*
 * Search a single .txt binding file for 'compat'.
 *
 * Only looks inside the "Required properties:" section to avoid false
 * positives from Example: blocks that also contain compatible strings.
 * A section ends when a new section heading is detected — a line that
 * starts at column 0, is non-empty, not a list item, and ends with ':'.
 *
 * On match, copies the first real prose line of the file (the driver
 * title) into desc_out.  Returns 1 on success, 0 otherwise.
 */
static int search_txt_for_compat(const char *path, const char *compat,
                                  char *desc_out, int maxlen)
{
    char *content = read_file(path);
    if (!content) return 0;

    size_t clen = strlen(compat);

    /* ── Locate the "Required properties:" section ── */
    const char *req_start = NULL;
    const char *p = content;
    while (*p) {
        if ((p == content || *(p - 1) == '\n') &&
            strncmp(p, "Required properties:", 20) == 0) {
            while (*p && *p != '\n') p++;  /* skip rest of heading line */
            if (*p == '\n') p++;
            req_start = p;
            break;
        }
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }

    if (!req_start) { free(content); return 0; }

    /* ── Find the end of the section ──
     * A new section starts at a line that:
     *   - begins at column 0 (no leading whitespace)
     *   - is not empty and not a list item ('-')
     *   - ends with ':'
     */
    const char *req_end = content + strlen(content); /* default: EOF */
    p = req_start;
    while (*p) {
        const char *line = p;
        /* Advance to end of line to find last non-space char */
        const char *eol = p;
        while (*eol && *eol != '\n' && *eol != '\r') eol++;
        /* Non-blank, column-0, non-list line ending with ':' → new section */
        if (*line != ' ' && *line != '\t' &&
            *line != '\n' && *line != '\r' && *line != '-') {
            const char *last = eol - 1;
            while (last > line && isspace((unsigned char)*last)) last--;
            if (last >= line && *last == ':') {
                req_end = line;
                break;
            }
        }
        p = eol;
        if (*p == '\n') p++;
    }

    /* ── Search for compat string within [req_start, req_end) ── */
    int found = 0;
    for (const char *s = req_start; s + clen <= req_end; s++) {
        if (strncmp(s, compat, clen) == 0) {
            char before = (s > req_start) ? *(s - 1) : ' ';
            char after  = *(s + clen);
            int ok_b = (before == '"' || before == ' ' || before == '\t' ||
                        before == '\'' || before == ':');
            int ok_a = (after  == '"' || after  == '\'' || after  == '\n' ||
                        after  == '\r' || after  == ' '  || after  == '\0' ||
                        after  == ',');
            if (ok_b && ok_a) { found = 1; break; }
        }
    }

    if (!found) { free(content); return 0; }

    /* ── Return first real prose line of the file as the description ── */
    int result = 0;
    p = content;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (!*p) break;
        const char *end = p;
        while (*end && *end != '\n' && *end != '\r') end++;
        int len = (int)(end - p);
        /* Skip pure separator lines (all dashes / equals) */
        int sep = 1;
        for (int i = 0; i < len; i++)
            if (p[i] != '-' && p[i] != '=' && p[i] != ' ') { sep = 0; break; }
        if (!sep && len > 0) {
            if (len >= maxlen) len = maxlen - 1;
            strncpy(desc_out, p, len);
            desc_out[len] = '\0';
            while (len > 0 && isspace((unsigned char)desc_out[len - 1]))
                desc_out[--len] = '\0';
            if (len > 0) { result = 1; break; }
        }
        p = end;
    }

    free(content);
    return result;
}

/*
 * Extract the top-level `title:` value from a YAML binding file without
 * doing any compatible-string verification.  Used when the file has already
 * been identified by filename, so no further content check is needed.
 * Returns 1 on success, 0 otherwise.
 */
static int extract_yaml_title(const char *path, char *desc_out, int maxlen)
{
    char *content = read_file(path);
    if (!content) return 0;

    const char *title_pos = NULL;
    const char *scan = content;
    while (*scan) {
        if (strncmp(scan, "title:", 6) == 0 &&
            (scan == content || *(scan - 1) == '\n')) {
            title_pos = scan + 6;
            break;
        }
        scan++;
    }

    if (!title_pos) { free(content); return 0; }

    while (*title_pos == ' ' || *title_pos == '\t') title_pos++;
    const char *end = title_pos;
    while (*end && *end != '\n' && *end != '\r') end++;

    int len = (int)(end - title_pos);
    if (len <= 0) { free(content); return 0; }
    if (len >= maxlen) len = maxlen - 1;

    strncpy(desc_out, title_pos, len);
    desc_out[len] = '\0';
    while (len > 0 && isspace((unsigned char)desc_out[len - 1]))
        desc_out[--len] = '\0';

    free(content);
    return len > 0;
}

/*
 * Search a single .yaml / .yml binding file for 'compat'.
 *
 * Only looks inside `compatible:` YAML key blocks (at any nesting depth),
 * NOT in description text, examples, or other sections.  This avoids false
 * positives where a word like "cache" appears only in prose description.
 *
 * A `compatible:` block is defined as: all lines that follow a line whose
 * first non-space token is "compatible:" and that have strictly greater
 * indentation than that line.  Blank lines inside the block are skipped.
 *
 * On a confirmed match, copies the value of the top-level `title:` key
 * (column 0) into desc_out.  Returns 1 on success, 0 otherwise.
 */
static int search_yaml_for_compat(const char *path, const char *compat,
                                   char *desc_out, int maxlen)
{
    char *content = read_file(path);
    if (!content) return 0;

    size_t clen = strlen(compat);
    int compat_found = 0;

    /* Walk every line, looking for `compatible:` keys */
    const char *p = content;
    while (*p && !compat_found) {

        const char *line = p;

        /* Measure indentation of this line */
        int indent = 0;
        while (*p == ' ' || *p == '\t') { indent++; p++; }

        /* Skip blank / comment lines */
        if (*p == '\n' || *p == '\r' || *p == '\0') {
            while (*p && *p != '\n') p++;
            if (*p == '\n') p++;
            continue;
        }

        /* Is this line `compatible:` (possibly with trailing whitespace)? */
        if (strncmp(p, "compatible:", 11) == 0) {
            const char *after = p + 11;
            while (*after == ' ' || *after == '\t') after++;
            int is_key = (*after == '\n' || *after == '\r' || *after == '\0');
            /* Also handle inline scalar: compatible: some-string */
            /* We want ANY line whose first token is "compatible:" */
            (void)is_key; /* we accept both forms */

            /* Skip to end of this line */
            while (*p && *p != '\n') p++;
            if (*p == '\n') p++;

            /* Collect the indented block that follows */
            while (*p && !compat_found) {
                const char *subline = p;

                /* Measure sub-line indentation */
                int sub_indent = 0;
                while (*p == ' ' || *p == '\t') { sub_indent++; p++; }

                if (*p == '\n' || *p == '\r') {
                    /* Blank line — doesn't terminate block, skip */
                    while (*p && *p != '\n') p++;
                    if (*p == '\n') p++;
                    continue;
                }
                if (*p == '\0') break;

                /* A non-blank line at indent <= compatible's indent ends block */
                if (sub_indent <= indent) {
                    p = subline; /* rewind so outer loop re-processes this line */
                    break;
                }

                /* This line is part of the compatible block — search it */
                const char *eol = p;
                while (*eol && *eol != '\n' && *eol != '\r') eol++;

                /* Scan the line for our compat string */
                for (const char *s = p; s + clen <= eol; s++) {
                    if (strncmp(s, compat, clen) == 0) {
                        /* Verify boundary: char before must not be ident char */
                        char before = (s > p) ? *(s - 1) : ' ';
                        char after2 = *(s + clen);
                        int ok_before = (before == ' ' || before == '\t' ||
                                         before == '"' || before == '\'' ||
                                         before == ':');
                        int ok_after  = (after2 == '\0' || after2 == '\n' ||
                                         after2 == '\r' || after2 == ' '  ||
                                         after2 == '"' || after2 == '\'' ||
                                         after2 == ',');
                        if (ok_before && ok_after) {
                            compat_found = 1;
                            break;
                        }
                    }
                }

                p = eol;
                if (*p == '\n') p++;
            }
        } else {
            /* Not a compatible: key line — skip to end of line */
            while (*p && *p != '\n') p++;
            if (*p == '\n') p++;
        }

        (void)line;
    }

    if (!compat_found) { free(content); return 0; }

    /* Find top-level `title:` (at column 0) */
    const char *title_pos = NULL;
    const char *scan = content;
    while (*scan) {
        if (strncmp(scan, "title:", 6) == 0 &&
            (scan == content || *(scan - 1) == '\n')) {
            title_pos = scan + 6;
            break;
        }
        scan++;
    }

    if (!title_pos) { free(content); return 0; }

    while (*title_pos == ' ' || *title_pos == '\t') title_pos++;
    const char *end = title_pos;
    while (*end && *end != '\n' && *end != '\r') end++;

    int len = (int)(end - title_pos);
    if (len <= 0) { free(content); return 0; }
    if (len >= maxlen) len = maxlen - 1;

    strncpy(desc_out, title_pos, len);
    desc_out[len] = '\0';
    while (len > 0 && isspace((unsigned char)desc_out[len - 1]))
        desc_out[--len] = '\0';

    free(content);
    return len > 0;
}

/*
 * Single-pass directory walk restricted to one file class:
 *   pass == 0 → only .yaml / .yml
 *   pass == 1 → only .txt
 */
static int find_desc_in_dir_pass(const char *dir, const char *compat,
                                  char *desc_out, int maxlen, int pass,
                                  char *path_out)
{
    DIR *d = opendir(dir);
    if (!d) return 0;

    struct dirent *ent;
    char path[MAX_PATH];
    int found = 0;

    while (!found && (ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;

        snprintf(path, MAX_PATH, "%s/%s", dir, ent->d_name);

        struct stat st;
        if (stat(path, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            found = find_desc_in_dir_pass(path, compat, desc_out, maxlen, pass,
                                          path_out);
        } else {
            size_t nlen = strlen(ent->d_name);
            if (pass == 0) {
                /* YAML / YML only */
                if ((nlen > 5 && strcmp(ent->d_name + nlen - 5, ".yaml") == 0) ||
                    (nlen > 4 && strcmp(ent->d_name + nlen - 4, ".yml")  == 0))
                    found = search_yaml_for_compat(path, compat, desc_out, maxlen);
            } else {
                /* TXT only */
                if (nlen > 4 && strcmp(ent->d_name + nlen - 4, ".txt") == 0)
                    found = search_txt_for_compat(path, compat, desc_out, maxlen);
            }
            if (found) strncpy(path_out, path, MAX_PATH - 1);
        }
    }

    closedir(d);
    return found;
}

/*
 * Recursively walk 'dir' looking for a .yaml or .yml file whose basename
 * (without extension) exactly equals 'compat' (e.g. "regulator-fixed.yaml"
 * for compatible "regulator-fixed").  On a hit, extract the title: directly.
 * Returns 1 if found, 0 otherwise.
 */
static int find_yaml_by_filename(const char *dir, const char *compat,
                                  char *desc_out, int maxlen, char *path_out)
{
    DIR *d = opendir(dir);
    if (!d) return 0;

    struct dirent *ent;
    char path[MAX_PATH];
    int found = 0;

    while (!found && (ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;

        snprintf(path, MAX_PATH, "%s/%s", dir, ent->d_name);

        struct stat st;
        if (stat(path, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            found = find_yaml_by_filename(path, compat, desc_out, maxlen, path_out);
        } else {
            size_t nlen  = strlen(ent->d_name);
            size_t clen  = strlen(compat);
            int    is_yaml = 0;
            size_t ext_len = 0;

            if (nlen > 5 && strcmp(ent->d_name + nlen - 5, ".yaml") == 0)
                { is_yaml = 1; ext_len = 5; }
            else if (nlen > 4 && strcmp(ent->d_name + nlen - 4, ".yml") == 0)
                { is_yaml = 1; ext_len = 4; }

            if (is_yaml && (nlen - ext_len) == clen &&
                strncmp(ent->d_name, compat, clen) == 0) {
                /* Exact filename match — trust the file and grab title directly */
                found = extract_yaml_title(path, desc_out, maxlen);
                if (found) strncpy(path_out, path, MAX_PATH - 1);
            }
        }
    }

    closedir(d);
    return found;
}

/*
 * Recursively walk 'dir' looking for a binding file that documents 'compat'.
 * Search order:
 *   0. Hardcoded filename aliases (e.g. "regulator-fixed" → "fixed-regulator.yaml")
 *   1. YAML/YML file whose basename equals the compat string (fastest, most precise)
 *   2. Any YAML/YML file whose compatible: block contains the compat string
 *   3. Any .txt file whose Required properties: section contains the compat string
 */
static int find_desc_in_dir(const char *dir, const char *compat,
                             char *desc_out, int maxlen, char *path_out)
{
    /* Pass -1: hardcoded compat-string → filename aliases */
    static const struct { const char *compat; const char *fname; } ALIASES[] = {
        { "regulator-fixed", "fixed-regulator" },
    };
    for (int i = 0; i < (int)(sizeof(ALIASES)/sizeof(ALIASES[0])); i++) {
        if (strcmp(compat, ALIASES[i].compat) == 0) {
            if (find_yaml_by_filename(dir, ALIASES[i].fname, desc_out, maxlen,
                                      path_out))
                return 1;
            break; /* alias tried and missed — fall through to normal passes */
        }
    }

    /* Pass 0: exact filename match in YAML */
    if (find_yaml_by_filename(dir, compat, desc_out, maxlen, path_out))
        return 1;
    /* Pass 1: content search in YAML / YML */
    if (find_desc_in_dir_pass(dir, compat, desc_out, maxlen, 0, path_out))
        return 1;
    /* Pass 2: content search in TXT (Required properties: section only) */
    return find_desc_in_dir_pass(dir, compat, desc_out, maxlen, 1, path_out);
}

/*
 * Given a DTS compatible property value (e.g. "arm,psci-1.0", "arm,psci"),
 * try each quoted compat string in order against the documentation folder.
 * Writes the first match into desc_out and matched file path into path_out.
 * Returns 1 if found.
 */
static int lookup_driver_desc(const char *compat_val, const char *doc_dir,
                               char *desc_out, int maxlen, char *path_out)
{
    if (!compat_val || !doc_dir || !doc_dir[0]) return 0;

    const char *p = compat_val;
    while (*p) {
        /* skip separators */
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ',')
            p++;
        if (!*p) break;

        char compat[MAX_NAME];
        if (*p == '"') {
            p++; /* opening quote */
            int i = 0;
            while (*p && *p != '"' && i < MAX_NAME - 1) compat[i++] = *p++;
            compat[i] = '\0';
            if (*p == '"') p++; /* closing quote */
        } else {
            /* bare word — rare but handle gracefully */
            int i = 0;
            while (*p && *p != ',' && *p != ' ' && *p != '\n' && i < MAX_NAME - 1)
                compat[i++] = *p++;
            compat[i] = '\0';
        }

        if (compat[0] && find_desc_in_dir(doc_dir, compat, desc_out, maxlen,
                                           path_out))
            return 1;
    }
    return 0;
}

/* 20 visually distinct colours for file attribution[cite: 1] */
static const char *COLORS[] = {
    "#4ade80","#60a5fa","#f97316","#e879f9","#facc15",
    "#34d399","#f87171","#a78bfa","#38bdf8","#fb923c",
    "#818cf8","#2dd4bf","#f472b6","#a3e635","#fbbf24",
    "#c084fc","#67e8f9","#86efac","#fca5a5","#fdba74",
};
#define NCOLORS ((int)(sizeof(COLORS) / sizeof(COLORS[0])))

/* ─── Recursive node emitter ───────────────────────────────[cite: 1] */

static void emit_node(FILE *f, Ctx *ctx, Node *n)
{
    const char *col   = COLORS[n->file_idx % NCOLORS];
    const char *fname = ctx->fnames[n->file_idx % ctx->nfiles];

    /* ── Resolve driver description up-front so we can emit it inline ── */
    char drv_desc[512] = "";
    char drv_path[MAX_PATH] = "";
    if (ctx->doc_dir[0]) {
        for (Prop *pr = n->props; pr; pr = pr->next) {
            if (strcmp(pr->name, "compatible") == 0 && pr->value) {
                lookup_driver_desc(pr->value, ctx->doc_dir,
                                   drv_desc, sizeof(drv_desc), drv_path);
                break;
            }
        }
    }

    /* ── Detect disabled status (use last 'status' prop — overlays may re-enable) ── */
    int node_disabled = 0;
    {
        const char *last_status = NULL;
        for (Prop *pr = n->props; pr; pr = pr->next)
            if (strcmp(pr->name, "status") == 0 && pr->value)
                last_status = pr->value;
        if (last_status) {
            /* Value is stored as DTS text, e.g. "disabled" or "okay" with quotes */
            const char *v = last_status;
            while (*v == '"' || *v == ' ' || *v == '\t') v++;
            if (strncmp(v, "disabled", 8) == 0) {
                char a = v[8];
                node_disabled = (a == '"' || a == '\0' || a == ' ' || a == '\t');
            }
        }
    }

    /* Outer wrapper carries search/filter data attributes[cite: 1] */
    fprintf(f, "<div class=\"node%s%s\" data-file=\"%d\" data-name=\"",
            n->nkids ? " hc" : "",
            node_disabled ? " dis" : "",
            n->file_idx);
    html_esc(f, n->name);
    fprintf(f, "\" data-label=\"");
    html_esc(f, n->label);
    fprintf(f, "\" data-addr=\"%s\">\n", n->unit_addr);

    /* ── Always-visible header ──[cite: 1] */
    fprintf(f, "<div class=\"nh\" onclick=\"tog(this)\">");
    fprintf(f, "<span class=\"arr\">%s</span>", n->nkids ? "&#9654;" : "&middot;");
    fprintf(f, "<span class=\"dot\" style=\"background:%s\" title=\"", col);
    html_esc(f, fname);
    fprintf(f, "\"></span>");

    if (n->label[0]) {
        fprintf(f, "<span class=\"lbl\">");
        html_esc(f, n->label);
        fprintf(f, ":</span> ");
    }

    fprintf(f, "<span class=\"nm\">");
    html_esc(f, n->name);
    if (n->unit_addr[0])
        fprintf(f, "<span class=\"ua\">@%s</span>", n->unit_addr);
    fprintf(f, "</span>");

    if (node_disabled)
        fputs(" <span class=\"dis-badge\">disabled</span>", f);

    /* ── Inline driver description badge ── */
    if (drv_desc[0]) {
        fputs("<span class=\"drv-badge\">&#x1F4D6; ", f);
        html_esc(f, drv_desc);
        if (drv_path[0]) {
            fprintf(f,
                " <span class=\"drv-link\" "
                "onclick=\"event.stopPropagation();showDocPopup(%d)\" "
                "title=\"View binding documentation\">&#x1F517;</span>",
                n->id);
        }
        fputs("</span>", f);
    }

    if (n->prop_count)
        fprintf(f, " <span class=\"pc\">%dp</span>", n->prop_count);
    if (n->nkids)
        fprintf(f, " <span class=\"kc\">%d&#8595;</span>", n->nkids);

    /* Right-aligned file badge[cite: 1] */
    fprintf(f, " <span class=\"fb\" style=\"--c:%s\">", col);
    html_esc(f, fname);
    fprintf(f, "</span>");

    fprintf(f, "</div>\n"); /* nh[cite: 1] */

    /* ── Collapsible body (hidden by default) ──[cite: 1] */
    fprintf(f, "<div class=\"nb\">\n");

    if (n->prop_count) {
        fprintf(f, "<div class=\"props\">\n");
        for (Prop *pr = n->props; pr; pr = pr->next) {
            fprintf(f, "<div class=\"prop\"><span class=\"pk\">");
            html_esc(f, pr->name);
            fprintf(f, "</span>");
            if (pr->value) {
                fprintf(f, " <span class=\"eq\">=</span> <span class=\"pv\">");
                emit_prop_value(f, pr->value); /* CHANGED: use our jump-aware emitter */
                fprintf(f, "</span>");
            }
            fprintf(f, ";</div>\n");
        }
        fprintf(f, "</div>\n"); /* props[cite: 1] */
    }

    if (n->nkids) {
        fprintf(f, "<div class=\"kids\">\n");
        for (int i = 0; i < n->nkids; i++)
            emit_node(f, ctx, n->kids[i]);
        fprintf(f, "</div>\n"); /* kids[cite: 1] */
    }

    fprintf(f, "</div>\n"); /* nb[cite: 1] */

    /* ── Embedded doc file content for popup (hidden) ── */
    if (drv_path[0]) {
        char *doc_content = read_file(drv_path);
        if (doc_content) {
            /* Extract just the filename for the popup title bar */
            const char *doc_fname = strrchr(drv_path, '/');
            doc_fname = doc_fname ? doc_fname + 1 : drv_path;

            fprintf(f, "<span id=\"doctitle-%d\" style=\"display:none\">", n->id);
            html_esc(f, doc_fname);
            fputs("</span>\n", f);

            fprintf(f, "<pre id=\"docdata-%d\" style=\"display:none\">", n->id);
            /* Cap at 256 KB to keep HTML size reasonable */
            size_t dlen = strlen(doc_content);
            if (dlen > 262144) doc_content[262144] = '\0';
            html_esc(f, doc_content);
            fputs("</pre>\n", f);

            free(doc_content);
        }
    }

    fprintf(f, "</div>\n"); /* node[cite: 1] */
}

static const char DIAGRAM_CSS[] =
"\n"
"#main{flex:1;display:flex;flex-direction:column;overflow:hidden;}\n"
"#view-bar{display:flex;align-items:center;gap:0;padding:6px 14px;\n"
"  border-bottom:1px solid var(--brd);flex-shrink:0;background:var(--bg2);}\n"
"#view-bar-label{font-size:10px;color:var(--fg2);margin-right:8px;text-transform:uppercase;letter-spacing:.06em;}\n"
".vbtn{padding:4px 14px;background:var(--bg3);border:1px solid var(--brd);\n"
"  color:var(--fg2);cursor:pointer;font-size:10.5px;font-family:var(--font);transition:background .1s;}\n"
".vbtn:first-of-type{border-radius:5px 0 0 5px;}\n"
".vbtn:last-of-type{border-radius:0 5px 5px 0;}\n"
".vbtn+.vbtn{border-left:none;}\n"
".vbtn.vact{background:rgba(88,166,255,.12);border-color:var(--acc);color:var(--acc);}\n"
".vbtn:hover:not(.vact){background:var(--bg4);color:var(--fg);}\n"
"#main-content{flex:1;overflow:auto;position:relative;}\n"
"#tree{padding:14px 18px;}\n"
"#diagram-view{display:none;position:absolute;top:0;left:0;width:100%;height:100%;}\n"
"#diagram-svg{width:100%;height:100%;background:var(--bg);display:block;}\n"
"#dg-ctrl{position:absolute;top:10px;left:10px;display:flex;align-items:center;\n"
"  gap:5px;background:rgba(13,17,23,.9);padding:6px 10px;\n"
"  border:1px solid var(--brd);border-radius:8px;z-index:10;user-select:none;}\n"
"#dg-ctrl span{font-size:10px;color:var(--fg2);margin-right:2px;}\n"
".ddb{padding:3px 8px;background:var(--bg3);border:1px solid var(--brd);\n"
"  color:var(--fg2);border-radius:4px;cursor:pointer;font-size:10px;\n"
"  font-family:var(--font);transition:background .1s;}\n"
".ddb:hover{background:var(--bg4);color:var(--fg);}\n"
".ddb.dda{background:rgba(88,166,255,.12);border-color:var(--acc);color:var(--acc);}\n"
"#dg-fit{padding:3px 10px;background:var(--bg3);border:1px solid var(--brd);\n"
"  color:var(--fg2);border-radius:4px;cursor:pointer;font-size:10px;\n"
"  font-family:var(--font);margin-left:4px;transition:background .1s;}\n"
"#dg-fit:hover{background:var(--bg4);color:var(--fg);}\n"
"#dg-hint{position:absolute;bottom:10px;left:50%;transform:translateX(-50%);\n"
"  font-size:9.5px;color:var(--fg2);opacity:.5;pointer-events:none;}\n"
"\n"
;

static const char DIAGRAM_JS[] =
"\n"
"var DTDiagram=(function(){\n"
"'use strict';\n"
"var NW=210,NH=66,HGAP=48,VGAP=104;\n"
"var COLORS=['#4ade80','#60a5fa','#f97316','#e879f9','#facc15',\n"
"            '#34d399','#f87171','#a78bfa','#38bdf8','#fb923c',\n"
"            '#818cf8','#2dd4bf','#f472b6','#a3e635','#fbbf24',\n"
"            '#c084fc','#67e8f9','#86efac','#fca5a5','#fdba74'];\n"
"var nodeMap={},roots=[],allNodes=[];\n"
"var svgEl,gEl,pan={x:60,y:40,s:1};\n"
"var dragging=false,ds={};\n"
"var maxD=3,ready=false;\n"
"\n"
"/* ── data setup ── */\n"
"function setup(){\n"
"  DT_NODES.forEach(function(n){n.children=[];n.col=false;nodeMap[n.id]=n;});\n"
"  DT_NODES.forEach(function(n){\n"
"    if(n.par>=0&&nodeMap[n.par])nodeMap[n.par].children.push(n);\n"
"    else roots.push(n);\n"
"  });\n"
"  roots.forEach(function(r){initCol(r,0);});\n"
"}\n"
"function initCol(n,d){\n"
"  n.col=(n.children.length>0&&d>=maxD-1);\n"
"  n.children.forEach(function(c){initCol(c,d+1);});\n"
"}\n"
"\n"
"/* ── layout: tidy tree ── */\n"
"function layout(){\n"
"  allNodes=[];\n"
"  var slot={v:0};\n"
"  roots.forEach(function(r){visit(r,0,slot);});\n"
"}\n"
"function visit(n,d,sl){\n"
"  n._d=d; allNodes.push(n);\n"
"  var vis=n.col?[]:n.children;\n"
"  if(!vis.length){n._x=sl.v*(NW+HGAP);sl.v++;}\n"
"  else{\n"
"    vis.forEach(function(c){visit(c,d+1,sl);});\n"
"    n._x=(vis[0]._x+vis[vis.length-1]._x)/2;\n"
"  }\n"
"  n._y=d*(NH+VGAP);\n"
"}\n"
"\n"
"/* ── svg helpers ── */\n"
"var NS='http://www.w3.org/2000/svg';\n"
"function S(t){return document.createElementNS(NS,t);}\n"
"function A(e,k,v){e.setAttribute(k,v);return e;}\n"
"\n"
"/* ── render ── */\n"
"function render(){\n"
"  if(!svgEl)return;\n"
"  buildDefs();\n"
"  gEl.innerHTML='';\n"
"  var eg=S('g'),rg=S('g');\n"
"  gEl.appendChild(eg); gEl.appendChild(rg);\n"
"  allNodes.forEach(function(n){\n"
"    var par=nodeMap[n.par];\n"
"    if(par&&allNodes.indexOf(par)>=0&&!par.col)\n"
"      treeEdge(eg,par._x+NW/2,par._y+NH,n._x+NW/2,n._y);\n"
"    (n.refs||[]).forEach(function(rid){\n"
"      var t=nodeMap[rid];\n"
"      if(!t||t===n||t===par)return;\n"
"      if(allNodes.indexOf(t)<0)return;\n"
"      refEdge(rg,n,t);\n"
"    });\n"
"  });\n"
"  allNodes.forEach(function(n){nodeBox(gEl,n);});\n"
"  tx();\n"
"}\n"
"\n"
"function buildDefs(){\n"
"  var d=svgEl.querySelector('defs');\n"
"  if(!d){d=svgEl.insertBefore(S('defs'),svgEl.firstChild);}\n"
"  var h='<marker id=\"mag\" viewBox=\"0 0 8 6\" refX=\"7\" refY=\"3\" markerWidth=\"6\" markerHeight=\"5\" orient=\"auto\"><polygon points=\"0,0 8,3 0,6\" fill=\"#3d444d\"/></marker>';\n"
"  COLORS.forEach(function(c,i){\n"
"    h+='<marker id=\"mac'+i+'\" viewBox=\"0 0 8 6\" refX=\"7\" refY=\"3\" markerWidth=\"6\" markerHeight=\"5\" orient=\"auto\"><polygon points=\"0,0 8,3 0,6\" fill=\"'+c+'\" opacity=\"0.8\"/></marker>';\n"
"  });\n"
"  d.innerHTML=h;\n"
"}\n"
"\n"
"function treeEdge(p,x1,y1,x2,y2){\n"
"  var e=S('path'),my=(y1+y2)/2;\n"
"  A(e,'d','M'+x1+','+y1+' C'+x1+','+my+' '+x2+','+my+' '+x2+','+y2);\n"
"  A(e,'fill','none');A(e,'stroke','#3d444d');A(e,'stroke-width','1.5');\n"
"  A(e,'marker-end','url(#mag)');\n"
"  p.appendChild(e);\n"
"}\n"
"\n"
"function refEdge(p,fn,tn){\n"
"  var ci=fn.file%COLORS.length,col=COLORS[ci];\n"
"  var fromR=tn._x>=fn._x;\n"
"  var x1=fromR?fn._x+NW:fn._x, y1=fn._y+NH*0.5;\n"
"  var x2=fromR?tn._x:tn._x+NW, y2=tn._y+NH*0.5;\n"
"  var bend=Math.max(70,Math.abs(fn._y-tn._y)*0.55+Math.abs(fn._x-tn._x)*0.1);\n"
"  var cx=(x1+x2)/2, cy=Math.min(fn._y,tn._y)-bend;\n"
"  var e=S('path');\n"
"  A(e,'d','M'+x1+','+y1+' Q'+cx+','+cy+' '+x2+','+y2);\n"
"  A(e,'fill','none');A(e,'stroke',col);A(e,'stroke-width','1.3');\n"
"  A(e,'stroke-dasharray','5,3');A(e,'opacity','0.6');\n"
"  A(e,'marker-end','url(#mac'+ci+')');\n"
"  p.appendChild(e);\n"
"}\n"
"\n"
"function nodeBox(p,n){\n"
"  var col=COLORS[n.file%COLORS.length];\n"
"  var g=S('g');\n"
"  A(g,'transform','translate('+n._x+','+n._y+')');\n"
"  g.style.cursor=n.children.length?'pointer':'default';\n"
"\n"
"  /* shadow rect */\n"
"  var sh=S('rect');\n"
"  A(sh,'x','-2');A(sh,'y','-2');A(sh,'width',NW+4);A(sh,'height',NH+4);\n"
"  A(sh,'rx','9');A(sh,'fill',col);A(sh,'opacity',n.dis?'0.03':'0.08');\n"
"  g.appendChild(sh);\n"
"\n"
"  /* body */\n"
"  var bg=S('rect');\n"
"  A(bg,'width',NW);A(bg,'height',NH);A(bg,'rx','7');\n"
"  A(bg,'fill',n.dis?'#0d1117':'#161b22');\n"
"  A(bg,'stroke',col);A(bg,'stroke-width',n.dis?'0.5':'1.5');\n"
"  A(bg,'opacity',n.dis?'0.38':'1');\n"
"  g.appendChild(bg);\n"
"\n"
"  /* top stripe */\n"
"  var st=S('rect');\n"
"  A(st,'width',NW);A(st,'height','4');A(st,'rx','7');\n"
"  A(st,'fill',col);A(st,'opacity',n.dis?'0.18':'1');\n"
"  g.appendChild(st);\n"
"\n"
"  /* node name */\n"
"  var nt=S('text');\n"
"  A(nt,'x','9');A(nt,'y','21');\n"
"  A(nt,'fill',n.dis?'#3d4349':'#e6edf3');\n"
"  A(nt,'font-size','11.5');A(nt,'font-weight','600');A(nt,'font-family','monospace');\n"
"  var dn=n.name+(n.addr?'@'+n.addr:'');\n"
"  if(dn.length>23)dn=dn.slice(0,22)+'...';\n"
"  nt.textContent=dn;\n"
"  g.appendChild(nt);\n"
"\n"
"  /* label top-right */\n"
"  if(n.lbl){\n"
"    var lt=S('text');\n"
"    A(lt,'x',NW-8);A(lt,'y','16');\n"
"    A(lt,'fill',n.dis?'#4a3020':'#f97316');\n"
"    A(lt,'font-size','9');A(lt,'font-family','monospace');\n"
"    A(lt,'text-anchor','end');\n"
"    lt.textContent=n.lbl+':';\n"
"    g.appendChild(lt);\n"
"  }\n"
"\n"
"  /* compatible */\n"
"  if(n.compat){\n"
"    var cmp=S('text');\n"
"    A(cmp,'x','9');A(cmp,'y','36');\n"
"    A(cmp,'fill',n.dis?'#1e3020':'#86efac');\n"
"    A(cmp,'font-size','9.5');A(cmp,'font-family','monospace');\n"
"    A(cmp,'font-style','italic');\n"
"    var cd=n.compat.replace(/\"/g,'').split(',')[0].trim();\n"
"    if(cd.length>27)cd=cd.slice(0,26)+'...';\n"
"    cmp.textContent=cd;\n"
"    g.appendChild(cmp);\n"
"  }\n"
"\n"
"  /* badges */\n"
"  var bx=8;\n"
"  if(n.nprops){bx=bdg(g,bx,NH-9,n.nprops+'p','#21262d',n.dis?'#333':'#8b949e',bx);}\n"
"  if(n.nkids){bdg(g,bx,NH-9,n.nkids+(n.col?'\\u25B6':'\\u2193'),'#21262d',n.dis?'#333':'#60a5fa',bx);}\n"
"  if(n.dis){bdg(g,NW-62,NH-9,'disabled','#0d1117','#444d56',NW-62);}\n"
"  if(n.refs&&n.refs.length){bdg(g,NW-62-(n.dis?52:0)-4,NH-9,'\\u2192'+n.refs.length,'#21262d',n.dis?'#333':'#c084fc',0);}\n"
"\n"
"  /* expand/collapse click */\n"
"  if(n.children.length){\n"
"    g.addEventListener('click',function(e){\n"
"      e.stopPropagation();\n"
"      n.col=!n.col;\n"
"      layout();render();\n"
"    });\n"
"  }\n"
"  p.appendChild(g);\n"
"}\n"
"\n"
"function bdg(p,x,y,txt,bg,fg){\n"
"  var w=txt.length*5.8+9;\n"
"  var r=S('rect');A(r,'x',x);A(r,'y',y-10);A(r,'width',w);A(r,'height',12);A(r,'rx','3');A(r,'fill',bg);\n"
"  var t=S('text');A(t,'x',x+4);A(t,'y',y);A(t,'fill',fg);A(t,'font-size','8.5');A(t,'font-family','monospace');\n"
"  t.textContent=txt;p.appendChild(r);p.appendChild(t);\n"
"  return x+w+5;\n"
"}\n"
"\n"
"/* ── transform ── */\n"
"function tx(){A(gEl,'transform','translate('+pan.x+','+pan.y+') scale('+pan.s+')');}\n"
"\n"
"/* ── pan & zoom ── */\n"
"function panzoom(){\n"
"  svgEl.addEventListener('mousedown',function(e){\n"
"    if(e.button!==0)return;\n"
"    dragging=true;ds={mx:e.clientX,my:e.clientY,px:pan.x,py:pan.y};\n"
"    svgEl.style.cursor='grabbing';e.preventDefault();\n"
"  });\n"
"  window.addEventListener('mouseup',function(){dragging=false;if(svgEl)svgEl.style.cursor='grab';});\n"
"  window.addEventListener('mousemove',function(e){\n"
"    if(!dragging)return;\n"
"    pan.x=ds.px+(e.clientX-ds.mx);pan.y=ds.py+(e.clientY-ds.my);tx();\n"
"  });\n"
"  svgEl.addEventListener('wheel',function(e){\n"
"    e.preventDefault();\n"
"    var rc=svgEl.getBoundingClientRect();\n"
"    var mx=e.clientX-rc.left,my=e.clientY-rc.top;\n"
"    var f=e.deltaY<0?1.12:0.9;\n"
"    var ns=Math.min(4,Math.max(0.04,pan.s*f));\n"
"    pan.x=mx-(mx-pan.x)*(ns/pan.s);\n"
"    pan.y=my-(my-pan.y)*(ns/pan.s);\n"
"    pan.s=ns;tx();\n"
"  },{passive:false});\n"
"}\n"
"\n"
"/* ── fit to screen ── */\n"
"function fit(){\n"
"  if(!allNodes.length)return;\n"
"  var vw=svgEl.clientWidth||900,vh=svgEl.clientHeight||600;\n"
"  var mnx=1e9,mny=1e9,mxx=-1e9,mxy=-1e9;\n"
"  allNodes.forEach(function(n){\n"
"    mnx=Math.min(mnx,n._x);mny=Math.min(mny,n._y);\n"
"    mxx=Math.max(mxx,n._x+NW);mxy=Math.max(mxy,n._y+NH);\n"
"  });\n"
"  var pad=50,sw=mxx-mnx,sh=mxy-mny;\n"
"  if(sw<=0||sh<=0)return;\n"
"  var sc=Math.min((vw-2*pad)/sw,(vh-2*pad)/sh,1.5);\n"
"  pan.s=sc;pan.x=pad-mnx*sc;pan.y=pad-mny*sc;tx();\n"
"}\n"
"\n"
"/* ── depth control ── */\n"
"function setDepth(d){\n"
"  maxD=d;\n"
"  roots.forEach(function(r){initCol(r,0);});\n"
"  layout();render();setTimeout(fit,20);\n"
"}\n"
"\n"
"/* ── public init ── */\n"
"function init(){\n"
"  svgEl=document.getElementById('diagram-svg');\n"
"  gEl=document.getElementById('diagram-g');\n"
"  svgEl.style.cursor='grab';\n"
"  setup();layout();panzoom();render();\n"
"  setTimeout(fit,60);\n"
"  document.querySelectorAll('.ddb').forEach(function(b){\n"
"    b.addEventListener('click',function(){\n"
"      document.querySelectorAll('.ddb').forEach(function(x){x.classList.remove('dda');});\n"
"      b.classList.add('dda');\n"
"      setDepth(parseInt(b.dataset.d)||999);\n"
"    });\n"
"  });\n"
"  var fb=document.getElementById('dg-fit');\n"
"  if(fb)fb.addEventListener('click',fit);\n"
"}\n"
"return{init:init,fit:fit};\n"
"})();\n"
"\n"
;

/* ═══════════════════════════════════════════════════════════
 *  Embedded CSS  (GitHub dark-inspired, monospace)[cite: 1]
 * ═════════════════════════════════════════════════════════*/

static const char CSS[] =
":root{"
  "--bg:#0d1117;--bg2:#161b22;--bg3:#21262d;--bg4:#2d333b;"
  "--fg:#c9d1d9;--fg2:#8b949e;--acc:#58a6ff;--brd:#30363d;"
  "--font:\"Cascadia Code\",\"JetBrains Mono\",\"Fira Code\","
         "ui-monospace,\"Courier New\",monospace;}\n"
"*{box-sizing:border-box;margin:0;padding:0;}\n"
"body{background:var(--bg);color:var(--fg);font-family:var(--font);"
     "font-size:12.5px;display:flex;height:100vh;overflow:hidden;}\n"

/* ── Sidebar ──[cite: 1] */
"#side{width:272px;min-width:200px;background:var(--bg2);"
       "border-right:1px solid var(--brd);display:flex;"
       "flex-direction:column;overflow:hidden;flex-shrink:0;}\n"
"#side-top{padding:10px 14px 8px 14px;border-bottom:1px solid var(--brd);"
           "background:var(--bg2);}\n"
"#side-top strong{color:var(--acc);font-size:14px;display:block;margin-bottom:3px;}\n"
"#logo{display:block;width:100%;height:auto;margin-bottom:5px;}\n"
"#side-top span{font-size:10px;color:var(--fg2);word-break:break-all;}\n"
"#sb{padding:9px 12px;border-bottom:1px solid var(--brd);}\n"
"#sb input{width:100%;background:var(--bg3);border:1px solid var(--brd);"
           "color:var(--fg);padding:6px 10px;border-radius:6px;"
           "font-family:var(--font);font-size:11.5px;outline:none;}\n"
"#sb input:focus{border-color:var(--acc);}\n"
"#sb input::placeholder{color:var(--fg2);}\n"
"#mc{font-size:10px;color:var(--fg2);padding:3px 13px;min-height:18px;}\n"
"#ctrl{padding:7px 11px;display:flex;gap:5px;border-bottom:1px solid var(--brd);}\n"
"#ctrl button{flex:1;padding:5px 2px;background:var(--bg3);border:1px solid var(--brd);"
              "color:var(--fg);border-radius:5px;cursor:pointer;"
              "font-size:10.5px;font-family:var(--font);transition:background .12s;}\n"
"#ctrl button:hover{background:var(--bg4);}\n"
"#stats{padding:9px 14px;border-bottom:1px solid var(--brd);}\n"
".st{display:flex;justify-content:space-between;padding:2px 0;"
     "font-size:10.5px;color:var(--fg2);}\n"
".st b{color:var(--acc);font-weight:600;}\n"
"#leg{padding:9px 11px;overflow-y:auto;flex:1;}\n"
"#leg h3{font-size:10px;text-transform:uppercase;letter-spacing:.07em;"
          "color:var(--fg2);margin-bottom:6px;}\n"
"#attribution{padding:8px 14px;border-top:1px solid var(--brd);"
              "font-size:9.5px;color:#444d56;text-align:center;"
              "white-space:nowrap;flex-shrink:0;}\n"
"#attribution span{color:#555f6a;font-weight:600;}\n"
".fi{display:flex;align-items:center;gap:7px;padding:5px 7px;"
     "border-radius:5px;cursor:pointer;margin-bottom:1px;"
     "transition:background .1s;}\n"
".fi:hover{background:var(--bg3);}\n"
".fi.act{background:var(--bg3);outline:1px solid var(--brd);}\n"
".fi .d{width:9px;height:9px;border-radius:50%;flex-shrink:0;}\n"
".fi .fn{font-size:11px;color:var(--fg);overflow:hidden;"
          "text-overflow:ellipsis;white-space:nowrap;flex:1;}\n"
".fi .fk{font-size:10px;color:var(--fg2);flex-shrink:0;}\n"

/* ── Main panel ──[cite: 1] */
"/* #main layout handled by DIAGRAM_CSS */\n"
"#nr{display:none;padding:40px 20px;color:var(--fg2);"
     "text-align:center;font-size:13px;}\n"

/* ── Tree nodes ──[cite: 1] */
".node{margin:1px 0;}\n"
".nh{display:flex;align-items:center;gap:5px;padding:3px 7px;"
     "border-radius:5px;cursor:pointer;user-select:none;"
     "min-height:26px;transition:background .1s;}\n"
".nh:hover{background:var(--bg2);}\n"
".nh.open{background:var(--bg3);border-radius:5px 5px 0 0;}\n"
".arr{font-size:8px;color:var(--fg2);width:11px;flex-shrink:0;"
      "display:inline-block;transition:transform .12s;text-align:center;}\n"
".arr.r{transform:rotate(90deg);}\n"
".dot{width:7px;height:7px;border-radius:50%;flex-shrink:0;}\n"
".lbl{color:#f97316;font-size:10.5px;font-style:italic;}\n"
".nm{color:var(--fg);font-weight:500;}\n"
".ua{color:var(--fg2);font-weight:400;}\n"
".pc{font-size:9.5px;color:var(--fg2);padding:1px 4px;"
     "border-radius:3px;background:var(--bg3);margin-left:1px;}\n"
".kc{font-size:9.5px;color:#60a5fa;padding:1px 4px;"
     "border-radius:3px;background:var(--bg3);margin-left:1px;}\n"
".fb{font-size:9px;padding:1px 5px;border-radius:3px;"
     "color:var(--c);border:1px solid var(--c);"
     "opacity:.5;margin-left:auto;white-space:nowrap;overflow:hidden;"
     "max-width:120px;text-overflow:ellipsis;}\n"

/* Collapsible body[cite: 1] */
".nb{border-left:1px solid var(--brd);margin-left:18px;display:none;}\n"
".nb.open{display:block;}\n"

/* Properties[cite: 1] */
".props{padding:3px 0 3px 8px;}\n"
".prop{padding:1.5px 6px;font-size:11px;color:var(--fg2);"
       "line-height:1.75;white-space:pre-wrap;word-break:break-all;}\n"
".pk{color:#7dd3fc;}\n"
".eq{color:var(--fg2);}\n"
".pv{color:#86efac;}\n"
".kids{padding-top:1px;padding-left:8px;}\n"

/* Search highlight[cite: 1] */
".hl>.nh{background:rgba(250,204,21,.1)!important;"
          "outline:1px solid rgba(250,204,21,.35);}\n"
".hidden{display:none!important;}\n"

/* Disabled node */
".node.dis>.nh{opacity:0.38;}\n"
".node.dis>.nh .nm{color:var(--fg2);}\n"
".node.dis>.nh .lbl{color:#555;}\n"
".dis-badge{font-size:9px;color:#6b7280;background:var(--bg3);"
            "border:1px solid #374151;border-radius:3px;"
            "padding:1px 5px;margin-left:4px;font-style:normal;}\n"

/* Scrollbar styling[cite: 1] */
"::-webkit-scrollbar{width:6px;height:6px;}\n"
"::-webkit-scrollbar-track{background:var(--bg);}\n"
"::-webkit-scrollbar-thumb{background:var(--bg4);border-radius:3px;}\n"
"::-webkit-scrollbar-thumb:hover{background:var(--fg2);}\n"

/* Jump Highlight & Reference Links */
".ref{color:var(--acc);text-decoration:underline;cursor:pointer;font-weight:bold;}\n"
".nh.jump-hl{background:rgba(88,166,255,0.4) !important; outline:1px solid var(--acc); transition: background 0.5s;}\n"

/* Inline driver description badge (shown in node header line) */
".drv-badge{font-size:9.5px;color:#c084fc;font-style:italic;margin-left:5px;"
           "flex-shrink:1;min-width:0;overflow:hidden;text-overflow:ellipsis;"
           "white-space:nowrap;opacity:0.9;}\n"
".drv-link{color:#c084fc;opacity:0.55;font-style:normal;cursor:pointer;"
           "margin-left:3px;user-select:none;}\n"
".drv-link:hover{opacity:1;}\n"

/* Doc popup modal */
"#doc-modal{display:none;position:fixed;top:0;left:0;width:100%;height:100%;"
            "background:rgba(0,0,0,.78);z-index:999;"
            "align-items:center;justify-content:center;}\n"
"#doc-modal.open{display:flex;}\n"
"#doc-modal-box{width:82%;max-width:960px;height:88vh;"
                "background:var(--bg2);border:1px solid var(--brd);"
                "border-radius:10px;display:flex;flex-direction:column;"
                "overflow:hidden;box-shadow:0 24px 64px rgba(0,0,0,.6);}\n"
"#doc-modal-hdr{padding:10px 16px;border-bottom:1px solid var(--brd);"
                "display:flex;align-items:center;gap:10px;flex-shrink:0;}\n"
"#doc-modal-icon{font-size:14px;}\n"
"#doc-modal-title{flex:1;color:var(--acc);font-size:12px;"
                  "overflow:hidden;text-overflow:ellipsis;white-space:nowrap;}\n"
"#doc-modal-close{background:var(--bg3);border:1px solid var(--brd);"
                  "color:var(--fg2);cursor:pointer;border-radius:5px;"
                  "font-size:13px;padding:3px 9px;font-family:var(--font);"
                  "flex-shrink:0;}\n"
"#doc-modal-close:hover{background:var(--bg4);color:var(--fg);}\n"
"#doc-modal-pre{flex:1;overflow:auto;padding:16px 20px;margin:0;"
                "font-family:var(--font);font-size:12px;color:var(--fg);"
                "white-space:pre;line-height:1.65;background:var(--bg);"
                "tab-size:4;}\n";

/* ═══════════════════════════════════════════════════════════
 *  Embedded JavaScript[cite: 1]
 * ═════════════════════════════════════════════════════════*/

static const char JS[] =
"/* Toggle a node open/closed */\n"
"function tog(nh){\n"
"  var nb=nh.nextElementSibling;\n"
"  var arr=nh.querySelector('.arr');\n"
"  var open=nb.classList.toggle('open');\n"
"  arr.classList.toggle('r',open);\n"
"  nh.classList.toggle('open',open);\n"
"}\n"
"\n"
"/* Expand or collapse every node */\n"
"function setAll(open){\n"
"  document.querySelectorAll('.nb').forEach(function(nb){\n"
"    nb.classList.toggle('open',open);\n"
"    var nh=nb.previousElementSibling;\n"
"    if(!nh) return;\n"
"    nh.classList.toggle('open',open);\n"
"    var arr=nh.querySelector('.arr');\n"
"    if(arr) arr.classList.toggle('r',open);\n"
"  });\n"
"}\n"
"function expandAll(){setAll(true);}\n"
"function collapseAll(){setAll(false);openRoot();}\n"
"\n"
"/* Open only the root's immediate body */\n"
"function openRoot(){\n"
"  var root=document.querySelector('#tree>.node');\n"
"  if(!root) return;\n"
"  var nh=root.querySelector('.nh');\n"
"  var nb=root.querySelector('.nb');\n"
"  if(nb){ nb.classList.add('open'); }\n"
"  if(nh){\n"
"    nh.classList.add('open');\n"
"    var arr=nh.querySelector('.arr');\n"
"    if(arr) arr.classList.add('r');\n"
"  }\n"
"}\n"
"\n"
"/* ── File filter ── */\n"
"var activeFile=-1;\n"
"function filterFile(fi){\n"
"  if(activeFile===fi){ clearFilter(); return; }\n"
"  activeFile=fi;\n"
"  document.querySelectorAll('.fi').forEach(function(e){e.classList.remove('act');});\n"
"  var el=document.querySelector('.fi[data-file=\"'+fi+'\"]');\n"
"  if(el) el.classList.add('act');\n"
"  document.getElementById('search').value='';\n"
"  document.getElementById('mc').textContent='';\n"
"  var all=document.querySelectorAll('.node');\n"
"  all.forEach(function(e){e.classList.add('hidden');e.classList.remove('hl');});\n"
"  var mine=document.querySelectorAll('.node[data-file=\"'+fi+'\"]');\n"
"  mine.forEach(function(e){\n"
"    e.classList.remove('hidden');\n"
"    revealAncestors(e);\n"
"  });\n"
"  document.getElementById('mc').textContent=\n"
"    mine.length+' node'+(mine.length!==1?'s':'')+' in this file';\n"
"  document.getElementById('nr').style.display=mine.length?'none':'';\n"
"}\n"
"\n"
"function revealAncestors(el){\n"
"  var p=el.parentElement;\n"
"  while(p && p.id!=='tree'){\n"
"    if(p.classList.contains('node')) p.classList.remove('hidden');\n"
"    if(p.classList.contains('nb')){\n"
"      p.classList.add('open');\n"
"      var nh=p.previousElementSibling;\n"
"      if(nh){\n"
"        nh.classList.add('open');\n"
"        var arr=nh.querySelector('.arr');\n"
"        if(arr) arr.classList.add('r');\n"
"      }\n"
"    }\n"
"    p=p.parentElement;\n"
"  }\n"
"}\n"
"\n"
"function clearFilter(){\n"
"  activeFile=-1;\n"
"  document.querySelectorAll('.fi').forEach(function(e){e.classList.remove('act');});\n"
"  document.querySelectorAll('.node').forEach(function(e){\n"
"    e.classList.remove('hidden','hl');\n"
"  });\n"
"  document.querySelectorAll('.nb').forEach(function(e){e.classList.remove('open');});\n"
"  document.querySelectorAll('.arr').forEach(function(e){e.classList.remove('r');});\n"
"  document.querySelectorAll('.nh').forEach(function(e){e.classList.remove('open');});\n"
"  document.getElementById('search').value='';\n"
"  document.getElementById('mc').textContent='';\n"
"  document.getElementById('nr').style.display='none';\n"
"  openRoot();\n"
"}\n"
"\n"
"/* ── Search ── */\n"
"function doSearch(){\n"
"  var q=document.getElementById('search').value.toLowerCase().trim();\n"
"  var mc=document.getElementById('mc');\n"
"  activeFile=-1;\n"
"  document.querySelectorAll('.fi').forEach(function(e){e.classList.remove('act');});\n"
"  document.querySelectorAll('.node').forEach(function(e){\n"
"    e.classList.remove('hl','hidden');\n"
"  });\n"
"  document.querySelectorAll('.nb').forEach(function(e){e.classList.remove('open');});\n"
"  document.querySelectorAll('.arr').forEach(function(e){e.classList.remove('r');});\n"
"  document.querySelectorAll('.nh').forEach(function(e){e.classList.remove('open');});\n"
"  if(!q){\n"
"    mc.textContent='';\n"
"    document.getElementById('nr').style.display='none';\n"
"    openRoot();\n"
"    return;\n"
"  }\n"
"  var hits=0;\n"
"  document.querySelectorAll('.node').forEach(function(el){\n"
"    var name=(el.dataset.name||'').toLowerCase();\n"
"    var label=(el.dataset.label||'').toLowerCase();\n"
"    var addr=(el.dataset.addr||'').toLowerCase();\n"
"    var props=el.querySelector('.props');\n"
"    var ptxt=props?props.textContent.toLowerCase():'';\n"
"    if(name.indexOf(q)>=0||label.indexOf(q)>=0||\n"
"       addr.indexOf(q)>=0||ptxt.indexOf(q)>=0){\n"
"      el.classList.add('hl'); hits++;\n"
"    } else {\n"
"      el.classList.add('hidden');\n"
"    }\n"
"  });\n"
"  /* Reveal ancestors of matching nodes */\n"
"  document.querySelectorAll('.node.hl').forEach(function(el){\n"
"    revealAncestors(el);\n"
"  });\n"
"  mc.textContent=hits+' match'+(hits!==1?'es':'');\n"
"  document.getElementById('nr').style.display=hits?'none':'';\n"
"}\n"
"\n"
"/* ── Jump to Label Reference ── */\n"
"function jumpTo(label) {\n"
"  var target = document.querySelector('.node[data-label=\"' + label + '\"]');\n"
"  if (!target) return;\n"
"  revealAncestors(target);\n"
"  target.scrollIntoView({ behavior: 'smooth', block: 'center' });\n"
"  var nh = target.querySelector('.nh');\n"
"  if (nh) {\n"
"    nh.classList.add('jump-hl');\n"
"    setTimeout(function() { nh.classList.remove('jump-hl'); }, 2000);\n"
"    if (!target.querySelector('.nb').classList.contains('open')) tog(nh);\n"
"  }\n"
"}\n"
"\n"
"/* ── Count nodes per file and show in legend ── */\n"
"(function(){\n"
"  var c={};\n"
"  document.querySelectorAll('.node').forEach(function(e){\n"
"    var f=e.dataset.file; c[f]=(c[f]||0)+1;\n"
"  });\n"
"  Object.keys(c).forEach(function(f){\n"
"    var el=document.getElementById('fk'+f);\n"
"    if(el) el.textContent=c[f];\n"
"  });\n"
"})();\n"
"\n"
"/* Open root node on page load */\n"
"openRoot();\n"
"\n"
"/* ── Doc file popup ── */\n"
"function showDocPopup(id){\n"
"  var pre=document.getElementById('docdata-'+id);\n"
"  var title=document.getElementById('doctitle-'+id);\n"
"  if(!pre) return;\n"
"  document.getElementById('doc-modal-pre').textContent=pre.textContent;\n"
"  document.getElementById('doc-modal-title').textContent=title?title.textContent:'';\n"
"  document.getElementById('doc-modal').classList.add('open');\n"
"}\n"
"function closeDocPopup(){\n"
"  document.getElementById('doc-modal').classList.remove('open');\n"
"}\n"
"document.addEventListener('keydown',function(e){\n"
"  if(e.key==='Escape') closeDocPopup();\n"
"});\n";

/* ═══════════════════════════════════════════════════════════
 *  Embedded logo (platform|tree, base64 PNG)
 * ═════════════════════════════════════════════════════════*/

static const char LOGO_B64[] =
    "iVBORw0KGgoAAAANSUhEUgAAAwIAAACXCAYAAACm/+zVAABbpElEQVR42u29eXxU9b3//zpz"
    "Zt+yryQhhEAg7CD7Dgq422LFamv7rct1K/baaq29Lr29tT+vXrW2Vu21Vq11QRRBZEc2wxog"
    "QFgDAZKQfZ/MPuec3x/ciUDmTGYm5zOZhPfz8eBRm5n5zJnPWT7v1+e9cZmZmRIIgiAIgoh5"
    "JEmCKIoQBAGCIMDn83X+tyiKEEURkiR1/uM4DgDAcRxUKlXnP57nwfM81Gp153+rVCqaYIK4"
    "ylDTFBAEQRBE7Br+foPf4/HA4/F0Mfi7+/ylAuJK/AKB4zio1WpotVpotdpOYeAXEgRBkBAg"
    "CIIgCCIK+Hw+uN1uuN1ueL1eCILAVGj4v9PlcnWKAo1GA71eD51OR4KAIEgIEARBEATBClEU"
    "4Xa74XA44PF4ut3tZ4UkSfB6vfB6vXA4HFCpVDAYDNDpdCQKCIKEAEEQBEEQShndgiDA6XTC"
    "5XLB5/P1mgAIJlDsdjucTic0Gg2MRiN0Oh3lFBAECQGCIAiCICJBEIROA5tV6I/SgsAfrqTV"
    "amE0GmEwGMhDQBAkBAiCIAiCCFUAOBwO2O32gAm8oeKv/nNpJSD/f19pnPuTi/3VhS7930g8"
    "EB6PB16vF3a7HWazGXq9ngQBQZAQIAiCIAgiEJIkweVyoaOjA16vN6zP+qv7XFrZ59JyoOEa"
    "4ZcKA0EQLktKDqUakf/3eL1etLa2Qq/Xw2w2Q6PR0IkmiD4ER30ECIIgCIItPp8PNpsNLpcr"
    "rB14nueh1Wqh1+s7y3qyFCr+JGGXyxV2wjLP8zCbzTAajeQdIIg+AnkECIIgCIKhce10OmGz"
    "2ULOA1CpVJ3Gvz/kJmTDmgM0Oh7GRC10VjV0ZjV0FjVMyTrozGqotSrwWhU4noMoSBDcInxu"
    "EV6XAGeLB/YmD7wOAc42DzqaXGhrtIWcxCwIAtrb2+FyuRAXFwe1mkwMgiAhQBAEQRBXIaIo"
    "oqOjAw6HI6RcAJVKBZ1OB6PRCK1WG5Lxr1Jz0Oh5GJO0SC2wIm24BcYkLYwJWmjNPHQmNTg+"
    "9N150SfB6xTg6fDB2e6FrcaF+tM2VB5qQN3ZZthbXRAFMajwcbvdaG5uhtVqpXKjBBHjUGgQ"
    "QRAEQSiMz+dDW1sb3G539wsxx8FgMHQKgFDQWzRIK7Qi+5p4JOWZEZdtgIpnZ3BLooSWSgdO"
    "7ahG6brzqD/TFtLvslqtFCpEECQECIIgCOLqwJ9AG0pCsE6ng9lsDskDoDXySB1mQfbERGRf"
    "kwidmQevjW4tf0mS4Gz14NDqcyhZeRYtF+zdigGj0QiLxUJ9BwiChABBEARB9F/cbjfa2trg"
    "8/mCvk+lUsFkMsFkMgU1kDmOg9bEY+DUJOROSURKgQUaA9/rv1MSJdSeasWOvx/Hub318HmE"
    "oL/BYDDAarWSGCAIEgIEQRAE0f/weDxoaWnpNilYq9XCarV2GwakMfLIn5OKodemIiHHCMRg"
    "dI3XJeDQqrPY8Y/jcLZ6gr7XYDAgLi6OxABBkBAgCIIgiP5DKJ4Af5iM2WwOWgZUY+CRMykR"
    "I27KQHyOESp1bMfXi4KEypJGbH2zFBeONgf9/Xq9nsQAQZAQIAiCIIj+QSg5Af5QILPZLJsL"
    "wKk4JOQYMeLWDAycnAS1rm8Zy43nbNj46iGcK66HJMqbFmazGRaLhRKICYKEAEEQBEH0XQRB"
    "QEtLCzweT1AREBcXB4PBIPsejZ7H4LkpGH9nDrRmvs/Oh6PVjW/+cgSHvz4vb3hwHCwWC0wm"
    "E4kBguhlqI8AQRAEQUSAJElob28PKgJ4nofVapUVARzHwZqpx/i7c5A1Lj7qVYCUxhivw7xH"
    "R0GlVuHImvMQvGLAeevo6IBarYZer6cLiSBICBAEQRBE3xIBDocDLpdL9j3+OvrBRED6CAsm"
    "/jQXCbkm9JfNcWO8DnMfGgmv04djG6sCdiQWRRHt7e1Qq9XUgZggehHeYrE8T9NAEARBEKHj"
    "8XjQ1tYW0MgFvgsHMhqNgRdfjQp5s1Iw49F8mFN16G8RMho9j9wJKWiqsKG5sgMIME2iKEIQ"
    "BOo+TBC9CKXtEwRBEEQYiKIIm80GURQDvs5xHEwmk6wngNeoUHhjOib/LBdaE99v50lv1WL+"
    "o6OQOSJR9j1utxsOh4MuKoIgIUAQBEEQsY/D4QhaIUin08kmwvJaFYYtSseYO7KhMfL9fq4S"
    "ssyY9/BImBJ0AV/3h1iF0oWZIAgSAgRBEATRa3i9XnR0dMiGBGk0Gtk6+Sqew5jbs3DNjwf2"
    "udKgPSFrdBJmPzgCWmPgXACfzxd0TgmCICFAEARBEL2KJEmw2+1BQ4IsFkvAZmEqNYch16Zi"
    "+A3p4Pira944FYcRC7IxZEam7HtcLlfQ6ksEQZAQIAiCIIhew+v1Bq0SZDAYoNMFDoHJmpCA"
    "cXdmQ2Pgr8q50+jVmHrPUMSlG2VFFnkFCIKEAEEQBEFEFUmSIAgCfD4fPB4PPB4P3G433G43"
    "PB4PvF4vvF5vUG+AWq2W7RqclGfGlPsGQW/VXNXznDLIisk/HAK1LrAY8ng8QYUWQRDKQ8V7"
    "CYIgiKsGURQhiiJ8Pl+ngS8IAiRJ6tyNvnJX2m/cy4kA4KI3IFA9fFOSFuPvyoYhQXPVzz2n"
    "4lC4IBul6ytRfaw5oCBzOp3Q6/VUTpQgSAgQBEEQhDJ4vV44nc5O4z+YUR/2QqpWy/YLGHZD"
    "BjLHxoPs2osY43WYcHsean7fjEBRQG63Gz6fDxoNCSeCICFAEARBED0w/v116v27/krDcRyM"
    "RmPXBGEOyBqfgILr0kgEXMHgKenIHpOMipLGLq/5y4nGxcXRRBEECQGCIAiCCB1JkuDz+eBw"
    "OOByuSAIAtPvU6lU0Ov1Xf5uTNBi5C2Z0Bp5OilXYIjTYuisTFQfa4bP09Uz43a7IQhCwOpL"
    "BEEo/AyjKSAIgiD6Az6fD+3t7WhqaoLdbmcuAgBAr9cHzA3InZqEtBFWgLwBXeBUHIbOzoQp"
    "UR/wdUEQqJQoQZAQIAiCIIjuEUURHR0daGxsDFrZJ2RDleOgUqnAcVzQpFWNRgOTydTl75Y0"
    "PQpvzKCQoCDEZ5iQMy454GuSJMHj8VApUYKIAhQaRBAEQfRZfD4f2trawjYc/cY+z/NQq9XQ"
    "aDTgeT6g8X9peVF/roFGowlYKUjFcxg8OwWmZC2dnKAnAMifnoHS9ZWQxK7nzX8+qXoQQZAQ"
    "IAiCIIguOBwO2Gy2sEKANBoNdDodtFottFotVCplHePGJC0GzUgCx5MB2x2J2WZY0wxoq3EE"
    "FHiCICh+fgiCICFAEARB9GH8oUB2uz0kLwDP89BqtTCZTNBoNBHtMqtUHFQaDhzPgeMASQIk"
    "QYLglS7b0c6ekABrpoFOUghojRqkDYkPKAT84UFURpQgSAgQBEEQRKcIaG9vh9Pp7FYE+Cv6"
    "mEwmqNXqkASASsNBa1BDb9UgfqARcQP0sKTqYYjTQK1TXdzp5wD8nxDwugW4Wn2wNbhgq3Fj"
    "KJULDe08+iSodSok5Zih4jmIQtdz6fV6aaIIgoQAQRAEQVysJuMXAcHgOA46nQ4WiyWkHWWO"
    "42BO0yFtuBXpI6xIG2aBKVUHFYX3sBMCgghIQOJACzR6Ndz2rka/z+ejPAGCICFAEARBXO1I"
    "khSSCOB5HhaLBQaDIagByXGAMVGLlAIL8uemIjnfBK1RDZW6d41Od4cXtSdb0VRhg86sQUZB"
    "AhKyTOBU/csYFnwiJElCXLoRah0fUAiIoghRFKmfAEGQECAIgiCuZhHQ0dHRrQjQarWwWq3Q"
    "arVBBAAHfbwag2emIG9WMuKyjOA1sWFku9o9WP8/JThdVAu33QtOxSE514JJdw7ByEU54DWq"
    "fnM+Rd/FEq+WFAPUOpX8+0gIEAQJAYIgCOLqxW63o6OjI+h7DAYD4uLiglaZ0ZnVGDQ9GcNv"
    "zEBcpj6mmn1JooR9n53B8c1VnfHykiihobwda188CEebBxPvGAy1tu8bxZIICN6Lv1Gj56G3"
    "aGUThnvaE4IgCBICBEEQRB/F6/Wio6MjaGKwyWSCxWKRFQEqnkNqgQUTfjQQSXkmqDSxF2bj"
    "cfhQX9YaMGlWFEQUvXccglvA1HsK+rxnQBTEizkC/vOXoJMXDdRUjCBICBAEQRBXH/7k4GC7"
    "wgaDAVarVTYfQGPgUXhTBoYtSIchMXZLUfJaHsYEXWdp0i5Cwe7Dzn+ehClJjzE35/bpRGaf"
    "+/K+D1qTWlYEkBAgCLZQpw6CIAgiJnE4HHC73bKv6/V6xMXFyYoAS5oeMx4djHF3Zse0CAAA"
    "tVaFa36QD0O8LqgBveWvR3Doq7N91kCWJEDwXC7sNDo+yPtJCBAECQGCIAjiqsLn88Fut8sb"
    "zmq1bE4Ax3FIyjNh9i+HIntSYp/5zcmDrBh366CgFYJcNi+2vnkUFQca0Zs2ss8jorXajsaz"
    "7ehodIVssAseoWv4E5UHJYheg7dYLM/TNBAEQRCxgr9KkMfjCfi6SqUKWh0oeYgZMx7OR1Ke"
    "qU/VoOc4wJpqQMXBRtib5T0hPreAhjNtyBqVBFOiPurH6WzzYOubpdj1wUkcXn0eZ3bVQhIk"
    "JA+yglcH2V+UAI/TB8F7uUfg7O46NJS3BxR0er2eugsTBAkBgiAI4mrB4/HAZrPJ7jIbjUaY"
    "TIGN/MRBJsz9VQHiBhj65G/XW3WQRAnn9tVDEuV32TuaXKg52YKCWZnQGKKX7id4RWx4pQQl"
    "K8/C0eKGu8MLW70T5XvqYU01IGNYguxnRVGCu8PbxZNxans1mis6AgoBg8FAQoAgGEKhQQRB"
    "EERM4XQ6ZROEeZ6H2WwOLAJyjZj2b3kwp+r67G/nOGDkohwk5pi7fW/N8RZsfasUrg5v1I6v"
    "qcKGykNNXY18n4hjGyuDftbnFgJWRXK2e2TmgqOuwgRBQoAgCIK4WhBFES6XS/Z1o9EYsMGU"
    "WqvC+LsHIinf3OfnQG/W4JrbB3dfGUgCjm6swpE156OWL9B0zgZ7c+DzY2uQb/gmSYDX5ev6"
    "d1GCs1VeCATrC0EQBAkBgiAIoh8RzBugVqthMHQN+eG1Koy9IxtZ4+P7Td5p/owMZI1K6vZ9"
    "XqcPW94oReWhRvYiTZBQc6IFHocv4OuCV+zsGHwlPrcA0ddVrdhb3PA4fSQECIKEAEEQBHE1"
    "I0lSt+VCA3kDMsfEIX9eSr+aC3OSHsPmZwVPvvUb2R4BW/9aiqbzNqbH5HMLqDneIn/+RAk+"
    "txjw716XL2DOh63BCZ9HkBUCgc43QRAkBAiCIIh+hiAI8Pl8QYXAlTHjvIbDmO9nQR/XvxJK"
    "ORWHEddlw5QUWlWgC0ebsePvx4MmGPcUj8OH2pOtQYQc4PN2NeoFryhr7LdesMPrCvwaz/OU"
    "I0AQJAQIgiCIqwGfzwdBkDcKrywXynEcht+QgeSh5n45H4Y4La65Y3DQvgKdRrgo4djGSuz7"
    "7EzAhFwlOH+gAe4gicmShC7fLUkS3HYvEOCQREFCU0VHl07Dl35W7nogCIKEAEEQBNGP8Hq9"
    "siVDdbqulYDMaToMmpHcr+dk8JR0JGaFLnT2Lz+NC6VNih+HJF0sadrNu4ArPBJeV+DcAOCi"
    "p8DZ5pEdze12o7W1NaiXiCAIEgIEQRBEPyCYwdelljwHDBgbj4QcY7+ek8RsMwZOSAk5Cbql"
    "yo5tbx2VTdqNFHuTG7WnWrt936U6QBIkeB0+WXHHa1QwJeiChv+43W60tLTA6/XSDUIQJAQI"
    "giCIq00IqFQqqNWXN81Sa1QYfmMGVOr+HUPOa1QYfeNAqHWhJ81WlDRi90dlioqBmuMtaK91"
    "dPu+S216t8MLIcgxqHgOIxZkI60gLuiYXq+XxABBkBAgCIIg+jNy8eAqlapLGcmUAjPiMvRX"
    "xbykD0tA2pD4sD5zYEV5wMZfkSD6RFQeboTL5ulGBHCdvQ98bkE2CfhS4jKMmPvQSORNSQOv"
    "UQUVie3t7ZQzQBAkBAiCIIj+hiRJsiEkV3aY5Tggd2oycJUUlFHxHEbfNDCkpGE/7bUObHmz"
    "VLZaTzh43QLKd9WF1LRMpVZBEiV4HL6QKxglDbTg2qWjMfaW3KDlUt1uN9rb22WvE4IgSAgQ"
    "BEEQfVQIyHGlEDAkapGUZ7qq5idnXApSB8eF9ZnqY83Y+3HPQ4Tqy9rQUN7W7fs4DuDVKrjt"
    "vrAFiNaoxqQ7h2D2vxVCZ5YvBet0OmG32+mGIQgSAgRBEMTVIASuxJKqhylJe1XNT1yGEVmj"
    "k8LrnCwBJavOoTpIE7BQzsvxby6E5A3weyy8rsiq/PAaFYbNHYCJd+RDo5fPibDb7fB4PHTT"
    "EAQJAYIgCKI/EE7jKHOqDrp+1kCsWyNZrULBnEyo1OEt263VdhS9eyLicJq2GgfOH6gP6b1q"
    "LR9WSFBAo0StwshFORh36yDZnAFBENDR0UEhQgRBQoAgCILo70LgsvwBDogbYOhMSr2ayBmb"
    "gvjM8EOizu6tw7GNVQjbbpaAigMNaK0KLRRHY+AhCj2vVKTWqjD++3kYPDVd9j0ulwsul4tu"
    "HIIgIUAQBEH0ByEgJwYuFQIcOJhTdFfngq3mkD89I+zPiaKE/cvPoK0mvNh6n0dAWVEtvO7Q"
    "4v11JuW8NGodjwmL85CYLd9MzeFwQBRFunkIgoQAQRAE0dfheV7GkBW/M/g4wJiovWrnaPDU"
    "NOhMahkxJf+5C0ebcXj1+bC+q/WCHWd21Yb8fkOcsuclMceMMTfnQq0NfF14PB643W66cQiC"
    "hABBEATR1+nSPfgSIXBpszGN8epdupJyLEiRqR7EqzgUDooP+JokSij+7DRaLoTmFZBECQdX"
    "nYPPHXr1H2OCsp4ajuMwdFYmUvKtgY9RkuByuShXgCBICBAEQRB9nSu7B1/KpVVieP7qXbpM"
    "SXqkD40PuPvvEyRMGZmK5PjAjdZcdi+Kl52G4Ok+nKblgh3n9tWFdWxKewQAQKPnMXJhjmxO"
    "iNvtpvAggiAhQBAEQfR1NBqNbJ6A2+3u3Pn1+a7eHWAVz2HQpDSoZMSQ1ydi8bzcwB+WgOOb"
    "q3DhaHPQ75BECae/rUFzRUdYx2ZJNjD5zQNGJMp6QURRpPAggiAhQBAEQfR1eJ6X9QqIotjp"
    "FfA5hat6nrLHJUNjDDxPpyrbcNeiwUiwBg7T6Whyofiz00HDaVwdXhxcdRZCGI3IVCoOpiQ2"
    "SdzGBB0GjEqU7axMQoAgIkdNU8CeG2+8ES+99JKiYz7++ONYt24dTe4V/PWvf8WcOXMUG8/h"
    "cGDatGlRaV5jMpkwcuRIDB8+HFlZWUhLS0NcXBx0Oh2sVit0Oh30ej2sVitUKnYaftGiRaio"
    "qKCLiegVIaDRaOD1egO+7nQ6odPr4OrwxtyxC14Rgvei4aziOfAalazh2lP0Zg0yhyegfE/X"
    "0J3yCzZ4vSLuu3Uo/udfpRAD1PQ/s6sW9afbkDYkPuD4Z3bVoumcLaxj0pk10BrYmBQqnkPm"
    "8EQcWVMBr7Nrs7KCggJ89tlnSElJUeT7nnrqKaxatYpuSIKEAKEMN998M+Li4hQbz+PxoKio"
    "iCY2gCG9cOFC6PV6xcb89ttvmYoAnudx22234fbbb8e0adOCxkhHgxMnTpAIIHoNjuOg1+vh"
    "cDgCvu52u+H1eNBRE1v145vO21D0j+M4s/uiYZ6Ua8GAwkSkDY1H2pA4JA60gFcrK94HTUwN"
    "KAQaWlzYe7QBP7ulABt2X8Chsq5hQF63gH2fnMbCJ8Z16eBrb3bjwBflYR+PMUEbtBtwT0kv"
    "iINWzwcUAi0tLWhqakJ+fn6Pv8fn82H79u10MxIkBAhl0Ol0mDt3rqJj7tixAzabjSb3CubM"
    "maOoCACANWvWMDve0aNH4/XXX8eQIUNiZg6//vprupCIXn9m8jwPQega/iMIAux2B2x1bkii"
    "xGzHPRwcLW5sfPUQzu6r7+yoW3WoCVWHmqDW8tCZNbCk6JEzPgW5E1KRMtgKS7I+7A7BV5JW"
    "EA+dSQO3vat35JviavzyR6Pw4xvycehPe7t+WALO7q1H7ckWZI9J/u7PkoRT2y6g7lRr+Bsx"
    "iXpoDOxMCr1FC0uaAfaWrmFAbW1tqKurU+R7du7cidbWVroRiasGyhFgzKxZs2A2mxUdc+3a"
    "tTSxAbjhhhsUHc/j8WDz5s1MjnX+/PlYuXJlTIkA1sKHIEKB4zgYjUbZ151OJ1oudMBjj408"
    "gYbydpwr/k4EXIrPI8De7ELtyVbs/bgMn/9mFz57cifWvVyCM7tq4e7wBvxcKFhTjYjLCDxP"
    "e481QhAk/OTGIUhPCpzAa2t04sSWC5f9zesUsH9FObwuQebcBBECCTqmHgEAiM8MvJba7XY0"
    "NTXRZghBkBCIPa6//npFxxMEARs2bKCJvQKtVov58+crOiYrz0thYSHefvttaLWx1RSpvLwc"
    "J06coIuJ6HUMBoNsmJwkSag4VofWC/aYOFZHixuiEJoxL3hF1Je1oWTlWXz6eBHeWrIeG14p"
    "wemiWnjsvrC+15JqQMKAwIZxe4cH+080IsGqw9IlI8DLlN48suY8Ohq/C7M6vOY86svaAr7X"
    "bNBgWG58YIGg4mBJNcg2/lIKc2LgZGSv14vW1tYe9xMQRZHWV4KEAKEcarUaCxYsUHTMvXv3"
    "Krbz0Z+YOXMmLBaLomOy2BlSq9V47bXXYDAYYm4OaSeMiKVnZzCvQGttB45fsZvdW6QVxCMh"
    "KzKvr73Zjf1flGPlc3vx4aPbUfTeCTSebYfP0723Q6PnkZRrkd2l332kHgBw44xsFOQEzlFz"
    "2304urESknQxz+HQV+cCG/ocMH9SBqzGwA3feI0K8Zkm5nOtNcmHHjmdzh4LgX379qG+vp5u"
    "QIKEAKEMU6ZMQUJCgqJjUuhGYJQOC/L5fNi4caPix3nnnXdixIgRMTmHJASIWMJoNAYtJVq8"
    "vAwXSpt7/TjjM0249rHRkRvCEuC2e1F7ogXb3j6KTx8vwrr/PogLpU3dehrShsTJ5kmUnGqC"
    "IEooGBiPhVOzAn+1KKF8dx3szS6Urq9Aw5nA3gCjXo0bp+fA7Q0sUNRREgJckNgkQRB6LATo"
    "GUhcjVCyMEMWLVqk6HiSJFHJ0EAXMQPPy65du9Dc3Kz4cS5dulTx32+327F3717YbDa0t7dH"
    "NIbT6URpaSldTETMoFKpYLFY0NbWFrBzrMfhw+r/KsbNz16DzMLE3jtOnsOQGRkYMDIRez8p"
    "Q8mqc3C0RF7Xvq3WgcNfn0fpugpkjkjExDvykTsxFQZr11DCzBH/V1s/gGAov2BDXZMTmSlG"
    "PLR4OP6y7Bi8AfoCXDjahKMbKrH3k9OywmPB5CwMzYlDQ2vg36XW8xF7RcIhmKdEr9cHFQq0"
    "vhIECYGownGc4vkBhw4dQnV1NU3uFUyaNAmJicoaAiw8L/Pnz8eAAQMUHfPEiRO4++67UVtb"
    "SxcC0e/Q6/XweDyw2wPnAzRV2LDhfw7he3+YjLg0I9CLRYSM8TrMvLcQ+dMysOvDkzi7tx4+"
    "d+QJzaIgoepwE+rL2pCUa8HEO/IxaFIqTInfVUazphqgt2hhb+5aTrWu2Ym65otCIDfDjLkT"
    "MrBhT9dwKo/dh2/fPR6wLCcAZCQbsfTOQpypakdTa+CyrQkDzMwThQHA2eaRFY0mk6lH/VUO"
    "HTqECxcu0E1HXH2bLjQFbBg3bhzS09MVHZOqBQVG6bAgURSZ7Azdeeedio7n8/nwyCOPkAgg"
    "+i0cx8Fiscj315CA6uPN+PoP+9FyoaPXj5fXqJA1Ogm3PDcJs+4vhCGu5wUBPE4fao634OsX"
    "9mP5r3fhxNYLnTvjnIpDYk7gnfjGVhca/s9wV6k43HFtHrQyJUvdMg3aVCoOP5g/CGOHJqG0"
    "vAUumR35tKFxUZnf9npnwL8bDAYkJSX1aGwKuyVICBAxbZySEJA3FBYuXKjomCwSxlJTUzFv"
    "3jxFx1y9ejVV+SH6/yKlUiE+Ph48z8uKgXPF9Vj+1C40nG0HpN4/Zp1JjSl3DcU9b83BkJmZ"
    "UPE9d1UIXhEXSpux4rd78MEDW1H2bQ28Lh9SBwc2wtvtXpyr/q7q2ZRRKRiSE57BnpNuxhM/"
    "Hg0A2FMa+JnIqThkDE9gPqeCT0RLZWCxZ7FYkJqaSusrQZAQiB2Uzg84efIkzpw5QxN7BWPH"
    "jkVmZmbMC6477rhD8a7B7733Hl0AxFWBRqOB1WoNGgPeUN6OL5/Zg5PbLoRczpPtLsXFDsM3"
    "/GY8pv1kmCLeAeBigm/tyVZ89Z/7sPK5fRADxP37Kb/wnRAYNMCCUfmhG+w6LY/HloxARrIB"
    "bR0eHD7dEvB9pkQdLCnsq6C1VNkDNk8DgMTERGRlZUU89vHjx1FeXk43GkFCgFCG4cOHY9Cg"
    "QYqOSW7LwNx4442KjidJEpO5Xrx4saLjVVdXo7i4mC4A4qqA4zgYDAZYLBZ5MSABDWfa8fUf"
    "9uPginJ4nL6YOHZTgg4z7x2ORb8ah/gBylXWcdm8OLW9GiWrzsm+p6L2ux10nYbH/Imhb5rM"
    "nZCBJQvyAAB7jzagwyFjhGeZAyYyK03tyVbZRmcpKSk9EgJULYggIUAoCoUFRQ+lw4JYJIwN"
    "HjwYQ4cOVXTMVatW9bhUHkH0NUwmU7eeAVeHFxtePYRlvyxCzfGWmPAOcCoOw+dn4c5XZ2DQ"
    "pLSQqtsY9WpoNd0v0cGeA+drLg+lmXtNBlQhfHdSnA7/+eAEpCboIQFYt6tKVqAl51mht2iY"
    "zp/H4UPV4UYI3sDej9mzZ/eoNwtttBEkBAhFUbpaUGVlJY4ePUoTewWFhYXIy8tTdEwWC8LN"
    "N9+s+JirV6+mC4C46uA4DkajERaLJWiFGEmUUFHSiM9/sxvb3j6K9nonJLGXBQEHJGabccNT"
    "4zHqhoFQa4Mvv1aTBvfcMATjCpJg0EUWVtjcfnmVnQEpJuSkBy/zqeZVuPfWAowbejH5tqHF"
    "icOnA5dSVutUSMuX72WgFO11DtQcbw1sxKhUPeoqX15ejpMnT9LNRZAQIJQhJycHw4cPV3RM"
    "cltGR3ABYFItSOnwperqapSUlNAFQFy1YsBkMiEuLi74zrp00YDc9eFJfPLYDuz+1ynYm929"
    "fvxxGUYs/NVYTLwjH3yQHf/aJie2H6zBS49Nwuu/moqhOeFX5mmze64wmjkMyw0+zvQxqXj4"
    "9uGdHYuPnG5BRW3g8q0agzoqicKnd9YGLJEKAAUFBRg9enTEY9OmCkFCgIhpow+gsCA5lA7B"
    "On78uOIJ2YMGDUJhYaGiY65evZrCgoirXgwYDAYkJiZCq+0mPl0CGs/ZsPWto3j3p5ux4ZUS"
    "VBxsgMvm6bXj1+h5zLyvEPN/Pho6k3xYzamKdjz+yh6MGpyALW/dgJcfm4S8ARaE2jfL5fZd"
    "MW8IKiiS4/X4/x6diAGpF3MZBFHCzsN1aGgJXLYzbUgcjAk6pnPVWm3HiW8Ch2uqVCosXrwY"
    "ZnPkzcxofSWudqihmMIoXS2ovr4eBw4coIm9gtzcXAwbNizmBReLfBHyEBHERXQ6HXieR3t7"
    "O9xud1CBLIkSbA1OFC8/g6MbKpE00ILsscnIGZOM5Dwr1Fq+s8yn4BVhb3ZB8IqwpBlhStAp"
    "UgL0UnGi1vIYd9sgSJDw7TvH4WwPLExKz7Rg6f/swicvzMNjd47A7PEZePuLE1i26Sza7cHF"
    "jJq/fK9PxXEYOTgBGrWqS5dhrUaFR34wHNcUpnT2ZXO5BXxTXIOA08oBOeNSmJ5fn0dA6fpK"
    "OGQ6Gg8YMADXX399xBXZqqqqcOTIEbqRCBIChDKkpaVh/Pjxio65bt06iKJIk3sFLDwvLAxs"
    "pYVAXV0d9u/fTxcAQfgXMbUaCQkJcDqdsNlsEAShWyPc2eZB1eEmVB1uwi6chFrPw5JsgNag"
    "BiDB1uiCs80DSZQQn2HClB8NRcGczKChPGHpAOm7Yxk+LwscgC1vHg3Y3VeUJOw92oClL+3E"
    "m09Nx7iCJLz51HTcOCMb//PhERQdroOc/rEG8DYsnDoA8yZmYv0lCcAqjsMP5ufhocXDL2vO"
    "XFVvl+0fYLBqkTEsnum5rTnWguObq2QTvufMmYOJEydGPP6aNWvIu0rQM1TpAe+9917FK6QE"
    "f6BKaG9vv7iLIwjo6LhYJcHj8cDpdMLn86GlpQXNzc1oaWlBVVUVXC4Xk2NZtGhRj1qcywkB"
    "VhiNRiQmJiIxMREAYDabwfM8eJ7vdLXq9Xro9fpeuTgvXLiALVu2yM61ktTW1sLpdGLgwIER"
    "HafP13UBz8zMxJgxY5RdGGtqcPfddysy1ieffBLwuJXEYDAgLi4O8fHxnSEc/qovGo0GRqMR"
    "wMVQD0mSLruf3W53573a1tYGAHA6nWhpaYHX643KNWixWHDrrbcqOuaxY8ci8vJxHIeEhARY"
    "LBYYDIbO+TSZTJ07ov655TgOcXFxKC0tlc0nmT59uuJljnsTSZLQ3NyM/fv348SJE2E9530u"
    "AS1VgZtVtdbY8c0bR6Aza5A9JonJsQ+ZkQGfR8TOD07CJeMZWLurCk/+eR/efGoazAYNbpqR"
    "gznjM/Dqx6X4+6pTuFB/eRw/z3O4dU7X59mAFBP++bvZ+MuyYyg+3ggOwOwJ6bj/1mGwXCEc"
    "lm0sh0emUk96QTzT/gG2BieK3jsh2/U4ISEBv/rVr6DRRF6x6PDhwxE981tbWzufSVcyefJk"
    "DBkyRNG5WL58eUR2i16vR0JCAnQ6HaxW6/9dF9+t75c+g/1r/bJly+DxeJg/V61WK8xmM8xm"
    "MywWC8xms2zej8fjgcPhgMvlgtvtht1uR11dHRobG+F2u/vcs0qtVnfOwZXz4D8fV2K32+H1"
    "ejs3OxobG9HU1ISmpqYebxYrKgS0Wi2eeOIJWCyWmD4J9fX1KC8vR2lpKQ4ePIhdu3ahrq6u"
    "x+Mqnbza1taGoqKiHo3B8zyGDBmCcePGobCwELm5ucjNzUVmZmavGfih8uKLLwYUAunp6Rg3"
    "bpyi35Weno6dO3eG/bmWlhaMHTtWVqxwnLLVNMaOHSv7feEaox9++GGPx/FfXwUFBRg2bBgG"
    "DhyIzMxMDBgwAMnJyd3Hb0dIR0cHWltb0dDQgAsXLqCiogKVlZU4fvw4SktL4XQ6FRP3L774"
    "oqLH/sADD8i+ZrVaMXbsWBQUFGDgwIEYOHAgsrOzOwV7uNfTT3/6U9nXXn75ZeTk5PS73S23"
    "243NmzfjjTfewLZt2+BwOHq86+tx+HB2bx0zIcCpOBTMzoTgvSgGAnkGBEHCsk3lGJWfgF/c"
    "ORJajQoWkwZP/WQMrps8AL95oxiHy5rg8YrQalS4YXo2li4ZEfD7Eq06PHPvOHi8F70nOm3X"
    "rs1NbW5sLq4O+HkVzyFjWAK0JjZlQ53tHuz5uAzNMp2EeZ7Hj370IxQUFPToe/7yl79E9Ln7"
    "7rtPNpT0hRdeUDRstaKiQvZZrVKpkJeXh9GjRyMvLw85OTnIzc1Feno6EhISZI1KOU6fPq3I"
    "upCTk4MRI0Zg8ODByM3NRUpKCpKTk5GWloakpCRF1wWbzYa6ujrU19fjzJkzOH36NE6fPo0z"
    "Z86gqqqqVzw+arUaI0eOxNChQzF48GBkZmYiLS0NycnJSEpKQlJSkmK2gSAInYLg/PnzOHPm"
    "DM6cOYOysjKcOXNGVrAyEwIzZ86MeREAAKmpqUhNTcWUKVMAAKIoYt++ffj000/x+eefR7RL"
    "Gh8fj6lTpyp6nBs3bozoWLRaLRYtWoRFixZh7ty5nTsBfQ25B+3111+vuIEdKZs3b5Y9Ryyq"
    "GinFV199FfFn4+PjcfPNN2Pu3LmYOnVqr1xf/h2UrKysLqJQEAScPHkSmzZtwtq1a3H48OGI"
    "v0fpEDS3242tW7detmBMmzYNN9xwQ+cOvVLXdkdHB7Zv3x7wtREjRvRLEQBczBu44YYbcN11"
    "12H9+vX49NNPsWLFCtjt9h6NW32sBaJPhErNpsYGr1Fh1KIcuGwe7F9+Bj6PGFAMvPzPIxiQ"
    "YsJdC/PAcRy0GhWmjkrFmtcWoORUMypqO5CdZsLUUalQBSnryXGBBYCf4uONOHEusBGhMaiR"
    "e00qWDyGBa+I4s/O4OSWalkjbtSoUXjssceg0+mifn253W5s27Yt4GuDBw9WPHdt/fr1l/3/"
    "tLQ0LFq0CAsWLMCECRMUtbkiLZ9tsVhw3XXX4brrrsOUKVOQmpoatfNhsVhgsViQn5+PadOm"
    "dXkGHjx4EMXFxSguLsauXbuYeRDy8/Nx/fXXY86cORg7dmzUNlp5nu+0aQNVrKyqqsK+fftQ"
    "XFyM3bt348SJE2yFAIvEyGigUqkwefJkTJ48GT//+c/xxBNPYNeuXWGNcd1110WcsBSuISxH"
    "XFwcHnzwQdx9991ISkpCX6asrAxlZWUxf51t2LAh8I5bYiImTZoUs/MbScm8wsJCPPbYY1iw"
    "YAGznX6lHoyFhYUoLCzE0qVLceDAAbz00kuyRnEwsTFr1ixFj+3bb7+F3W5HcnIyfvazn+Ge"
    "e+5BQgKb8oubNm2SXfRiWaQqhUajwU033YRrr70Wv/rVr/Dll19i7dq1OHnyJFpbW8Mez9Hq"
    "RnOVHcm5DDe7OGDszbkQPCIOrCgPGBvf3O7Gk6/vxcjBCRgzJLHz70a9GtNGp2La6J4bYV6f"
    "iK92nEdTW+BwlLQh8UjINin+8102L/Z9ehpHN1TIigCz2YznnntO8R4y4dzDDocjamuTXwjM"
    "nDkTDz30EGbOnKl4CHKkQiA3NxePPPIIvve97/WooRvLDaOZM2di5syZAID29nasXLkSf/rT"
    "n1BTU9Pz25XjsGjRItx///2YPHlyTD4Hs7KykJWVhe9973sAgFOnTuH999/Hhx9+2LmJqdjV"
    "pFarsWDBgj6/eAwaNAiffPIJbr/99rA+p/TC6nA4ZHcdAvHDH/4QO3bswNKlS/u8CAgmgmLJ"
    "wL5yd/dSFixYoLgwVIpjx46hvLw85PcnJSXhjTfewPr163HTTTfFtAgIxPjx4/Hxxx/jmWee"
    "Ac/zIX9u/vz5iu84btq0CY899hj27NmDxx57jJkI6G5RvxqEgB+9Xo8xY8bg2WefxapVq/DF"
    "F1/gD3/4AxYtWoTU1FRYrdbOnAu1Wi1rZLk7vKg+2gwwjjTQGNQYvzgPuRNTZb1Ddc1O/O5/"
    "D6ChlU2+W1uHByu3VQRMQuZUHIbMSFfWK/t/CdxF/ziB0nUVAb0hwEVv9wMPPMAk7DJcwzwa"
    "QsCf2/jll1/ik08+wezZs5mJgIqKipArKOl0Ojz//PPYtm0b7rrrrpgUAYGwWq348Y9/jB07"
    "doRt413JkCFDsHLlSrzzzjsxKwICMXToUPzhD3/Axo0bkZWVddF+V2rwSZMmdSad9nXUajVe"
    "eukllJWV4dChQ92+32QyYfbs2Yoew5YtW0KKczabzXjppZdwyy239KvFW86IiSUDu6ioSDbc"
    "oL+EBc2dOxevvvoqUlJS+vw19eCDD8LtduO///u/Q3o/i8pUS5cuRUZGBvPf6nQ6ZRPtBw0a"
    "pHj4Ql+A47hOF/rcuXMBXEzAq6ioQHl5OZqamuBwOLB582YsX768y+cFr4jzBxowdFYm9BYN"
    "02PVGtSY8bPhcLZ5UHO8JeB71u2qwlufH8dv/9/YoCFAkbByewVqGgPvesdnmhRvInbhaDN2"
    "fnAS9WWtCBbSfdttt+Hpp5/utfw2URSxcePGgK9lZ2dj1KhRin/n2rVro7L5EmoEQk5ODt59"
    "913FG6dGE4PBgFdeeQUVFRXYu3dv2J9fvHgxXnzxxT4jgOQEwQcffIAFCxYo5xHobztMWq0W"
    "v/nNb0I2lpR+MIVSLSguLg6fffZZvxMBwXYmYiksSG5nyO+O7OtC4Ac/+AHee++9fiEC/Dz6"
    "6KMhhRTo9fpOY1FJoiECAGDr1q2y4QtXkzcglE2c4cOH48Ybb8Q999yDBx98EE8//bTsAn+h"
    "tBl1Za1ROTZrigGz7iuEJSXw2uLxinjlo1IcKmtW9Hub2tz4eP0ZWTGVPSZJsWpB7XVO7Pm4"
    "DOv/pwR1p+RFAM/zmDt3Ll5++eVe9XgfOnQI9fX1sveV0l6KkBrmKUQo5bOHDx+OlStX9mkR"
    "cOk19eSTT4b9uQcffBB/+tOf+rQI8FNQUIDFixcr4xHgOE7xxWXPnj14/vnnLzPMDQYDrFYr"
    "EhISMH78eEybNg3Z2dnMJmnGjBnIzs5GZWVlVEWQ1+uV3XXwYzQa8dFHH/WotXqsIrczYbFY"
    "FDew165di/fff7/L3F5ako7n+S4JWUajUfY4586dq3hIyZ133okdO3ZE7RwsXrwYr776KhP3"
    "u8/nwxdffIHVq1dj7969kCQJer0egwYNwqRJk/DTn/4UmZmZzB7+ixcvxksvvRT0fXPmzAm7"
    "4gYrfD4fGhsb0djYCLvdDo/HA6/XC4fDAVEUO8ut2my2zjJywZ4fSovp+vp6fPbZZ2F/LjMz"
    "szNuVWk2bdqEkydPyr7uz/8IVD510KBBmDJlSkCPis8toHRtBbJHJzFLGv5uYQVSBlsxcUk+"
    "drxzHF5X1/4INrsXr35cijd/PR0mgzKe0p2H6mTFhUbPI29Keo/6KUiiBLfdh9NFNTi2qQoN"
    "5e2QRCmofTF16lS88cYbnaEMPcHlcuEnP/nJZc82tVoNk8nU5Rq51PusVqtl89ZibZPKX0nH"
    "brd3eT64XC64XC74fL5Oj7bP5+u2pHFGRgY++ugj5onADQ0NOHfuHDweD7KysiIq7xoqkydP"
    "Rnx8fMg5Q0uWLMEzzzzD/HlfUVGBmpoaaLVaDBs2jGkRnoULFyojBMaOHav4TtfKlSuDVvv4"
    "8MMPwfM87r33Xjz77LNMDBaO4zB27NigQkCr1WL+/PmKfu+3334Lm80W9D0vv/yyImUkg+Ev"
    "RWW329HW1gaLxXJZjLXVasWwYcMUfzDIhQXNnz9f8d2Rzz77THEDm0UZ2XCT13vCNddcg5df"
    "fpnJPVVaWooHHngA58+fv+zvHR0daGxsxL59+/Cvf/0Ln3/+ObPwlVDEc28s6g6HAwcPHsSB"
    "Awc6y79VVlaiqalJse9IT09X/Lnx5Zdf4oUXXgj7c7/4xS+YzKMkSfjtb3+LqqqqbkXh73//"
    "e/zkJz+57O9WqxV33nkn9uzZE9CrUnWkCSe2VmP4vAHgVOzj1IfMzERDeTtK11YGTKBdt7MK"
    "m/ddwC2zem4wub0CPlhzGi3tgZPMk3ItGDAisrAgn1tA/ek2nNvfgNPf1qC9vvvQV47jMH/+"
    "fLz33nsYMGCAIvO5bds2fPvtt4qeo7S0NEyYMCHqz4zz589j3759OH78eGfJzJqaGsV7JanV"
    "arzzzjvMREBTUxPefvttrFq1qou9lZWVhV/+8pe44447FP9elUqF/Px8FBcXd/veMWPGKF5K"
    "+lIOHDiAN954Azt27Lgs5FilUmHmzJl4/vnnmfToGjp0qDJCQGnDRxTFkEJjBEHA3/72NwwZ"
    "MgR33XUXk5PTXcOdGTNmKK7WuovVu+eeexRvcuTH6XTiH//4B/7+97+jtra22/dv375d0YdD"
    "fX297M5Ebydkh4JWq8W8efMUHTPSMrKRYLFY8Ne//pWJO3rPnj246667ul2kWltb8fvf/x7/"
    "+te/mPzG9PT0oK9rNBpcd911UZnv+vp6fPXVV/jqq69w8OBB5ueZRZJluNXN/Nx0001MfmNJ"
    "SUm3IsC/fvz+97/HkiVLLgvtVKlUuPXWW/Huu+9iz549XT7ndQkoWXkWaUPikDSQfblsjY7H"
    "NbcPRsXBRrTVdBUmze1u/GXZMVw/LRuaHnopjpW3Yu3OwBtfvFqFEQuyg4ofSZTg84oQ3AI8"
    "Th/cHV601TpQfbwFNcdaYGtwwt3hC6m2O8dxGDp0KD788ENF15hIr9fu7itWSbxXitw9e/Zg"
    "1apVWLdunSL9j0LhscceY7bxuGnTJvziF79AS0vgXJiqqir8+7//OwRBwA9/+EMmYqA79Ho9"
    "/vSnP/WoeZ2sQPb58Oyzz+KDDz4IeF+Iooht27bhtttuw/bt25GcnKz471dECCi9e3bw4MGw"
    "LvDPP/+cmRDoLsRDaeNUEATZkpR+I+bpp59m8lsbGxtx9913o7S0NGQlOXjwYMWNikBd8ljE"
    "bG/ZskXxnRMWvTRYdpe+kv/4j/9QbOftUmpqavDAAw+EPN87d+6EIAhhVfkJ58Hb3Tlk3Rvh"
    "/Pnz+POf/4zly5dHrUsyi+dVQ0NDSLtpVzJ48GBmccarVq0Ka+Pj7NmzXY4lLS0N9957b2fo"
    "Whfju7IDez4qw/ylo6AzaZifN1OiHmNuHIhv/3EiYEnR7QdrcfxcK0bnR16wwyeIeHvFCbg8"
    "QsDXkwdZMGDEd+MLXhEVJY2oLGmEx+GDJAGSIMLjEuB1+uBs88DR4obL7g270pJKpYLFYsFT"
    "Tz2FtLQ0Re/97sJuI4FFYYErBcDKlSvx5z//OWAdeJYMHjwYP//5z5mMvX//ftx3330hPQP/"
    "9re/MREC/tCpYDzyyCOKd4v287vf/a5LeHIg2trasGzZMjz88MOK//4eC4Fhw4Yp3qY+XMVe"
    "UVHB7CYI1rqZ53ksXLhQ0e/bt28fGhoaZF9/7rnnmMSLud1uLFmyJKyHDIukQ7mwoNmzZ3eJ"
    "4YxFA1vpOXE6nbIlSpVmzJgxuPvuu5mM/cwzz6CxsTHk93s8HtjtdiYGeUdHR1Q3Nq58nrzz"
    "zjt48cUXFReh3ZGQkNDZRFEp1q9fH1F7e1beAEmSwu6RIechWbJkCb7++musWrUqoBg4V1yP"
    "He8cx8x7h0NnZi8GBk/LwJnddbhQ2hzAiJfwty9O4rVfToGaj8zjs/NwPdYUBfYGqHgO+TMy"
    "OpOEfR4B2/92HCe2VAUUJpHCcRx0Oh2sVit0Oh1uvvlmRedw9+7dEfWQCEZiYiLT8pEVFRV4"
    "/PHHoxoeeinPPvssk51wm82GRx55JOSNkFOnTsHn8yleNbA772FGRgYeeughJnP75Zdf4t13"
    "3w35/ceOHVP8GCorK3teNYiFMRiuEGBxkfqRqxAAXCyZqnQFg2DG6ciRIxV/MPr54x//GPZO"
    "g9LnvqWlBbt3746Kceb1erFp0yZFx+R5XvFeGtu2bQupjKwS/Pa3v2WSF/Dtt9+GfU9zHMcs"
    "Wffs2bOyr7Hsh+J2u/Hggw/id7/7XdRFAMCm6WGkYprVc+zAgQOorq4O6zNyrnaLxYLHH39c"
    "NllRFCSc2lGDb/9xAo42D/PzZ0zQYeisTKhkDP0dJbUoq2yLaGyfIOKj9WdQ2xj4WaPRqmG2"
    "mNB2wQlXmxc1x1pxcusFxUQAx3HQarWIj49HQkIC1Go1k/WVRVjQwoULmZW03rdvH66//vpe"
    "EwGFhYW49tprmYz9n//5n90WYrkSQRAUPYba2tpuN4YeeOABJhWCbDZb2InHLEJHT58+3XMh"
    "oLSBduLECZw7dy6szyjpOrySYN3nlDaEJUkK+qB68sknmRhqp0+fxt///vewPpOdnY2RI0cq"
    "ehwbNmwIeKGr1WrFH0ZFRUUhuQTDoa8sXIGYOXMmpk+fzmTsV155JezPZGVlMVtcT506FdVz"
    "6H+A33vvvSGV6GOF0s8rm80WUdIly7CgcHpk+EWAnBDgOA4zZszA448/LhuiJvpEnNx6Ad/8"
    "5Qja69gKdo4DBk9Jlw1FKr/QjmPlrRGNXd3gwLKNZyHKxO5r1XocWV6D7a+dwa63zuHApxUQ"
    "fKIiv0uj0SAuLg6JiYkwGAyda5zStoUkSUy8wKw8iMXFxbjzzjsV92CEw7/9278xGbesrAyf"
    "fvpp2NeJ0tX49u/fH/R1i8XCJBwJAN588000N4dX+peFh3z//v09EwIDBw5EYWGhogcVbotr"
    "AMxKDQKQLUHHomTqkSNHZN1UBQUFiieh+vnzn/8ctnufRdKhnJE0bdo0xMfHx7yBrfT14PP5"
    "FPdaRPuBX1JSEjDhsjtY1qkO1iSQVazv73//e9kGX9GARdPDTZs2RZTfwCosSBTFsMOCRowY"
    "EfR1lUqF++67D0uXLpVNoBcFCef21WPFf+zBsU1V8LkEZudRb9Vg6KzAFfqcbgEbdlfJGvPy"
    "BjLw1hcn0G4P7NVQq9WdO6Jeh4CW8w44GyLfmeQ4DhqNBiaTCcnJyUhJSYHRaLwsaZPF+nrw"
    "4MGQCmCEA4uS1sDFDch77723VzyHftLS0pgVJfnTn/4U9u4+izKi+/btC/r6XXfdxSQUu729"
    "He+8807Yn1O6XL4kSSguLu6ZEIiFsCAAzGrpt7e3yxrmY8aMUVyABPvtP/vZz5h4A6qqqvDl"
    "l1/2+rm32WyyZTyV3nERRTFoQnaki9uiRYsUHZNFPKucyJwzZw6TscP1NPmZOHEik+Nxu92y"
    "QkClUjF5phUVFUU8D0rBordFpGKaVVjQ/v37g3pwAzFjxoxu32MwGPD0009jyZIlQZPXbQ1O"
    "7HjnGDa+dghVh5vgdbKpAJU1JhlqbeDj2LSvGkKY4TpV9XZsKa4J+vuv/N0ajQZGo7HbNYnj"
    "uItVSdRq6HQ6WCwWJCYmIjExEVarVVZcjRkzRvGS5Cw2f6677jomoclPPvlkWDlVLPjZz37G"
    "5Le1tLRE5BlVOgKhOyGgVqtx7733MpnbFStWXFYiNFSU7lxdVlaG1tbWniULK22gnT9/PqJk"
    "iGuuuYbJyTpw4IBsmTOljb5gDyqr1Yrbb7+dyW/89NNPw447S0lJUdxQ27x5MzweT0DjTOmE"
    "7P379wfN/YiE0aNHK15tJ1phQT/+8Y+ZiMz29vaIPHzARS8Qq3s60HUGAOPHj1c8zNDn8+HX"
    "v/51SOUSWaK0wHG5XBElscdSWBCAkL2sycnJeOmll2Cz2fDVV1/J7mZ6XQLO7K5D5aEm5IxP"
    "Rv70DOROSIFap1z1q/gME6zpRjRXdO01c76mA3XNTmSlhl5Y4Wh5C05XtcuK40vDdS418P0J"
    "vW63G6Iodr7Hb/zzPN/5v/5/vWVbsHqesjjOr7/+Gt98802vPi/0ej1+/OMfMxl7+fLlss/g"
    "YCjtebHZbEErJC5atIhJBT0A+Oijj8L+jFarVTwp3Z+TGbEQSE9Px/jx4xU9qEiMBr1ez8wj"
    "EGyhU/oBcOrUKdmuhTfeeONlda6VQpIkfP7552F/buHChYrXTJY79xMnToxaw7JYMrQkScL6"
    "9euZP/DVajWzUI2VK1dG5NpOTk5WfOfDT7AFlkVY0Keffho0OTkasGh6uG3btoh2tFh5AyIJ"
    "C8rIyEBBQUHI709LS8M///lPPP/883jjjTeCXtsepw+ni2pxZmcdDHFaZI1KRNboJMRlmGBJ"
    "NcBg1UKj44FL7WvpYjUeR6sbjhY3HG0e8BoVUvKsMMZ/580xp+iRMCCwEACAgyebQhYCoiih"
    "6FCdbAMxnU4nm6vjFwksEimVfp6eOHFC8fvQYDAo7kkVBKHbrufRYOHChYiLi2My9rJlyyL6"
    "nNI5bLt27Qq6CbpkyRImv//IkSMhl2i/lGuuuUbxe82f46XuyYWi9C5iJIk811xzDbOqQdu3"
    "bw/4dxb184P99u9///tMfl9RUVGXDq+98ZB2uVyy8dMsPC8sDGyl5+TQoUNhhzlEwuzZs5GS"
    "khJTD/z58+cz6R8AXPQ8BYJFaJckSXjzzTd7fVFn0fQw0qRLVqJz3759YTdXmjt3bthrmNls"
    "xnPPPYeMjAy89tpr3ZYelCQJjlY3Tu2owemdtdCaNNCbNdDoeWiNamgM6s5jcNu98LkuNuLy"
    "OHzwOH1Q8RwScyyYdGc+ssckQxIleGwCeJX80n2svAU3z8wJ6ff4BAnfBAkLYrEB1R3Dhg1D"
    "Xl5eTFyvwZg3b57ihtmGDRtkNwSjCasIhJKSkoiiPgYPHqx4fHxRUZHsa6mpqZg1axaTOfjk"
    "k08i+pzSHhFRFDvnIGIhoPSOeLCOssFgVdqqpqZGNlE4mrkRGRkZitf+9hOJK91qtSquzLdu"
    "3QqHwxGVuT569GhE4icYQ4YMQX5+flSuB6VZvHgxk3ErKytx8ODBiDcZWFBRUSF7T48aNQo5"
    "OTmKfl9RUVGvewNY3EORNmWKtbCgSHdz/WVFJ0yYgN/85jfYu3dvSMUWREGCq90DV3t4YRG1"
    "J1qw/W/HcO0jY9Bwyo6aQ+1oOO+QfX95dUfIY7fa3DhwInAsOs/zzCp3RfN6ZfU8ZeFB/PDD"
    "D3v9ecHSCK6rq8OPfvSjsD+ndPQJgKAVz2677TZm177JZIpoDpS2uY8ePdqZgxjRL2XRmGbd"
    "unURNaZhJQSChQUp/aCqqqrCkSNHZC9IFq3LBUGIaGecRXKU3EN61KhRiu8C9IVqQf77gTVm"
    "s5lZzfzVq1dHFBdvMBiYLUJy3gBW5/Cf//xnry/qLHpb7NmzBy0tLWF/jmW1oHCTD9VqdY+u"
    "M47jMGfOHGzYsAHvvfce3njjDZSVlUW0hoVksFfbse31MmjUGkC6GJbDcVzgrsdtLoiiBJWq"
    "e29H8fFGeGXKgGo0GmaeuWgaPBUVFRGFYgRDp9MpXsWvoqJCNgohmtx6663MjOCFCxcy2+gJ"
    "h/r6etlNIQD4wQ9+wOy7n376acQClxZnicjCZNGYJhIDbfDgwYp3Ne5OCGRnZysev7x27VpZ"
    "o4lVWFB3HYzlUDp8wuv1ylbw6SsJY0rPSVlZGU6fPs38QXD99dczie/1C4FImDVrFrNjCiYE"
    "lL7WGhoaopLj0R2TJk2SrZMf7XuIVX7Anj17wk7+nzlzpiLhUhaLBQ8//DBWrVqF3/72txgy"
    "ZAiTjZuLigeA9J0QkcPhFuDyhFaaUc4b4BdLzH6LDCxKkrO4D2fNmqV4uN1HH33ETEiGAysv"
    "cSzxzTffyNpcw4YNU/wajNU56JEQUHrRbGtrw86dOyMSJCzwer2ypSxZxKzL7f4WFBQwuyAj"
    "SZg1GAyYO3euoscRrLGX0nN97ty5sLsnd8eAAQMUT1aPVljQ9773PSbjVlZWBq3VHwxWu0V2"
    "u132GVNQUKB4aNfnn38eUY39WBepkTZlirWwICW9EzzPY+jQofjd736Hr7/+GgkJCdDpdIrm"
    "0F2ZtBvMQPd6RXi83RuUPkHEobLADY04juuVsCAWmz8sikMofZyCIITdYIsFubm5zAo1xKoR"
    "HK0Ni1iivb39stKpYd/pJpNJcde9XEfZ7mDV7GLz5s1oawvcql3puMCGhgbZWrashI5/zsNl"
    "7ty5iu/Wyj2k8/LyMHTo0D6xICidNB8NIWCxWJh1Ev76668jCgvieZ5ZqN/atWvhdrujck8D"
    "Fysm9TYsmjKVlJRElMTOanEVBCHs+1qtVjPZ0OE4DkePHoVOp4NWq4UgCPB4PHC73fD5fBBF"
    "EaIohnxv+Etv6nQ6mEymy54zwcbgeQ483/0zqa7ZiQsN8rkGrIpwRNPAbmho6LZ7bLio1Wom"
    "4XZKl7SOhY2DWMTtdmPbtm1X9Rxs2rTpMps7bCEwf/78mGhMM3DgQGZlQ+VKaqakpGDChAmK"
    "ftf69etla1KzavJ05MgRVFZW9vpDIlieAot44r4QFlRbWyubL6Ik8+bNY7bjF0mzGACYMGEC"
    "kpKSmBzT8uXLo2Z8VFRUROUcdkcs9bZgJQR27doVdojjrFmzFO9U7mfVqlWdokCtVnd25fWL"
    "AFEUu4gCv1GvUqk6//nr7vv/+0qCCQE1z0Gj7t7ZX9/sQnObW1bURNsjkJ6ejnHjxik65rp1"
    "68LuYNsd06dPV/z6icSrxQKWm4+xwrZt29DRETihPisrC8OGDev3c3ClVzfsO11pw8fhcESU"
    "IHPbbbcxmaC2tjZs2rQp4Gss6ufLudnNZjOz7qqRxEyq1WrFd2v37dsn2z1R6Z3Muro6lJSU"
    "KDpmUlKS4udozZo1UWk+xWrnvbq6OuJqQawWoZqaGtlScbm5uYqHrESaKB3rIjXY8yoYeXl5"
    "zBbXSHJRWCUt22y2gLllHMdd1lBLiY20YLHkWg0PDd/9OlXX7ERTW+BeCDzPM2ky2N31qvR3"
    "9oVS0YIgRKU4RHfEx8cza876xRdfxETOFICgScIshdCTTz4pG2kSba4MjQpLCLBoTLN161Y4"
    "nc6wP8cqLOirr76S7Xqn9APAZrPJlrCaMWMGsx2ZSG7I6dOnK95gRM6ln5WVpXicYqRVqYKx"
    "cOFCxatqRGNBUKvVile88LNhw4aIjWBWFYxWrFghe+5ZxCRHmigd6wbLqVOncObMmZgxvH0+"
    "X9geCo1Gw8z1v2HDBtnwM6URBEH2PstONYUUGlRZZ0dbh1d2nqKN0vdisPU1UnieV/z62bt3"
    "b0yEBc2ePZuZzfHBBx/IhkDHEkrbt37Onz+Pf/3rXzH7u8Pa3p41axbMZrOiBxCJq3n8+PFh"
    "dYQMB7mwIKvVihkzZij6XRs3bpRNKGQVFlRbWxtRQw8WnXPlzv3111/fJ+LulZ6TlpYW7Nmz"
    "h/lNP3HiRGahEcHK7gZj2LBhiifs+ol2WNDhw4d7/cGen5+PIUOGxIRIZRUWtHv3blmPohwz"
    "Z85k1jHVHxYUDYLl1OVldV/NRhQlnKpogygjJqIdFpSQkIDJkycrOuamTZsUT9ifOHGi4g0Y"
    "YyUsiFXOWHt7e8Re4miiVqsxadKkmFoXY1IIKG34eL1e2TCcYNxzzz3MVFuwxF2lH47BFlal"
    "q/P4iWSHRKVSKV7N5dChQ6iuro7Kddba2opdu3YpOqbFYlFcGF6ZwMMKltW2Iqn+xdJYPHLk"
    "iKwrODMzE2PHjlVccMZCWFCslN7Ny8tjVvksEgOK1XXW1tYWNAFRSURRDBr3PiS7e6EjShKO"
    "nmkJahRFkwULFsRESfJo20CiKEatSlxvCYEdO3ZEZV3rKePGjYPJZGIy9pYtW/qHEGCRKR+s"
    "dKQc8fHxzB7m//jHP2QXcaUfAC6XS/biGDBgALKyspjdlOEyYcIEpKamKnoccmFBKSkpiscp"
    "sjCw582bB61WG/MLVzQf+Pv27YPdbo9IaLKqXR2snTuLik8sKlNFgtLhC8GaHgaDZVhQuHOt"
    "0WiYladdt25d1MrF+hOOA8GrOIwZmtjtGIIo4Wh5YCFwaT5DXxWuLpdL8V1YjuMUP879+/fH"
    "RFhQRkYGcnNzmYwd6eZQtJk6dSqz+3X37t39QwhMnjwZiYmJin55JIbPPffcA71er/hEtLe3"
    "4+OPPw74msFgUDxUZ+vWrXA4ApduGzNmDLMTHslNGc3azizi7vvCzpDD4YjKjqLVamVWzz3S"
    "sKZZs2Yp3kEauOgJWrZsWdTOYV1dHQ4cONDrD3UWvS3WrVsXkaeD1abNzp070dzcHNZnWIYF"
    "RTO8QxAEWY/AoEwL0hK6L/Hc3O5BTZNTVphHM1HYbDYrXpJ8+/btEW1KBGPMmDHIzMzslxsH"
    "rCowAoi4p0x/mYOzZ8/CZrP1DyHAwiUWbi37+Ph4PPTQQ0wm4r333pMtKcWifn4w45RVQ4/m"
    "5mbZcJxgKL27ePz4cZw9ezbga0rvILIwsFm0l9+6dStcLhfzG37cuHHMdvsijQN94IEHmBzP"
    "+++/Lyu2U1JSFK/4xCIhPVKRGgs5NldLWFBLS0tEntZI8Xq9sqJs+pg0cCGs6kfPNEMUA4/h"
    "L1saLa677jrFvassDGwW/UZioVoQAIwcOZLJuD6fL6KcxP40B31BCIV0t7NoTFNcXBy2S+zn"
    "P/85rFYrkwf5m2++GTVD2OfzYePGjbKv5+TkMDnZkXTVHTlypOLHI2dUpKWlYdq0aYp+16ZN"
    "myKqShWMWbNmKR5LGK0FgVWSPXCxc3O4TJ48GbNnz1b8WGw2G/7+97/Lvs7C89Rfw4IaGxsj"
    "qvgRa9WCWIUFrVmzJqox0HKViVQqDpNGpEAVgggsDZIfEG0h8P3vf1/R8TweD5NSlUp7xo8c"
    "OYKKioqYeGawWheqqqqiVkmrJ5hMJmbh2JFUWotJITBu3Dikp6dHxRiUY+LEibjvvvuYTMLr"
    "r78um6ug0WgUT67cuXNn0HqyrC7IqqqqsD+jtAAMZjDddtttihtnLDq8Kj0nPp8voqT5SFC6"
    "ksylhNvYSa1W47nnnmNyLH/961/R1NQk+7rSu3stLS0xEQealJSkeOWLDRs2RNSUidUO/Lff"
    "fouWlpawxXt/CAsSRVE2FyEpToeCgd3/RgnAsfLWoPdltEhJSVE8LGjr1q1h5x52R2FhoeIx"
    "9LGycQBc3IRjQWtrK/oCKSkpzMLh+sIchCQEWBiD4eyAJicn46233mLygDp06BDeffdd2den"
    "T5+uuBeiuweAxWJhcrIj2bVS+tyfPXsWx48fj4rhYLPZFM/WZ5E0350wVNpQZEW4uTuPPvoo"
    "k3yYCxcu4H//939lX4+Pj1fc87Rhw4aYqIyxYMGCmMixuVrCghobG6OaDOnxeGTDggZnWTE0"
    "BCHQ3OZGWWVgQznaHYUXLlyo+PexKOPKIk8uVsKC/IYwC6KVQB+rvz9SuysmhYDSN0FpaWnI"
    "LrGhQ4fiyy+/VNwjAVx0sT7++ONBTxSL3Iju3JasYrjD3RHLy8tT3GUoJ4JSU1MVNwrXr1+v"
    "uFty8uTJSEhI6HVDK1JYlUcDwvNk3XrrrXj88ccVPwZJkvD4448HDQdjUaowVnb3YqUpE8uw"
    "oHANKNZhQZF4S3qyZskJgTH5iUhP7D6XraK2A9WNDlkhEM1mYkqfF7fbHXbuYW/cV2VlZTh1"
    "6hT6OwUFBVHvUB2RIcwwFI5VV/WoCoFhw4Yp7hILxfBJTEzE0qVL8dVXX2HQoEFMfvyTTz4Z"
    "NG6eRf38UMqFKe3W9DNnzpywXIDRDAtasGCB4jcji3Cb3hCGSsLSTfnDH/4wpPfdddddeP31"
    "15kI3nfeeadbw1XpRb2jowPbt2/v9Yc5i94W33zzjWyn9WCw2oHfvn172NfwrFmzmOSWAdFt"
    "IiYIQtAd1nkTM6FShdZRuLYpsBCIZn6A2WxW/HotKipSvFpQfn6+4htisdI7wI/SeXR+rFYr"
    "brnllpg3hOWKSijBjTfeqPjmodJ0uy3GwiUmiiLmzp0Ln88Hr9cLjUYDg8EAs9mMgoICjBo1"
    "CpMnT2ZSJtTPa6+9FrTjKMCmi2AoD4Dy8nKMHz9e8d9sMpmwYsUKvP3229ixYwcaGhouK2tl"
    "sVjA8zzi4uJgsVgUX8yrq6tlM+hZNLkqLi5WdDyO4xRPxCwpKUFdXV3Ubvjy8nKmQqC6uhpv"
    "vfVWl4VFpVJh9OjReOKJJ5h1zd6yZQv+67/+q1vjQ+mY5M2bN0dkLCtNrPS2uFrCgurr66PS"
    "CdyPf70M+Gw3qDFvYvelLSUJOHiyCXZnYC+40tdPdwJN6e/bv3+/4scZK835WHLmzBlmO9ev"
    "vfYaxo0bh08++QSnT5+OyVCZc+fOQRAEJptTqampWLt2LV599VVs27YNtbW1JAQA4Ne//nWv"
    "/ugXX3wRr7/+erfvY7EjHsoDYMuWLbj99tuZ/PaBAwfihRde6JV5l+u6ynGc4qUcgYveh/r6"
    "+rBKOtrtdixZsiSgu3/MmDHIyMjo0wvC+vXrsXTpUiZjcxyHX/3qV3j44Ydx9uzZzjnUarXI"
    "ysqC2Wxm9ruKi4vx0EMPdbvIzJ8/HzqdTvHrLBZQ+nnldrsjyrFhFRbk9XrD9p6xDAtavXp1"
    "VMvFulwu2bCgaydmwmrqPqRHlCRsP1gj+3o0hYDSSe0A8Mgjj+DWW28NuxTzH//4R1mvntL3"
    "VWVlZUTN+ViyZ88eJuVR/dfU/fffj/vvvx8ej6dzAzJamyf3339/t4VSbDYbjh49yqyXQHZ2"
    "Nl555RUAFyM+Wlpa0N7eHpUu9CdPnsQvfvGLyIVAbm4us+ZDvYHNZsNTTz2FL7/8MiSjRund"
    "36NHj4aUG7FhwwbU1tYyyYvoTeQMpry8PCYVPVJTU8PuiPzVV1/Jxvz2dtK8EpSUlGDdunWK"
    "X9uXYjQaMWLEiKgKzEcffTSkxV/pxS5Yh/BowqK3xY4dO2R7qwSDZVhQuEn1s2fP7hdhQZIk"
    "yV7fal6FG6Zngw8hLKjD4cXeo4Gre/E8H9VE4XHjxjF59gwdOjRsgVlSUhLwtZycHMX7+kTa"
    "nI8ly5Ytw+OPP474+Him36PVajFgwICo/a7KysqQqyW+8847IW0Q9xSr1crsmRSIzZs3d/ue"
    "oMGALAyf3mLz5s1YsGBBSCIAuFg/X+lup6HuHDocDjz55JNRTUJjTbBa5KzCCCJh9erVsq8p"
    "7R07efIk01AdOZ588smY25GKBLvdjqeffhr3339/SCJAr9dj7ty5ih7Dtm3bFI9JjgQWvS1i"
    "LSwoEsOblSiprq5WPPQwGG63W9b7kJ9txZRRoW14fFNcDac78LqiVquZFaoIRDQ3C7oTvHJ5"
    "eSya88VS2VA/NpsNzzzzTMwJlJ4STuL4ihUr8M0336C/EcocBBUCLMKCookoitiyZQu+//3v"
    "45577gmreUdvlwvbvHkzHn74YWZJPNFm/fr1ssJG6VCNSHE6nbIPgoKCAuTl5fXa9aAkTU1N"
    "WLx4Md5///0+UdoskFH0/vvvY86cOXj//fdDXrzmzp0Lo9HYLxd1pTdtBEGIqPoKq7CgSJpE"
    "abVaxUv9XrphEC2jye8NkK0WNCQR+Vnd7zCKooQtxfJhQRqNJmqJwtGuThSMr7/+Omp2QH19"
    "fVQFZDh88cUXeOKJJ/qNzRGuEBBFEQ888EDM5W/0hJqampA2/WTv+vT0dCauO9YIgoDi4mK8"
    "+OKLmDJlCn70ox9FlNCldOhEeXl52J19V69ejTlz5mDlypV9ph5vJAbTgQMHohprK8eWLVtk"
    "qwewCKXpzTrS/t302bNn44033kBlZWXMX0MlJSV4/vnnMWXKFDz99NOorq7uVWO5uw7h0YJF"
    "b4u9e/eiubk5ZoTAtm3bLitsEAr9pVqQz+cLWgb5B9cOgk7b/U5+daMDxccbZQ3zaOYHSJLE"
    "JLE3krmVE5hpaWmKF+1Yv359TKx1cnz88ceYP38+li9f3ic6Agejvb097CaPTqcT9913H+6/"
    "/34cOHCgXwihUDYs1MEM4Viv/2qz2VBVVYXy8nKUlpbiyJEj2L9/f4/Lb+bn54cdZ9gdkarM"
    "qqoqPPzww0hOTsZNN92E6dOnY/LkyUwbQ7G4IYuKioKKpH//93/Hs88+26u/K9jOkNJGZFVV"
    "VUyE55w7dw4vvPAC/vjHPyI/Px/jx4/H+PHjkZ+fj9zcXKSlpfXKc6Curg5lZWU4ceIE9u7d"
    "i71794bdufhSWHQILyoqilojuGDESm+LQYMGMQv3iKVqQZWVlbIx5SxwOp2y3tTBAyxYOCW0"
    "mOuSk004ca5VVghE2zP7i1/8An/5y18wYcKEXrt3ioqKZLtUX3/99Yp7SIKtMbHC+fPn8dhj"
    "j+HZZ5/FvHnzMHXqVEyYMAF5eXlRFYs9ZfPmzRF7vNesWYM1a9agoKAAs2bNwvTp01FYWBjV"
    "/AalhEAoqKNl+LS2tuKPf/wjgItlKlUqFQwGA7RaLTQaDYxGIzo6OiAIAiRJ6jTmvV4vHA4H"
    "BEFAS0tL57+6ujpmi/Do0aNx+PDhXl/ILqWxsRHvvfce3nvvPQAXO8Tm5+cjOzsbSUlJSEtL"
    "g8lkgslkgkajAc/znVVazGbzZbGfDodD1sPAYk5LSkq69WgsX74cX375JcaMGYNBgwYhMzMT"
    "aWlpUKvVMBqNUXkAyfUdSEhIgCRJil4TchWUegtJklBWVoaysjJ8+umnnX/X6XTIyMhAYmJi"
    "5z+j0QiTyQSO4zp3XC+9p3U6HZKSkqDVajs9LC6Xq3OHyeFwwOPxwGazobGxEc3NzZf9q6+v"
    "V7yuc2FhIc6dO6fomMuWLYuJczd8+HDFn1eReKtyc3OD5thEY0G7FI/Hw+R4tm/fHrV7VxRF"
    "2VANlYrDkgV50Gn4EO5vYNO+arTbAz+HdTpd1AV/RUUFbrnlFgwcOLDTyMrMzOxcw1g2P7x0"
    "3ZEjLy9P0fvK5XJh165dfcaIbGtrw4oVK7BixQoAF5PJs7KykJGRgbi4OMTHx3f+r/+//cnm"
    "Vxb/MJlMUU1EBxByPmgwTp48iZMnT3Z2qjcajRg4cCBSUlIQFxeHuLg4JCQkdP63fz3027SX"
    "Cu1oJgj7bedgG7CXbQRkZmZKgQyfkpISRU/cp59+yqSTKEEQBEH0RxwOB9ra2gIKjwEpRnz0"
    "X3MxfUz3TSJ9PhFDFn+Gyjq77GaHwWCgCSeIq5CAfq+FCxcqrt76UwIGQRAEQbBEEAQ4HA5Z"
    "78PsCRmYMDw5pLG+Ka6RFQFqtTpmEncJgogRIaB0WJDdbpdt1kEQBEEQxOW43W7ZkEo1z+G+"
    "WwqgDyFJ2OUR8PEG+TLFWq02qmVDCYKIcSFgMpkwc+ZMRb9k8+bNfT4DnSAIgiCigSRJsNvt"
    "st6AaaPTMDnE3gHHyltQdKg24Gscx8FgMMR8YRCCIKIoBK699lrFqwdQWBBBEARBhIbT6ZSt"
    "eGIxavBv3x8GrSa0ijZrd1XhXHXgDtE8z/epSjAEQURBCCgdFuTxeLBlyxaaaYIgCILohu5y"
    "AyYWpmDuNZkIZQ/f4xXx8bozEGXGIm8AQRCXCQGtVou5c+cq+gXbt28PuxEMQRAEQVyNOJ1O"
    "eDwe2dd/vqQQqQn6kMZau7MSJ84HLgnN83zMdHUnCCJGhMCcOXM6a88rBYUFEQRBEET3CIIA"
    "u90u+/q8iZm4dlJmSGM1trrw9gr5bvY6nY6qBREEcbkQUDosSBAEbNy4kWaZIAiCIILgTxCW"
    "6yIcb9Hi/tsKoNeGVtq76FAd9pQG7sZNScIEQXQRAmq1Gtdee62ig+/evRtNTU00ywRBEAQR"
    "BI/HE7Sj9oLJWbh+ahZCtd3f+Ow42joChxhpNBoKCyII4nIhMHXqVCQmJio6OIUFEQRBEERw"
    "RFGEzWaDKIoBX4+3aPGbn46B2RhaKM+3JbXYflC+ZKjJZKJJJwjiciGgdFiQJElYv349zTBB"
    "EARBBFkrHQ6HbPMwXsXh/908FMMHxYU0Xrvdi7e+OAGfEFhUaDQaKhlKEMTlQoDjOCxYsEDR"
    "gUtKSlBdXU0zTBAEQRAyeL3eoM3DRg9JxEOLh0PNh9Y3YMOeKqwpqpR93Wg0UidhgiAuFwLj"
    "x49HRkaGogNTWBBBEARByCOKItrb22UThLUaFX79k9EYlGkJaTyXW8DLHx5Buz2wd0Gj0cBg"
    "MNDEEwRxuRBQOiwIANatW0ezSxAEQRAB8FcJkgsJ4jjgttkDceP0nJAShCUJ+GLrORw40SQz"
    "3sXcAKoURBDEpagB4IYbblB00BMnTuDMmTM0uwRBEAQRAI/HA7fbDbU6cDnQQZkWLFkwBKer"
    "OkIa70KDAy//6xh4Xo1AkT9arRZ6vZ4mniCIy4VAYWEhBg4cqOiga9asoZklCIIgCBl0Ol3Q"
    "Ep4dXuDnr5WGOaoGKSkpNLkEQYSMikVYEOUHEARBEARBEESMCwGlw4IqKytx7NgxmlmCIAiC"
    "IAiCiGH+fyQR/MLhD7NWAAAAAElFTkSuQmCC"
    ;

/* ═══════════════════════════════════════════════════════════
 *  HTML writer[cite: 1]
 * ═════════════════════════════════════════════════════════*/

static void emit_html(Ctx *ctx, const char *main_dts_path, const char *out_path)
{
    FILE *f = fopen(out_path, "w");
    if (!f) die("cannot open output file for writing");

    int total_nodes = ctx->root ? count_nodes(ctx->root) : 0;

    /* ── Head ──[cite: 1] */
    fputs("<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n"
          "<meta charset=\"UTF-8\">\n"
          "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
          "<title>DT Viz \xe2\x80\x94 ", f);
    html_esc(f, main_dts_path);
    fprintf(f, "</title>\n<style>\n%s%s</style>\n</head>\n<body>\n", CSS, DIAGRAM_CSS);

    /* ── Sidebar ──[cite: 1] */
    fputs("<div id=\"side\">\n", f);

    fputs("<div id=\"side-top\">", f);
    fprintf(f, "<img id=\"logo\" src=\"data:image/png;base64,%s\" alt=\"platform|tree\">",
            LOGO_B64);
    fputs("<span>", f);
    html_esc(f, main_dts_path);
    fputs("</span></div>\n", f);

    fputs("<div id=\"sb\">"
          "<input id=\"search\" type=\"text\" autocomplete=\"off\" "
          "placeholder=\"Search nodes, labels, properties\xe2\x80\xa6\" "
          "oninput=\"doSearch()\">"
          "</div>\n"
          "<div id=\"mc\"></div>\n", f);

    fputs("<div id=\"ctrl\">"
          "<button onclick=\"expandAll()\">Expand All</button>"
          "<button onclick=\"collapseAll()\">Collapse</button>"
          "<button onclick=\"clearFilter()\">Reset</button>"
          "</div>\n", f);

    fprintf(f,
        "<div id=\"stats\">\n"
        "  <div class=\"st\"><span>Nodes</span><b>%d</b></div>\n"
        "  <div class=\"st\"><span>Properties</span><b>%d</b></div>\n"
        "  <div class=\"st\"><span>Source files</span><b>%d</b></div>\n"
        "  <div class=\"st\"><span>Labels</span><b>%d</b></div>\n"
        "</div>\n",
        total_nodes, ctx->total_props, ctx->nfiles, ctx->nlabels);

    fputs("<div id=\"leg\"><h3>Source Files</h3>\n", f);
    for (int i = 0; i < ctx->nfiles; i++) {
        const char *col = COLORS[i % NCOLORS];
        fprintf(f,
            "<div class=\"fi\" data-file=\"%d\" onclick=\"filterFile(%d)\" title=\"",
            i, i);
        html_esc(f, ctx->fpaths[i]);
        fprintf(f, "\">"
                   "<div class=\"d\" style=\"background:%s\"></div>"
                   "<div class=\"fn\">", col);
        html_esc(f, ctx->fnames[i]);
        fprintf(f, "</div><div class=\"fk\" id=\"fk%d\"></div></div>\n", i);
    }
    fputs("</div>\n", f); /* leg[cite: 1] */
    fputs("<div id=\"attribution\">Tool constructed by "
          "<span><a href=\"https://mozcelikors.com\" target=\"_blank\">@mozcelikors</a></span> using AI</div>\n", f);
    fputs("</div>\n", f); /* side[cite: 1] */

    /* ── Main area ──[cite: 1] */
    fputs("<div id=\"main\">\n", f);
    fputs("<div id=\"view-bar\">\n"
          "<span id=\"view-bar-label\">View</span>\n"
          "<button class=\"vbtn vact\" id=\"btn-tree\" onclick=\"switchView('tree')\">"
          "&#x22EE; Tree</button>\n"
          "<button class=\"vbtn\" id=\"btn-diagram\" onclick=\"switchView('diagram')\">"
          "&#x2B21; Diagram</button>\n"
          "</div>\n", f);
    fputs("<div id=\"main-content\">\n"
          "<div id=\"nr\">No matching nodes found.</div>\n"
          "<div id=\"tree\">\n", f);

    if (ctx->root)
        emit_node(f, ctx, ctx->root);
    else
        fputs("<p style=\"color:#8b949e;padding:20px\">No root node found.</p>\n", f);

    fputs("</div>\n", f); /* tree */
    /* ── Diagram view ── */
    fputs("<div id=\"diagram-view\">\n"
          "<div id=\"dg-ctrl\">\n"
          "<span>Depth:</span>\n"
          "<button class=\"ddb dda\" data-d=\"3\">3</button>\n"
          "<button class=\"ddb\" data-d=\"4\">4</button>\n"
          "<button class=\"ddb\" data-d=\"5\">5</button>\n"
          "<button class=\"ddb\" data-d=\"999\">All</button>\n"
          "<button id=\"dg-fit\">&#x229E; Fit</button>\n"
          "</div>\n"
          "<svg id=\"diagram-svg\"><defs></defs>"
          "<g id=\"diagram-g\"></g></svg>\n"
          "<div id=\"dg-hint\">Scroll to zoom Â· Drag to pan Â· Click node to expand/collapse</div>\n"
          "</div>\n", f); /* diagram-view */
    fputs("</div>\n", f); /* main-content */
    fputs("</div>\n", f); /* main */

    /* ── Doc popup modal (single instance, populated by JS) ── */
    fputs("<div id=\"doc-modal\" onclick=\"if(event.target===this)closeDocPopup()\">\n"
          "  <div id=\"doc-modal-box\">\n"
          "    <div id=\"doc-modal-hdr\">\n"
          "      <span id=\"doc-modal-icon\">&#x1F4D6;</span>\n"
          "      <span id=\"doc-modal-title\"></span>\n"
          "      <button id=\"doc-modal-close\" onclick=\"closeDocPopup()\">&#x2715; Close</button>\n"
          "    </div>\n"
          "    <pre id=\"doc-modal-pre\"></pre>\n"
          "  </div>\n"
          "</div>\n", f);

    /* ── DT_NODES data + diagram init script ── */
    fputs("<script>\nvar DT_NODES=[\n", f);
    if (ctx->root) {
        int first = 1;
        emit_node_json(f, ctx, ctx->root, &first);
    }
    fputs("\n];\n", f);
    fprintf(f, "%s\n", DIAGRAM_JS);
    fputs("</script>\n", f);

    /* ── Main JS ── */
    fprintf(f, "<script>\n%s\n", JS);
    /* ── View switch function ── */
    fputs("function switchView(v){\n"
          "  var tree=document.getElementById('tree');\n"
          "  var diag=document.getElementById('diagram-view');\n"
          "  var btnT=document.getElementById('btn-tree');\n"
          "  var btnD=document.getElementById('btn-diagram');\n"
          "  if(v==='tree'){\n"
          "    tree.style.display='block'; diag.style.display='none';\n"
          "    btnT.classList.add('vact'); btnD.classList.remove('vact');\n"
          "  } else {\n"
          "    tree.style.display='none'; diag.style.display='block';\n"
          "    btnT.classList.remove('vact'); btnD.classList.add('vact');\n"
          "    if(!DTDiagram._ready){ DTDiagram.init(); DTDiagram._ready=true; }\n"
          "    else { DTDiagram.fit(); }\n"
          "  }\n"
          "}\n"
          "</script>\n", f);
    fputs("</body>\n</html>\n", f);

    fclose(f);
}

/* ═══════════════════════════════════════════════════════════
 *  Entry point[cite: 1]
 * ═════════════════════════════════════════════════════════*/

int main(int argc, char *argv[])
{
    if (argc < 3) {
        fprintf(stderr,
            "Device Tree Source Visualizer\n"
            "Usage:  %s <dts-folder> <main.dts> [dt-doc-folder]\n\n"
            "  dts-folder      directory that contains the .dts/.dtsi files\n"
            "  main.dts        top-level device tree source file to parse\n"
            "  dt-doc-folder   (optional) kernel DT bindings documentation folder;\n"
            "                  when supplied, driver descriptions are shown inline\n"
            "                  for nodes that have a 'compatible' property\n\n"
            "Output: devicetree_viz.html (written to current directory)\n",
            argv[0]);
        return 1;
    }

    const char *dts_dir  = argv[1];
    const char *main_dts = argv[2];
    const char *doc_dir  = (argc >= 4) ? argv[3] : NULL;

    Ctx ctx;
    memset(&ctx, 0, sizeof(ctx));

    /* Base directory for resolving relative includes[cite: 1] */
    strncpy(ctx.base_dir, dts_dir, MAX_PATH - 1);
    {
        int l = (int)strlen(ctx.base_dir);
        while (l > 0 && (ctx.base_dir[l-1]=='/' || ctx.base_dir[l-1]=='\\'))
            ctx.base_dir[--l] = '\0';
    }

    /* Optional DT binding documentation folder */
    if (doc_dir && doc_dir[0]) {
        strncpy(ctx.doc_dir, doc_dir, MAX_PATH - 1);
        int l = (int)strlen(ctx.doc_dir);
        while (l > 0 && (ctx.doc_dir[l-1]=='/' || ctx.doc_dir[l-1]=='\\'))
            ctx.doc_dir[--l] = '\0';
        printf("      DT doc folder: %s\n", ctx.doc_dir);
    }

    /* 1. Preprocess ─ resolve all includes, strip comments[cite: 1] */
    printf("[1/3] Preprocessing  %s\n", main_dts);
    SBuf pp; sb_init(&pp);
    preprocess(&ctx, main_dts, &pp, 0);
    printf("      %d source file(s)  |  %zu bytes after preprocessing\n",
           ctx.nfiles, pp.len);

    /* 2. Parse ─ build the node tree[cite: 1] */
    printf("[2/3] Parsing device tree ...\n");
    Parser ps = { pp.buf, pp.buf + pp.len, 0, &ctx };
    parse_toplevel(&ps);

    if (!ctx.root) {
        fprintf(stderr, "Error: no root node ('/ { }') found in %s\n", main_dts);
        free(pp.buf);
        return 1;
    }

    int total_nodes = count_nodes(ctx.root);
    printf("      %d nodes  |  %d properties  |  %d labels\n",
           total_nodes, ctx.total_props, ctx.nlabels);

    /* 3. Emit HTML[cite: 1] */
    const char *outfile = "devicetree_viz.html";
    printf("[3/3] Writing %s ...\n", outfile);
    emit_html(&ctx, main_dts, outfile);
    printf("\nDone!  Open \033[1;32m%s\033[0m in your browser.\n", outfile);

    free(pp.buf);
    return 0;
}