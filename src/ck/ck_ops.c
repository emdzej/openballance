/* Parameter operations used by Ballance: the functions registered by ParameterOperations.dll
   (InitInstance 0x24b52210; table in re/ops.txt) and CK2.dll, looked up like
   CKParameterManager::GetOperationFunction (0x24010f6d). Only operations the game uses are ported. */
#include "ck_types.h"
#include "ck_3d.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define G(a, b) {0x##a##u, 0x##b##u}
/* ---- value access (missing source -> 0, like the originals) ---- */
static int32_t I(const CkParameter *p) { int32_t v = 0; if (p && p->value && p->size >= 4) memcpy(&v, p->value, 4); return v; }
static float F(const CkParameter *p) { float v = 0; if (p && p->value && p->size >= 4) memcpy(&v, p->value, 4); return v; }
static const char *S(const CkParameter *p) { return p && p->value && p->size ? (const char *)p->value : ""; }
static void setI(CkParameter *o, int32_t v) { ck_param_set(o, &v, 4); }
static void setF(CkParameter *o, float v) { ck_param_set(o, &v, 4); }
static void setS(CkParameter *o, const char *s) { ck_param_set(o, s, (uint32_t)strlen(s) + 1); }

/* The divide guards (FUN_24b41250 / FUN_24b411c0): a zero divisor becomes 1 and is reported. */
static void div_guard(CkContext *ctx)
{
    if (ctx->log) ctx->log("ParamerOperation: Divide : Division by zero");
}

static void op_add_int(CkContext *c, CkParameter *o, CkParameter *a, CkParameter *b) { setI(o, I(a) + I(b)); }        /* FUN_24b44890 */
static void op_sub_int(CkContext *c, CkParameter *o, CkParameter *a, CkParameter *b) { setI(o, I(a) - I(b)); }        /* FUN_24b44b10 */
static void op_mul_int(CkContext *c, CkParameter *o, CkParameter *a, CkParameter *b) { setI(o, I(a) * I(b)); }        /* FUN_24b44bb0 */
static void op_add_float(CkContext *c, CkParameter *o, CkParameter *a, CkParameter *b) { setF(o, F(a) + F(b)); }      /* FUN_24b41a00 */
static void op_sub_float(CkContext *c, CkParameter *o, CkParameter *a, CkParameter *b) { setF(o, F(a) - F(b)); }      /* FUN_24b41ab0 */
static void op_mul_float(CkContext *c, CkParameter *o, CkParameter *a, CkParameter *b) { setF(o, F(a) * F(b)); }      /* CK2.dll */
static void op_mul_int_float(CkContext *c, CkParameter *o, CkParameter *a, CkParameter *b) { setF(o, (float)I(a) * F(b)); } /* FUN_24b42590 */
static void op_mul_float_int(CkContext *c, CkParameter *o, CkParameter *a, CkParameter *b) { setI(o, (int32_t)(F(a) * (float)I(b))); } /* FUN_24b45220 */

static void op_div_int(CkContext *c, CkParameter *o, CkParameter *a, CkParameter *b)   /* FUN_24b44c50 */
{
    int32_t d = I(b);
    if (!d) { d = 1; div_guard(c); }
    setI(o, I(a) / d);
}
static void op_div_float(CkContext *c, CkParameter *o, CkParameter *a, CkParameter *b) /* FUN_24b41c20 */
{
    float d = F(b);
    if (d == 0.0f) { d = 1.0f; div_guard(c); }
    setF(o, F(a) / d);
}
static void op_mod_int(CkContext *c, CkParameter *o, CkParameter *a, CkParameter *b)   /* FUN_24b44f60 */
{
    int32_t d = I(b), r = 0;
    if (b && d) {
        r = I(a) % d;
        if (r < 0) r += d;
    }
    setI(o, r);
}

static void op_int_to_float(CkContext *c, CkParameter *o, CkParameter *a, CkParameter *b) { setF(o, (float)I(a)); }    /* FUN_24b42ac0 */
static void op_float_to_int(CkContext *c, CkParameter *o, CkParameter *a, CkParameter *b) { setI(o, (int32_t)F(a)); }  /* FUN_24b45710 (ftol: truncation) */
static void op_floats_to_v2(CkContext *c, CkParameter *o, CkParameter *a, CkParameter *b)                              /* FUN_24b4ca00 */
{
    float v[2] = {F(a), F(b)};
    ck_param_set(o, v, 8);
}

/* FUN_24b4ec50: anything -> String through the source type's string function */
static void op_to_string(CkContext *c, CkParameter *o, CkParameter *a, CkParameter *b)
{
    char buf[512];
    if (!a) { setS(o, ""); return; }
    ck_param_to_string(c, a, buf, sizeof buf);
    setS(o, buf);
}
/* FUN_24b412d0: String -> anything through the output type's string function */
static void op_from_string(CkContext *c, CkParameter *o, CkParameter *a, CkParameter *b) { ck_param_from_string(c, o, a ? S(a) : ""); }
/* FUN_24b503d0: object of the output type's class by name (CKContext::GetObjectByNameAndParentClass) */
static void op_by_name(CkContext *c, CkParameter *o, CkParameter *a, CkParameter *b) { ck_param_from_string(c, o, a ? S(a) : ""); }
/* FUN_24b4eb40: concatenation */
static void op_add_string(CkContext *c, CkParameter *o, CkParameter *a, CkParameter *b)
{
    const char *x = S(a), *y = S(b);
    size_t n = strlen(x), m = strlen(y);
    char *buf = malloc(n + m + 1);
    memcpy(buf, x, n);
    memcpy(buf + n, y, m + 1);
    setS(o, buf);
    free(buf);
}
static void op_length(CkContext *c, CkParameter *o, CkParameter *a, CkParameter *b) { setI(o, a ? (int32_t)strlen(S(a)) : 0); } /* FUN_24b45860 */
static void op_name(CkContext *c, CkParameter *o, CkParameter *a, CkParameter *b)                                       /* FUN_24b4ecd0 */
{
    CkObj *x = ck_obj(c, (CkId)I(a));
    setS(o, x && x->name ? x->name : "");
}
static void op_type(CkContext *c, CkParameter *o, CkParameter *a, CkParameter *b)                                       /* FUN_24b4f230 */
{
    CkObj *x = ck_obj(c, (CkId)I(a));
    setI(o, x ? (int32_t)x->cid : 0);
}
static void op_row_count(CkContext *c, CkParameter *o, CkParameter *a, CkParameter *b)                                  /* FUN_24b46040 */
{
    CkDataArray *arr = ck_array(c, (CkId)I(a));
    setI(o, arr ? (int32_t)arr->nrows : 0);
}
static void op_group_count(CkContext *c, CkParameter *o, CkParameter *a, CkParameter *b)                                /* FUN_24b46200 */
{
    CkObj *x = ck_obj(c, (CkId)I(a));
    setI(o, x && x->cid == CKCID_GROUP ? (int32_t)((CkGroup *)x)->members.n : 0);
}
static void op_script(CkContext *c, CkParameter *o, CkParameter *a, CkParameter *b)                                     /* FUN_24b51f50 */
{
    CkBeObject *be = ck_beobject(c, (CkId)I(a));
    int32_t i = I(b);
    setI(o, be && i >= 0 && (uint32_t)i < be->scripts.n ? (int32_t)be->scripts.v[i] : 0);
}
static void op_script_count(CkContext *c, CkParameter *o, CkParameter *a, CkParameter *b)                               /* FUN_24b462e0 */
{
    CkBeObject *be = ck_beobject(c, (CkId)I(a));
    setI(o, be ? (int32_t)be->scripts.n : 0);
}

static void op_current_mesh(CkContext *c, CkParameter *o, CkParameter *a, CkParameter *b)                              /* FUN_24b51ec0 */
{
    Ck3dEntity *e = ck_entity(c, (CkId)I(a));
    setI(o, e ? (int32_t)e->mesh : 0);
}

static void op_material(CkContext *c, CkParameter *o, CkParameter *a, CkParameter *b)                                  /* FUN_24b51550 */
{
    /* CKMesh::GetMaterial(pIn 2) (+0x1dc, CK2_3D 0x1001e04c): material group i + 1 (group 0 is the
       null-material group, kept as CkMesh.materials slot 0); none -> 0 */
    CkMesh *m = ck_mesh(c, (CkId)I(a));
    int32_t i = I(b);
    setI(o, m && i >= 0 && (uint32_t)i + 1 < m->materials.n ? (int32_t)m->materials.v[i + 1] : 0);
}
static void op_2d_material(CkContext *c, CkParameter *o, CkParameter *a, CkParameter *b)                               /* FUN_24b51730 */
{
    Ck2dEntity *e = ck_2dentity(c, (CkId)I(a));
    setI(o, e ? (int32_t)e->material : 0);
}
static void op_world_matrix(CkContext *c, CkParameter *o, CkParameter *a, CkParameter *b)                              /* FUN_24b4d560 */
{
    Ck3dEntity *e = ck_entity(c, (CkId)I(a));
    if (e) ck_param_set(o, e->world, sizeof e->world);
}
static void op_position(CkContext *c, CkParameter *o, CkParameter *a, CkParameter *b)                                  /* FUN_24b49ee0 */
{
    /* GetPosition(res, pIn 2 as the referential) */
    Ck3dEntity *e = ck_entity(c, (CkId)I(a));
    if (!e) return;
    float p[3];
    ck_entity_get_position(c, e, (CkId)I(b), p);
    ck_param_set(o, p, 12);
}
static void op_2d_position(CkContext *c, CkParameter *o, CkParameter *a, CkParameter *b)                               /* FUN_24b4ccf0 */
{
    /* CK2dEntity::GetPosition(res, FALSE, NULL): in pixels */
    Ck2dEntity *e = ck_2dentity(c, (CkId)I(a));
    if (!e) return;
    float r[4], p[2];
    ck_2d_get_homogeneous_rect(e, r);
    p[0] = r[0] * CK_SCREEN_W, p[1] = r[1] * CK_SCREEN_H;
    ck_param_set(o, p, 8);
}
static void op_vector_y(CkContext *c, CkParameter *o, CkParameter *a, CkParameter *b)                                  /* FUN_24b42fe0 */
{
    float v[3] = {0, 0, 0};
    if (a && a->value && a->size >= 12) memcpy(v, a->value, 12);
    setF(o, v[1]);
}
static void op_entity_y(CkContext *c, CkParameter *o, CkParameter *a, CkParameter *b)                                  /* FUN_24b42e70 */
{
    Ck3dEntity *e = ck_entity(c, (CkId)I(a));
    if (e) setF(o, e->world[3][1]);
}
static void op_box_max(CkContext *c, CkParameter *o, CkParameter *a, CkParameter *b)                                   /* FUN_24b4a860 */
{
    float box[6] = {0};
    if (a && a->value && a->size >= 24) memcpy(box, a->value, 24);
    ck_param_set(o, box + 3, 12);
}
static void op_bounding_box(CkContext *c, CkParameter *o, CkParameter *a, CkParameter *b)                              /* FUN_24b4f040 */
{
    Ck3dEntity *e = ck_entity(c, (CkId)I(a));
    if (!e) return;
    float box[6];
    ck_entity_world_box(c, e, box);
    ck_param_set(o, box, 24);
}
static void op_emissive(CkContext *c, CkParameter *o, CkParameter *a, CkParameter *b)                                  /* FUN_24b4e720 */
{
    CkMaterial *m = ck_material(c, (CkId)I(a));
    if (m) ck_param_set(o, &m->emissive, 16);
}
static void op_diffuse(CkContext *c, CkParameter *o, CkParameter *a, CkParameter *b)                                   /* FUN_24b4e6a0 */
{
    CkMaterial *m = ck_material(c, (CkId)I(a));
    if (m) ck_param_set(o, &m->diffuse, 16);
}
/* Object arrays are held as their CkIds */
static void op_array_element(CkContext *c, CkParameter *o, CkParameter *a, CkParameter *b)                             /* FUN_24b509c0 */
{
    int32_t i = I(b);
    CkId id = 0;
    if (a && a->value && i >= 0 && (uint32_t)(i + 1) * 4 <= a->size) memcpy(&id, (const uint8_t *)a->value + i * 4, 4);
    setI(o, ck_obj(c, id) ? (int32_t)id : 0);
}
static void op_material_list(CkContext *c, CkParameter *o, CkParameter *a, CkParameter *b)                             /* FUN_24b4fcf0 */
{
    /* GetMaterialCount (0x1001e01b) = groups - 1, GetMaterial(i) = group i + 1 */
    CkMesh *m = ck_mesh(c, (CkId)I(a));
    if (!m) return;
    ck_param_set(o, m->materials.n > 1 ? m->materials.v + 1 : NULL, m->materials.n > 1 ? (m->materials.n - 1) * 4 : 0);
}

typedef struct {
    CkGuid op, res, p1, p2;
    CkOpFn fn;
} OpReg;

static const OpReg OPS[] = {
    {G(33cc6b49, 3589282b), G(5a5716fd, 44e276d7), G(5a5716fd, 44e276d7), G(5a5716fd, 44e276d7), op_add_int},
    {G(33cc6b49, 3589282b), G(47884c3f, 432c2c20), G(47884c3f, 432c2c20), G(47884c3f, 432c2c20), op_add_float},
    {G(33cc6b49, 3589282b), G(6bd010e2, 115617ea), G(6bd010e2, 115617ea), G(6bd010e2, 115617ea), op_add_string},
    {G(67641171, 6499077a), G(5a5716fd, 44e276d7), G(5a5716fd, 44e276d7), G(5a5716fd, 44e276d7), op_sub_int},
    {G(67641171, 6499077a), G(47884c3f, 432c2c20), G(47884c3f, 432c2c20), G(47884c3f, 432c2c20), op_sub_float},
    {G(38996b85, 334e35c2), G(5a5716fd, 44e276d7), G(5a5716fd, 44e276d7), G(5a5716fd, 44e276d7), op_mul_int},
    {G(38996b85, 334e35c2), G(47884c3f, 432c2c20), G(47884c3f, 432c2c20), G(47884c3f, 432c2c20), op_mul_float},
    {G(38996b85, 334e35c2), G(47884c3f, 432c2c20), G(5a5716fd, 44e276d7), G(47884c3f, 432c2c20), op_mul_int_float},
    {G(38996b85, 334e35c2), G(5a5716fd, 44e276d7), G(47884c3f, 432c2c20), G(5a5716fd, 44e276d7), op_mul_float_int},
    {G(748c3502, 0fea0cf1), G(5a5716fd, 44e276d7), G(5a5716fd, 44e276d7), G(5a5716fd, 44e276d7), op_div_int},
    {G(748c3502, 0fea0cf1), G(47884c3f, 432c2c20), G(47884c3f, 432c2c20), G(47884c3f, 432c2c20), op_div_float},
    {G(2a3f4ab6, 753a4a20), G(5a5716fd, 44e276d7), G(5a5716fd, 44e276d7), G(5a5716fd, 44e276d7), op_mod_int},
    {G(428009b4, 1caa5c78), G(47884c3f, 432c2c20), G(5a5716fd, 44e276d7), G(1cb10760, 419f50c5), op_int_to_float},
    {G(428009b4, 1caa5c78), G(5a5716fd, 44e276d7), G(47884c3f, 432c2c20), G(1cb10760, 419f50c5), op_float_to_int},
    {G(428009b4, 1caa5c78), G(4efcb34a, 6079e42f), G(47884c3f, 432c2c20), G(47884c3f, 432c2c20), op_floats_to_v2},
    {G(428009b4, 1caa5c78), G(5a5716fd, 44e276d7), G(6bd010e2, 115617ea), G(1cb10760, 419f50c5), op_from_string},
    {G(428009b4, 1caa5c78), G(30ec20ab, 6df6517d), G(6bd010e2, 115617ea), G(1cb10760, 419f50c5), op_from_string},
    {G(428009b4, 1caa5c78), G(6bd010e2, 115617ea), G(5a5716fd, 44e276d7), G(1cb10760, 419f50c5), op_to_string},
    {G(428009b4, 1caa5c78), G(6bd010e2, 115617ea), G(47884c3f, 432c2c20), G(1cb10760, 419f50c5), op_to_string},
    {G(428009b4, 1caa5c78), G(6bd010e2, 115617ea), G(1ad52a8e, 5e741920), G(1cb10760, 419f50c5), op_to_string},
    {G(428009b4, 1caa5c78), G(6bd010e2, 115617ea), G(54b4422b, 730f0f4f), G(1cb10760, 419f50c5), op_to_string},
    {G(428009b4, 1caa5c78), G(6bd010e2, 115617ea), G(f3c84b4e, 0ffacc34), G(1cb10760, 419f50c5), op_to_string},
    {G(599203f6, 23b06096), G(30ec20ab, 6df6517d), G(6bd010e2, 115617ea), G(1cb10760, 419f50c5), op_by_name},
    {G(556a69af, 076e3f09), G(5a5716fd, 44e276d7), G(6bd010e2, 115617ea), G(1cb10760, 419f50c5), op_length},
    {G(5939156e, 62ab3472), G(6bd010e2, 115617ea), G(30ec20ab, 6df6517d), G(1cb10760, 419f50c5), op_name},
    {G(35aa0ddb, 7a5e3335), G(19644e43, 4d6f6123), G(30ec20ab, 6df6517d), G(1cb10760, 419f50c5), op_type},
    {G(fb556b23, 69b22a75), G(5a5716fd, 44e276d7), G(024d52f1, 678223b2), G(1cb10760, 419f50c5), op_row_count},
    {G(44df6f61, 5f8f55c8), G(5a5716fd, 44e276d7), G(5c0f151b, 6f0547fd), G(1cb10760, 419f50c5), op_group_count},
    {G(250020d4, 10d51b91), G(7ea4176d, 1b405d30), G(71d80779, 402f42f3), G(5a5716fd, 44e276d7), op_script},
    {G(6a5c5061, 5efe7428), G(24535345, 65d15229), G(5b8a05d5, 31ea28d4), G(1cb10760, 419f50c5), op_current_mesh},
    {G(6b8c78bd, 7b846d3c), G(55ab12cd, 22ae6a8b), G(24535345, 65d15229), G(5a5716fd, 44e276d7), op_material},
    {G(6b8c78bd, 7b846d3c), G(55ab12cd, 22ae6a8b), G(181671e3, 1bdc1503), G(1cb10760, 419f50c5), op_2d_material},
    {G(472851ba, 5ccc2831), G(643f046e, 65211b71), G(5b8a05d5, 31ea28d4), G(1cb10760, 419f50c5), op_world_matrix},
    {G(4bc87aea, 6b5b643e), G(48824eae, 2fe47960), G(5b8a05d5, 31ea28d4), G(5b8a05d5, 31ea28d4), op_position},
    {G(4bc87aea, 6b5b643e), G(48824eae, 2fe47960), G(5b8a05d5, 31ea28d4), G(1cb10760, 419f50c5), op_position},
    {G(4bc87aea, 6b5b643e), G(4efcb34a, 6079e42f), G(181671e3, 1bdc1503), G(1cb10760, 419f50c5), op_2d_position},
    {G(389f72bd, 7aef3482), G(47884c3f, 432c2c20), G(48824eae, 2fe47960), G(1cb10760, 419f50c5), op_vector_y},
    {G(389f72bd, 7aef3482), G(47884c3f, 432c2c20), G(5b8a05d5, 31ea28d4), G(1cb10760, 419f50c5), op_entity_y},
    {G(6ea214cb, 43be6b56), G(48824eae, 2fe47960), G(668649c8, 283e2ee1), G(1cb10760, 419f50c5), op_box_max},
    {G(31a7649f, 305e5164), G(668649c8, 283e2ee1), G(5b8a05d5, 31ea28d4), G(1cb10760, 419f50c5), op_bounding_box},
    {G(13684596, 0ab57f34), G(57d42fee, 7cbb3b91), G(55ab12cd, 22ae6a8b), G(1cb10760, 419f50c5), op_emissive},
    {G(0f9e20cc, 6ea10a47), G(57d42fee, 7cbb3b91), G(55ab12cd, 22ae6a8b), G(1cb10760, 419f50c5), op_diffuse},
    {G(660f5c52, 0adf7be9), G(30ec20ab, 6df6517d), G(71df7142, c437133a), G(5a5716fd, 44e276d7), op_array_element},
    {G(765f7aa3, 33d679b2), G(71df7142, c437133a), G(24535345, 65d15229), G(1cb10760, 419f50c5), op_material_list},
    {G(551c20ba, 20da6d50), G(5a5716fd, 44e276d7), G(71d80779, 402f42f3), G(1cb10760, 419f50c5), op_script_count},
};

static CkOpFn exact(CkGuid op, CkGuid res, CkGuid p1, CkGuid p2)
{
    for (size_t i = 0; i < sizeof OPS / sizeof *OPS; i++)
        if (ck_guid_eq(OPS[i].op, op) && ck_guid_eq(OPS[i].res, res) && ck_guid_eq(OPS[i].p1, p1) && ck_guid_eq(OPS[i].p2, p2))
            return OPS[i].fn;
    return NULL;
}

static bool has(CkGuid g) { return g.a || g.b; }

CkOpFn ck_op_function(CkGuid op, CkGuid res, CkGuid p1, CkGuid p2)
{
    CkOpFn f = exact(op, res, p1, p2);
    if (f) return f;
    CkGuid rp = ck_type_parent(res), pp1 = ck_type_parent(p1), pp2 = ck_type_parent(p2);
    if (has(pp1)) {
        if (has(pp2)) {
            if (has(rp) && (f = ck_op_function(op, rp, pp1, pp2))) return f;
            if ((f = ck_op_function(op, res, pp1, pp2))) return f;
        }
        if (has(rp) && (f = ck_op_function(op, rp, pp1, p2))) return f;
        if ((f = ck_op_function(op, res, pp1, p2))) return f;
    }
    if (has(pp2)) {
        if (has(rp) && (f = ck_op_function(op, rp, p1, pp2))) return f;
        if ((f = ck_op_function(op, res, p1, pp2))) return f;
    }
    if (has(rp) && (f = ck_op_function(op, rp, p1, p2))) return f;
    return NULL;
}
