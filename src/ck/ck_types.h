/* Parameter types (CKParameterManager) and parameter operations. GUIDs are the Virtools SDK's CKPGUID_*
   values; parents and string formats come from the type registration in CK2.dll (0x24058300 area) and
   the class registrations (CKClassRegisterAssociatedParameter). */
#pragma once
#include "ck.h"

typedef enum { CKT_RAW, CKT_INT, CKT_FLOAT, CKT_BOOL, CKT_STRING, CKT_OBJECT, CKT_VECTOR, CKT_VECTOR2D,
               CKT_COLOR, CKT_RECT, CKT_MESSAGE } CkTypeKind;

typedef struct {
    CkGuid guid;
    const char *name;
    CkGuid parent;            /* "derived from", {0,0} if none */
    CkTypeKind kind;
    uint32_t size;            /* default value size */
    uint32_t class_id;        /* object types: CKCID_* */
} CkParamType;

extern const CkGuid CKPGUID_NONE, CKPGUID_INT, CKPGUID_FLOAT, CKPGUID_BOOL, CKPGUID_STRING;

static inline bool ck_guid_eq(CkGuid a, CkGuid b) { return a.a == b.a && a.b == b.b; }
const CkParamType *ck_type(CkGuid g);                 /* NULL if unknown */
CkGuid ck_type_parent(CkGuid g);                     /* {0,0} if none */
bool ck_type_derives(CkGuid g, CkGuid base);        /* g == base or derived from it */
uint32_t ck_type_class(CkGuid g);                   /* object types: class id, else 0 */

/* CKParameter::GetStringValue / SetStringValue with the type's string function. to_string writes at most
   cap bytes (NUL-terminated) and returns the length. */
uint32_t ck_param_to_string(CkContext *ctx, const CkParameter *p, char *out, uint32_t cap);
void ck_param_from_string(CkContext *ctx, CkParameter *p, const char *s);

/* Operation function: output, input 1, input 2 (resolved value holders, may be NULL). */
typedef void (*CkOpFn)(CkContext *ctx, CkParameter *out, CkParameter *in1, CkParameter *in2);
/* CKParameterManager::GetOperationFunction 0x24010f6d: exact match, then parent types. */
CkOpFn ck_op_function(CkGuid op, CkGuid res, CkGuid p1, CkGuid p2);
