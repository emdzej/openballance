/* Constants of IVP's contact response (docs/ivp_contact.md 2.1, FS2, LS0, IM). The mindist settings block at
   0x10075db0 is computed at DLL init by 0x10015fe0((double)0.01f) on x87 and stored as f32; the values below
   are those f32 bit patterns (the collision part's share of the block is in phys_coll.h). */
#pragma once

/* ---- mindist settings (0x10075db0, 2.1) ---- */
#define IVP_SET_MIN_COLL_DIST 0.01f                      /* +0x004 0x10075db4 */
#define IVP_SET_MAX_COLL_DIST 0.01f                      /* +0x108 0x10075eb8 */
#define IVP_SET_FRICTION_DIST 0x1.47ae14p-6f             /* +0x10c 0x10075ebc = 0x3ca3d70a (0.02) */
#define IVP_SET_KEEPER_DIST 0x1.78d4fep-6f               /* +0x110 0x10075ec0 = 0x3cbc6a7f (0.023) */
#define IVP_SET_DISTANCE_KEEPERS_SAFETY 0x1.a36e2ep-14f  /* +0x118 0x10075ec8 = 0x38d1b717 (0.0001) */
#define IVP_SET_MAX_DIST_FOR_FRICTION 0x1.70a3d6p-5f     /* +0x11c 0x10075ecc = 0x3d3851eb (0.045) */
#define IVP_SET_MAX_DIST_FOR_IMPACT_SYSTEM 0x1.c28f5cp-3f   /* +0x120 0x10075ed0 = 0x3e6147ae (0.22) */
#define IVP_SET_MINDIST_CHANGE_FORCE_DIST 0x1.47ae14p-6f /* +0x128 0x10075ed8 = 0x3ca3d70a (0.02) */

/* ---- f64 constants of the friction and solver code (FS2, LS0) ---- */
#define IVP_C_1E6F 9.999999974752427e-07         /* 0x100637e0 (double)1e-6f */
#define IVP_C_1E38 1e-38                         /* 0x10063830 */
#define IVP_C_001F 0.0010000000474974513         /* 0x10063690 (double)0.001f */
#define IVP_C_01F 0.10000000149011612            /* 0x10063500 (double)0.1f */
#define IVP_C_1E19 1e-19                         /* 0x10063480 */
#define IVP_C_TEST_EPS 1.0000000116860974e-7     /* 0x3e7ad7f2a0000000: (double)1e-7f */
#define IVP_C_1E5F 9.999999747378752e-6          /* 0x10063af8 (double)1e-5f */
#define IVP_C_981F 9.81f                         /* 0x10063b1c */
#define IVP_C_MAX_CONTACTS 150                   /* 0x96 (10036d70) */
#define IVP_C_MAX_LCP_STEPS 250                  /* 0xfa (10034ab0) */
#define IVP_C_MAX_IMPACT_ROUNDS 5000             /* 10024320 */

/* ---- impact solver (IM) ---- */
#define IVP_C_APPROACH -9.999999747378752e-05    /* 0x100638c8 */
#define IVP_C_DIFF_EPS 9.999999747378752e-05     /* 0x100638c0 */
#define IVP_C_PUSHES_SPEED 0.0099999998          /* 0x10063758 */
#define IVP_C_RESCUE_FACTOR 1.2f                 /* 0x100638d0 */
#define IVP_C_DELAY_FACTOR -0.8333333f           /* 0x100638d4 */
#define IVP_C_FIXED_MASS 100000.0                /* 0x100638d8 */
#define IVP_C_ROT_RESCUE 2.5e-5                  /* 0x100638e0 */
#define IVP_C_FAR_GAP 1e20f                      /* 0x60ad78ec */
#define IVP_C_TAYLOR4 0.041666668f               /* 0x100638bc */
