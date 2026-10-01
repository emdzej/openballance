/* TT_DatabaseManager_RT.dll (Terratools): arrays saved to and loaded from an obfuscated file
   (Database.tdb). Manager 4db6188e:287e1410: file name + "crypted" flag (FUN_10002940), registered array
   names (FUN_10001c20 register, FUN_10001e10 clear), load FUN_10001e40, save FUN_10002420.

   File: every byte e = rotr3((-b) ^ 0xaf); decoded, a sequence of records
     name NUL, u32 size, then size bytes: i32 columns, i32 rows, i32 key column,
     per column: name NUL, u32 type (1 int, 2 float, 3 string), then the cells column by column
     (int/float 4 bytes, strings NUL-terminated).
   The "crypted" flag is stored but the original always encodes and decodes. */
#include "bb.h"
#include <stdio.h>
#include <stdlib.h>

static uint8_t decode(uint8_t e) { return (uint8_t)-(uint8_t)(((uint8_t)(e << 3) | (e >> 5)) ^ 0xaf); }
static uint8_t encode(uint8_t b)
{
    uint8_t x = (uint8_t)((uint8_t)-b ^ 0xaf);
    return (uint8_t)((x >> 3) | (uint8_t)(x << 5));
}

/* The saves live under the file's base name (the original path is an absolute Windows path). */
static const char *base_name(const char *path)
{
    const char *b = path;
    for (const char *p = path; *p; p++)
        if (*p == '/' || *p == '\\' || *p == ':') b = p + 1;
    return b;
}

static void db_register(CkContext *ctx, const char *name)
{
    for (uint32_t i = 0; i < ctx->ndb_arrays; i++)
        if (!strcmp(ctx->db_arrays[i], name)) return;   /* 0x15: already registered */
    ctx->db_arrays = realloc(ctx->db_arrays, (ctx->ndb_arrays + 1) * sizeof *ctx->db_arrays);
    ctx->db_arrays[ctx->ndb_arrays++] = strdup(name);
}

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* FUN_10001e40: returns 1 ok, 0x1f no file, 0x20 bad content, 0x21 array not in file */
static int db_load(CkContext *ctx, bool auto_register, const char *want)
{
    if (!ctx->db_file || !ctx->load_user_file) return 0x1f;
    size_t n;
    uint8_t *d = ctx->load_user_file(ctx, base_name(ctx->db_file), &n);
    if (!d) return 0x1f;
    for (size_t i = 0; i < n; i++) d[i] = decode(d[i]);
    int found = 0;
    size_t p = 0;
    while (p < n) {
        const uint8_t *nul = memchr(d + p, 0, n - p);
        if (!nul || (size_t)(nul - d) + 5 > n) break;
        const char *name = (const char *)d + p;
        size_t q = (size_t)(nul - d) + 1;
        uint32_t size = rd32(d + q);
        q += 4;
        if (size > n - q) { free(d); return 0x20; }
        const uint8_t *blk = d + q;
        p = q + size;
        if (want && *want && strcmp(want, name)) continue;
        int32_t ncols = (int32_t)rd32(blk), nrows = (int32_t)rd32(blk + 4), key = (int32_t)rd32(blk + 8);
        if (ncols < 0 || nrows < 0 || ncols <= key || key < -1) { free(d); return 0x20; }
        CkId id = ck_find(ctx, name, CKCID_DATAARRAY);
        CkDataArray *a = ck_array(ctx, id);
        if (!a) {
            a = (CkDataArray *)ck_create(ctx, CKCID_DATAARRAY, name);   /* + CKScene::AddObjectToScene */
            a->key_column = -1;
        } else {
            ck_array_clear(ctx, a);
            for (uint32_t c = 0; c < a->ncols; c++) free(a->cols[c].name);
            free(a->cols);
            a->cols = NULL;
            a->ncols = 0;
        }
        if (auto_register) db_register(ctx, name);
        size_t k = 12;
        uint32_t *types = calloc((size_t)ncols + 1, sizeof *types);
        a->cols = calloc((size_t)ncols + 1, sizeof *a->cols);
        for (int32_t c = 0; c < ncols; c++) {
            const char *cn = (const char *)blk + k;
            size_t l = strnlen(cn, size - k);
            k += l + 1;
            types[c] = rd32(blk + k);
            k += 4;
            if (types[c] >= 1 && types[c] <= 3) {          /* other column types are skipped */
                a->cols[a->ncols].name = strndup(cn, l);
                a->cols[a->ncols].type = types[c];
                a->ncols++;
            }
        }
        a->key_column = key;
        for (int32_t r = 0; r < nrows; r++) ck_array_add_row(ctx, a);
        for (uint32_t c = 0; c < a->ncols; c++)
            for (int32_t r = 0; r < nrows; r++) {
                CkCell *cell = ck_array_cell(a, (uint32_t)r, c);
                if (a->cols[c].type == CKARRAYTYPE_STRING) {
                    const char *s = (const char *)blk + k;
                    size_t l = strnlen(s, size > k ? size - k : 0);
                    free(cell->s);
                    cell->s = strndup(s, l);
                    k += l + 1;
                } else {
                    cell->i = (int32_t)rd32(blk + k);
                    k += 4;
                }
            }
        free(types);
        found++;
        if (want && *want) break;
    }
    free(d);
    return (want && *want && !found) ? 0x21 : 1;
}

static void put32(uint8_t **buf, size_t *n, size_t *cap, uint32_t v)
{
    if (*n + 4 > *cap) *buf = realloc(*buf, *cap = (*n + 4) * 2);
    memcpy(*buf + *n, &v, 4);
    *n += 4;
}
static void puts0(uint8_t **buf, size_t *n, size_t *cap, const char *s)
{
    size_t l = strlen(s ? s : "") + 1;
    if (*n + l > *cap) *buf = realloc(*buf, *cap = (*n + l) * 2);
    memcpy(*buf + *n, s ? s : "", l);
    *n += l;
}

/* FUN_10002420: returns 1 ok, 0x29 write error, 0x2a registered array missing */
static int db_save(CkContext *ctx)
{
    uint8_t *buf = NULL;
    size_t n = 0, cap = 0;
    for (uint32_t i = 0; i < ctx->ndb_arrays; i++) {
        CkDataArray *a = ck_array(ctx, ck_find(ctx, ctx->db_arrays[i], CKCID_DATAARRAY));
        if (!a) { free(buf); return 0x2a; }
        puts0(&buf, &n, &cap, a->be.h.name);
        size_t size_at = n;
        put32(&buf, &n, &cap, 0);
        put32(&buf, &n, &cap, a->ncols);
        put32(&buf, &n, &cap, a->nrows);
        put32(&buf, &n, &cap, (uint32_t)a->key_column);
        for (uint32_t c = 0; c < a->ncols; c++) {
            puts0(&buf, &n, &cap, a->cols[c].name);
            put32(&buf, &n, &cap, a->cols[c].type);
        }
        for (uint32_t c = 0; c < a->ncols; c++)
            for (uint32_t r = 0; r < a->nrows; r++) {
                CkCell *cell = ck_array_cell(a, r, c);
                if (a->cols[c].type == CKARRAYTYPE_STRING) puts0(&buf, &n, &cap, cell->s);
                else if (a->cols[c].type == CKARRAYTYPE_INT || a->cols[c].type == CKARRAYTYPE_FLOAT) put32(&buf, &n, &cap, (uint32_t)cell->i);
            }
        uint32_t size = (uint32_t)(n - size_at - 4);
        memcpy(buf + size_at, &size, 4);
    }
    for (size_t i = 0; i < n; i++) buf[i] = encode(buf[i]);
    bool ok = ctx->db_file && ctx->save_user_file && ctx->save_user_file(ctx, base_name(ctx->db_file), buf, n);
    free(buf);
    return ok ? 1 : 0x29;
}

/* ---- Set Database Properties 1436624f:34e1290a (FUN_10001710): pIn File, Crypted ---- */
static int bb_set_db_properties(CkContext *ctx, CkBehavior *b)
{
    ck_activate_input(ctx, b, 0, false);
    const char *file = bb_in_string(ctx, b, 0);
    if (!file) {
        if (ctx->log) ctx->log("Keine Datei angegeben");
        ck_activate_output(ctx, b, 1, true);
        return CKBR_OK;
    }
    if (*file) {   /* FUN_10002940 ignores an empty name */
        free(ctx->db_file);
        ctx->db_file = strdup(file);
        bb_get_in(ctx, b, 1, &ctx->db_crypted, 4);
    }
    ck_activate_output(ctx, b, 0, true);
    return CKBR_OK;
}

/* ---- Load Database (FUN_10001130): pIn Auto Register, Array Name (empty = all); outputs OK, Error ---- */
static int bb_load_database(CkContext *ctx, CkBehavior *b)
{
    if (ck_input_active(ctx, b, 0)) ck_activate_input(ctx, b, 0, false);
    int32_t reg = 0;
    bb_get_in(ctx, b, 0, &reg, 4);
    int r = db_load(ctx, reg == 1, bb_in_string(ctx, b, 1));
    if (r == 1) {
        ck_activate_output(ctx, b, 0, true);
    } else {
        if (ctx->log) ctx->log(r == 0x1f ? "Datei nicht gefunden" : r == 0x20 ? "Fehler im Inhalt der Datei!" : "In der Datei ist kein Array vorhanden");
        ck_activate_output(ctx, b, 1, true);
    }
    return CKBR_OK;
}

/* ---- Register Array (FUN_10001340): inputs Register, Clear; pIn Array ---- */
static int bb_register_array(CkContext *ctx, CkBehavior *b)
{
    if (!ck_input_active(ctx, b, 0)) {
        if (!ck_input_active(ctx, b, 1)) return CKBR_OK;
        ck_activate_input(ctx, b, 1, false);
        for (uint32_t i = 0; i < ctx->ndb_arrays; i++) free(ctx->db_arrays[i]);
        ctx->ndb_arrays = 0;
        ck_activate_output(ctx, b, 0, true);
        return CKBR_OK;
    }
    ck_activate_input(ctx, b, 0, false);
    CkObj *a = ck_obj(ctx, bb_in_object(ctx, b, 0));
    if (!a) {
        ck_activate_output(ctx, b, 1, true);
        return CKBR_OK;
    }
    db_register(ctx, a->name);
    ck_activate_output(ctx, b, 0, true);
    return CKBR_OK;
}

/* ---- Save Database (FUN_10001550): outputs OK, Error ---- */
static int bb_save_database(CkContext *ctx, CkBehavior *b)
{
    ck_activate_input(ctx, b, 0, false);
    int r = db_save(ctx);
    if (r != 1 && ctx->log) ctx->log(r == 0x29 ? "Fehler beim Speichern der Datei!" : "Ein registriertes Array ist nicht vorhanden");
    ck_activate_output(ctx, b, r == 1 ? 0 : 1, true);
    return CKBR_OK;
}

BB_DECL(d_set_db_properties, 1436624f, 34e1290a, "Set Database Properties", bb_set_db_properties);
BB_DECL(d_load_database, 05441494, 38ac7789, "Load Database", bb_load_database);
BB_DECL(d_register_array, 348773dc, 19ae6322, "Register Array", bb_register_array);
BB_DECL(d_save_database, 5d303e9d, 552c0af2, "Save Database", bb_save_database);

const CkBBDecl *const bb_ttdatabase[] = {&d_set_db_properties, &d_load_database, &d_register_array, &d_save_database};
const unsigned bb_ttdatabase_count = sizeof bb_ttdatabase / sizeof *bb_ttdatabase;
