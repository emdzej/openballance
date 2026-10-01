/* CKDataArray operations (CK2.dll 0x24026000-0x24029000, CKDataArray::*). */
#include "ck_types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

CkDataArray *ck_array(const CkContext *ctx, CkId id)
{
    CkObj *o = ck_obj(ctx, id);
    return o && o->cid == CKCID_DATAARRAY ? (CkDataArray *)o : NULL;
}

CkCell *ck_array_cell(CkDataArray *a, uint32_t row, uint32_t col)
{
    return a && row < a->nrows && col < a->ncols ? &a->cells[row * a->ncols + col] : NULL;
}

/* CKDataArray::AddRow 0x24027d6a: a new row with default cells; parameter columns get a new parameter
   of the column type. */
uint32_t ck_array_add_row(CkContext *ctx, CkDataArray *a)
{
    if (a->nrows == a->cap_rows) {
        a->cap_rows = a->cap_rows ? a->cap_rows * 2 : 8;
        a->cells = realloc(a->cells, (size_t)a->cap_rows * (a->ncols ? a->ncols : 1) * sizeof *a->cells);
    }
    CkCell *row = &a->cells[a->nrows * a->ncols];
    memset(row, 0, a->ncols * sizeof *row);
    for (uint32_t c = 0; c < a->ncols; c++) {
        if (a->cols[c].type == CKARRAYTYPE_STRING) {
            row[c].s = strdup("");
        } else if (a->cols[c].type == CKARRAYTYPE_PARAMETER) {
            CkParameter *p = (CkParameter *)ck_create(ctx, CKCID_PARAMETEROUT, a->cols[c].name);
            p->kind = CKP_OUT;
            p->type = a->cols[c].param_type;
            p->owner = a->be.h.id;
            row[c].obj = p->h.id;
        }
    }
    return a->nrows++;
}

/* CKDataArray::RemoveRow 0x24027f3a */
void ck_array_remove_row(CkContext *ctx, CkDataArray *a, uint32_t row)
{
    if (row >= a->nrows) return;
    for (uint32_t c = 0; c < a->ncols; c++)
        if (a->cols[c].type == CKARRAYTYPE_STRING) free(a->cells[row * a->ncols + c].s);
    memmove(&a->cells[row * a->ncols], &a->cells[(row + 1) * a->ncols], (size_t)(a->nrows - row - 1) * a->ncols * sizeof *a->cells);
    a->nrows--;
}

/* CKDataArray::Clear 0x24028013 */
void ck_array_clear(CkContext *ctx, CkDataArray *a)
{
    while (a->nrows) ck_array_remove_row(ctx, a, a->nrows - 1);
}

/* FUN_2402931b / FUN_240293a1 */
bool ck_compare_int(int32_t op, int32_t a, int32_t b)
{
    switch (op) {
    case 1: return a == b;
    case 2: return a != b;
    case 3: return a < b;
    case 4: return a <= b;
    case 5: return a > b;
    case 6: return a >= b;
    }
    return false;
}

bool ck_compare_float(int32_t op, float a, float b)
{
    switch (op) {
    case 1: return a == b;
    case 2: return a != b;
    case 3: return a < b;
    case 4: return a <= b;
    case 5: return a > b;
    case 6: return a >= b;
    }
    return false;
}

bool ck_array_set_from_param(CkContext *ctx, CkDataArray *a, uint32_t row, uint32_t col, CkParameter *p)
{
    CkCell *c = ck_array_cell(a, row, col);
    if (!c || !p) return false;
    switch (a->cols[col].type) {
    case CKARRAYTYPE_STRING: {
        char buf[1024];
        ck_param_to_string(ctx, p, buf, sizeof buf);
        free(c->s);
        c->s = strdup(buf);
        break;
    }
    case CKARRAYTYPE_PARAMETER: {
        CkParameter *cp = ck_param(ctx, c->obj);
        if (cp) ck_param_set(cp, p->value, p->size);
        break;
    }
    default:
        c->i = 0;
        if (p->value && p->size >= 4) memcpy(&c->i, p->value, 4);
    }
    return true;
}

/* The column's test function (column vtable +0x14: FUN_240268ce int/object, FUN_2402684b float,
   FUN_24026880 string, FUN_24026903 parameter): cell <op> key */
static bool cell_test(CkContext *ctx, CkDataArray *a, uint32_t r, uint32_t col, int32_t op, uint32_t key,
                      const void *keyptr, uint32_t size)
{
        CkCell *c = &a->cells[r * a->ncols + col];
        bool hit = false;
        switch (a->cols[col].type) {
        case CKARRAYTYPE_FLOAT: {
            float k;
            memcpy(&k, &key, 4);
            hit = ck_compare_float(op, c->f, k);
            break;
        }
        case CKARRAYTYPE_STRING:
            hit = ck_compare_int(op, strcmp(c->s ? c->s : "", keyptr ? (const char *)keyptr : ""), 0);
            break;
        case CKARRAYTYPE_PARAMETER: {
            CkParameter *cp = ck_param(ctx, c->obj);
            if (cp && cp->value && keyptr) hit = ck_compare_int(op, memcmp(cp->value, keyptr, size <= cp->size ? size : cp->size), 0);
            break;
        }
        default: {
            hit = ck_compare_int(op, c->i, (int32_t)key);
        }
        }
        return hit;
}

int32_t ck_array_find_row(CkContext *ctx, CkDataArray *a, uint32_t col, int32_t op, uint32_t key, const void *keyptr,
                          uint32_t size, int32_t start)
{
    if (col >= a->ncols || start < 0 || (uint32_t)start >= a->nrows) return -1;
    for (uint32_t r = (uint32_t)start; r < a->nrows; r++)
        if (cell_test(ctx, a, r, col, op, key, keyptr, size)) return (int32_t)r;
    return -1;
}

/* CKDataArray::TestRow 0x2402822e */
bool ck_array_test_cell(CkContext *ctx, CkDataArray *a, int32_t row, int32_t col, int32_t op, uint32_t key,
                        const void *keyptr, uint32_t size)
{
    if (col < 0 || (uint32_t)col >= a->ncols || row < 0 || (uint32_t)row >= a->nrows) return false;
    return cell_test(ctx, a, (uint32_t)row, (uint32_t)col, op, key, keyptr, size);
}

/* CKDataArray::GetCount 0x24028f2e */
int32_t ck_array_count(CkContext *ctx, CkDataArray *a, int32_t col, int32_t op, uint32_t key, const void *keyptr,
                       uint32_t size)
{
    if (col < 0 || (uint32_t)col >= a->ncols) return 0;
    int32_t n = 0;
    for (uint32_t r = 0; r < a->nrows; r++) n += cell_test(ctx, a, r, (uint32_t)col, op, key, keyptr, size);
    return n;
}

/* The column's compare function (column vtable +0x10: LAB_24026708 int/object: b - a with wraparound;
   LAB_24026735 float: +-2 by the sign of b - a; LAB_24026794 string: strcmp(b, a)), negated when not
   ascending. Parameter columns (LAB_240267d9) compare their bytes. */
static int column_compare(CkContext *ctx, const CkDataArray *a, uint32_t col, uint32_t ra, uint32_t rb, bool ascending)
{
    const CkCell *x = &a->cells[ra * a->ncols + col], *y = &a->cells[rb * a->ncols + col];
    int v;
    switch (a->cols[col].type) {
    case CKARRAYTYPE_FLOAT: {
        float d = y->f - x->f;
        v = d == 0 ? 0 : d > 0 ? 2 : -2;
        break;
    }
    case CKARRAYTYPE_STRING: v = strcmp(y->s ? y->s : "", x->s ? x->s : ""); break;
    case CKARRAYTYPE_PARAMETER: {
        CkParameter *p = ck_param(ctx, x->obj), *q = ck_param(ctx, y->obj);
        v = p && q && p->value && q->value ? memcmp(q->value, p->value, p->size < q->size ? p->size : q->size) : 0;
        break;
    }
    default: v = (int32_t)((uint32_t)y->i - (uint32_t)x->i);
    }
    return ascending ? v : -v;
}

int ck_array_compare_rows(CkContext *ctx, const CkDataArray *a, uint32_t col, uint32_t best, uint32_t cand)
{
    return column_compare(ctx, a, col, best, cand, true);
}

/* CKDataArray::Sort 0x24028c0d: msvcrt qsort over the row pointers with LAB_240266f1 = the column compare
   with its arguments swapped. The quicksort below is the msvcrt one (median swap, partition, insertion
   of the largest for up to 8 elements), so equal keys end up in the same order as in the original. */
typedef struct {
    CkContext *ctx;
    const CkDataArray *a;
    uint32_t col;
    bool ascending;
} SortCtx;

static int row_cmp(const SortCtx *s, uint32_t ra, uint32_t rb) { return column_compare(s->ctx, s->a, s->col, rb, ra, s->ascending); }

static void swap_u32(uint32_t *x, uint32_t *y)
{
    uint32_t t = *x;
    *x = *y;
    *y = t;
}

static void msvcrt_qsort(uint32_t *base, size_t num, const SortCtx *s)
{
    enum { CUTOFF = 8 };
    uint32_t *lostk[30], *histk[30];
    int sp = 0;
    if (num < 2) return;
    uint32_t *lo = base, *hi = base + num - 1;
    for (;;) {
        size_t size = (size_t)(hi - lo) + 1;
        if (size <= CUTOFF) {
            for (uint32_t *h = hi; h > lo; h--) {        /* shortsort: move the maximum to the end */
                uint32_t *max = lo;
                for (uint32_t *p = lo + 1; p <= h; p++)
                    if (row_cmp(s, *p, *max) > 0) max = p;
                swap_u32(max, h);
            }
        } else {
            uint32_t *mid = lo + size / 2;
            swap_u32(mid, lo);
            uint32_t *loguy = lo, *higuy = hi + 1;
            for (;;) {
                do loguy++; while (loguy <= hi && row_cmp(s, *loguy, *lo) <= 0);
                do higuy--; while (higuy > lo && row_cmp(s, *higuy, *lo) >= 0);
                if (higuy < loguy) break;
                swap_u32(loguy, higuy);
            }
            swap_u32(lo, higuy);
            if (higuy - 1 - lo >= hi - loguy) {
                if (lo + 1 < higuy) lostk[sp] = lo, histk[sp] = higuy - 1, sp++;
                if (loguy < hi) {
                    lo = loguy;
                    continue;
                }
            } else {
                if (loguy < hi) lostk[sp] = loguy, histk[sp] = hi, sp++;
                if (lo + 1 < higuy) {
                    hi = higuy - 1;
                    continue;
                }
            }
        }
        if (--sp < 0) return;
        lo = lostk[sp];
        hi = histk[sp];
    }
}

void ck_array_sort(CkContext *ctx, CkDataArray *a, int32_t col, bool ascending)
{
    if (col < 0 || (uint32_t)col >= a->ncols || a->nrows < 2) return;
    uint32_t *order = malloc(a->nrows * sizeof *order);
    for (uint32_t r = 0; r < a->nrows; r++) order[r] = r;
    SortCtx s = {ctx, a, (uint32_t)col, ascending};
    msvcrt_qsort(order, a->nrows, &s);
    CkCell *cells = malloc((size_t)(a->cap_rows > a->nrows ? a->cap_rows : a->nrows) * a->ncols * sizeof *cells);
    for (uint32_t r = 0; r < a->nrows; r++) memcpy(&cells[r * a->ncols], &a->cells[order[r] * a->ncols], a->ncols * sizeof *cells);
    free(a->cells);
    a->cells = cells;
    free(order);
}

void ck_array_insert_column(CkContext *ctx, CkDataArray *a, int32_t index, uint32_t type, const char *name, CkGuid ptype)
{
    uint32_t nc = a->ncols + 1;
    uint32_t at = index < 0 || (uint32_t)index > a->ncols ? a->ncols : (uint32_t)index;
    a->cols = realloc(a->cols, nc * sizeof *a->cols);
    memmove(&a->cols[at + 1], &a->cols[at], (a->ncols - at) * sizeof *a->cols);
    a->cols[at] = (CkArrayColumn){strdup(name ? name : ""), type, ptype};
    CkCell *cells = calloc((size_t)(a->cap_rows ? a->cap_rows : 1) * nc, sizeof *cells);
    for (uint32_t r = 0; r < a->nrows; r++) {
        memcpy(&cells[r * nc], &a->cells[r * a->ncols], at * sizeof *cells);
        memcpy(&cells[r * nc + at + 1], &a->cells[r * a->ncols + at], (a->ncols - at) * sizeof *cells);
        CkCell *c = &cells[r * nc + at];
        if (type == CKARRAYTYPE_STRING) {
            c->s = strdup("");
        } else if (type == CKARRAYTYPE_PARAMETER) {
            CkParameter *p = (CkParameter *)ck_create(ctx, CKCID_PARAMETEROUT, name);
            p->kind = CKP_OUT;
            p->type = ptype;
            p->owner = a->be.h.id;
            c->obj = p->h.id;
        }
    }
    free(a->cells);
    a->cells = cells;
    a->ncols = nc;
    if (a->key_column >= (int32_t)at) a->key_column++;
}

void ck_array_remove_column(CkContext *ctx, CkDataArray *a, int32_t index)
{
    if (index < 0 || (uint32_t)index >= a->ncols) return;
    uint32_t at = (uint32_t)index, nc = a->ncols - 1;
    for (uint32_t r = 0; r < a->nrows; r++)
        if (a->cols[at].type == CKARRAYTYPE_STRING) free(a->cells[r * a->ncols + at].s);
    for (uint32_t r = 0; r < a->nrows; r++) {
        CkCell *row = &a->cells[r * a->ncols];
        memmove(&a->cells[r * nc], row, at * sizeof *row);
        memmove(&a->cells[r * nc + at], row + at + 1, (a->ncols - at - 1) * sizeof *row);
    }
    free(a->cols[at].name);
    memmove(&a->cols[at], &a->cols[at + 1], (a->ncols - at - 1) * sizeof *a->cols);
    a->ncols = nc;
    if (a->key_column == (int32_t)at) a->key_column = -1;
    else if (a->key_column > (int32_t)at) a->key_column--;
}
