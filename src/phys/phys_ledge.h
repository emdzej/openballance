/* IVP's compact collision geometry (docs/ivp_collision.md 1.2), kept byte for byte: the minimize and event
   solvers navigate it with pointer arithmetic (`edge & ~0xf` is the triangle, `edge & 0xc` its slot, the
   opposite edge an offset in 4-byte units, the ledge 16 bytes per triangle before the triangle). Every ledge
   and surface is 16-byte aligned. */
#pragma once
#include <stdint.h>

/* IVP_Compact_Poly_Point (16 bytes): object-space coordinates, the 4th float always 0 */
typedef struct {
    float k[3];
    float hesse;
} IvpPolyPoint;

/* IVP_Compact_Edge: bits 0..15 start point index, 16..30 signed offset (in edges) to the opposite edge,
   31 virtual (always 0 in Ballance) */
typedef uint32_t IvpEdge;

/* IVP_Compact_Triangle (16 bytes): bits 0..11 tri_index, 12..23 pierce_index, 24..30 material (0), 31 virtual;
   then the three edges p0->p1, p1->p2, p2->p0 (outward normal (p1-p0) x (p2-p0)) */
typedef struct {
    uint32_t word;
    IvpEdge e[3];
} IvpCompactTriangle;

/* IVP_Compact_Ledge (16-byte header, then n_triangles triangles) */
typedef struct {
    int32_t c_point_offset;       /* +0x0: ledge -> its points (may point into the surface's shared pool) */
    int32_t ledgetree_node_offset;/* +0x4: 0 for every leaf ledge */
    uint32_t flags;               /* +0x8: bits 0..1 has_children, 2..3 is_compact (1), 8..31 size / 16 */
    int16_t n_triangles;          /* +0xc */
    int16_t for_future_use;       /* +0xe */
} IvpCompactLedge;

/* IVP_Compact_Ledgetree_Node (0x1c bytes, preorder: the left child follows at +0x1c) */
typedef struct {
    int32_t offset_right_node;    /* 0: a leaf */
    int32_t offset_compact_ledge; /* leaf: its ledge; internal: a hull (never in Ballance) or 0 */
    float center[3];
    float radius;
    uint8_t box_sizes[3];         /* AABB half extents in units of radius * 0.004 */
    uint8_t free_0;
} IvpLedgeNode;

/* IVP_Compact_Surface (0x30-byte header) */
typedef struct {
    float mass_center[3];         /* +0x00 */
    float rotation_inertia[3];    /* +0x0c: unit inertia about the mass centre (the sqrt quirk, 1.6.7) */
    float upper_limit_radius;     /* +0x18 */
    uint32_t dev_and_size;        /* +0x1c: bits 0..7 max_factor_surface_deviation, 8..31 byte size */
    int32_t offset_ledgetree_root;/* +0x20 */
    int32_t dummy[3];
} IvpCompactSurface;

_Static_assert(sizeof(IvpPolyPoint) == 16, "point");
_Static_assert(sizeof(IvpCompactTriangle) == 16, "triangle");
_Static_assert(sizeof(IvpCompactLedge) == 16, "ledge");
_Static_assert(sizeof(IvpLedgeNode) == 0x1c, "node");
_Static_assert(sizeof(IvpCompactSurface) == 0x30, "surface");

/* edge navigation (tables 0x100685b8 / 0x100685c8, indexed by edge & 0xc) */
static inline const IvpEdge *ivp_next(const IvpEdge *e)
{
    static const int8_t t[4] = {0, 4, 4, -8};
    return (const IvpEdge *)((const char *)e + t[((uintptr_t)e >> 2) & 3]);
}
static inline const IvpEdge *ivp_prev(const IvpEdge *e)
{
    static const int8_t t[4] = {0, 8, -4, -4};
    return (const IvpEdge *)((const char *)e + t[((uintptr_t)e >> 2) & 3]);
}
static inline const IvpEdge *ivp_opp(const IvpEdge *e) { return e + ((int32_t)(*e << 1) >> 17); }
static inline const IvpCompactTriangle *ivp_tri(const IvpEdge *e)
{
    return (const IvpCompactTriangle *)((uintptr_t)e & ~(uintptr_t)0xf);
}
static inline int ivp_tri_index(const IvpCompactTriangle *t) { return (int)(t->word & 0xfff); }
static inline int ivp_pierce_index(const IvpCompactTriangle *t) { return (int)((t->word >> 12) & 0xfff); }
static inline const IvpCompactLedge *ivp_ledge_of_tri(const IvpCompactTriangle *t)
{
    return (const IvpCompactLedge *)((const char *)t - 16 * (ivp_tri_index(t) + 1));
}
/* 0x10016470 */
static inline const IvpCompactLedge *ivp_ledge_of(const IvpEdge *e) { return ivp_ledge_of_tri(ivp_tri(e)); }
static inline const IvpCompactTriangle *ivp_ledge_tri(const IvpCompactLedge *l, int i)
{
    return (const IvpCompactTriangle *)((const char *)l + 16 + 16 * i);
}
static inline const IvpPolyPoint *ivp_points(const IvpCompactLedge *l)
{
    return (const IvpPolyPoint *)((const char *)l + l->c_point_offset);
}
/* the start point of an edge, and its index */
static inline int ivp_edge_point(const IvpEdge *e) { return (int)(*e & 0xffff); }
static inline const float *ivp_P(const IvpCompactLedge *l, const IvpEdge *e) { return ivp_points(l)[*e & 0xffff].k; }
/* a ledge's first edge (ledge + 0x14), the synapse edge of a new mindist */
static inline const IvpEdge *ivp_first_edge(const IvpCompactLedge *l) { return ivp_ledge_tri(l, 0)->e; }
static inline int ivp_has_children(const IvpCompactLedge *l) { return (l->flags & 3) != 0; }
static inline const IvpLedgeNode *ivp_surface_root(const IvpCompactSurface *cs)
{
    return (const IvpLedgeNode *)((const char *)cs + cs->offset_ledgetree_root);
}
