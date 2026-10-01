/* Virtools (CK2 2.1) composition files: .cmo / .nmo, file version 8.

   Layout (CKFile::ReadFileHeaders / ReadFileData, CK2.dll 0x2401d83c / 0x2401ded1):
     header part 0 (32 bytes): "Nemo Fi\0", crc, ck version, file version, _, write mode, header1 packed size
     header part 1 (32 bytes): data packed size, data size, manager count, object count, max id, product
                               version, product build, header1 size
     header1 (zlib if packed != size): per object {id, class id, file index, name length, name};
                                       plugin dependencies
     data    (zlib if packed != size): per manager {guid, size, state chunk}; per object {size, state chunk}

   A state chunk (CKStateChunk::ConvertFromBuffer, CK2.dll) is:
     u32 version = data version | class id << 8 | chunk version << 16 | options << 24
     u32 dword count, dwords...
     then per option bit: 1 = object ID positions, 4 = sub-chunk positions, 2 = manager ints (count + list)
   Inside the dwords, fields are grouped under identifiers: [identifier, index of the next identifier
   (0 = last), fields...]. With option 8 (CHNK_OPTION_FILE), object references are indices into the file's
   object table. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t a, b;
} CkGuid;

typedef struct {
    uint8_t data_version, class_id, chunk_version, options;
    uint32_t ndw;
    const uint32_t *dw;      /* into CkFile.data */
    uint32_t nids, nsub, nman;
    const uint32_t *ids, *sub, *man;
} CkChunk;

typedef struct {
    uint32_t id;             /* object ID as saved */
    uint32_t class_id;       /* CKCID_* */
    const char *name;        /* "" if unnamed */
    bool has_chunk;
    CkChunk chunk;
} CkFileObject;

typedef struct {
    CkGuid guid;
    bool has_chunk;
    CkChunk chunk;
} CkFileManager;

typedef struct {
    uint32_t ck_version, file_version, write_mode;
    uint32_t nobjects, nmanagers;
    CkFileObject *objects;
    CkFileManager *managers;
    uint8_t *header1, *data;  /* owned */
    char *names;              /* owned */
} CkFile;

/* Parses a whole file held in memory (copied as needed). false + message on malformed input. */
bool ck_file_parse(CkFile *f, const uint8_t *bytes, size_t len, char *err, size_t errlen);
/* Reads path through the file layer (vfs.h) and parses it. */
bool ck_file_load(CkFile *f, const char *path, char *err, size_t errlen);
void ck_file_free(CkFile *f);

/* ---- reading a chunk (the CKStateChunk read API, over the dwords) ---- */

typedef struct {
    const CkChunk *c;
    uint32_t pos;            /* next dword */
    uint32_t end;            /* end of the current identifier's fields */
    bool error;              /* set on reading past the end; reads then return 0 */
} CkReader;

void ck_reader_init(CkReader *r, const CkChunk *c);
/* Positions at the fields of identifier id. false if absent. */
bool ck_seek(CkReader *r, uint32_t id);
/* Number of dwords under the current identifier from the current position. */
uint32_t ck_remaining(const CkReader *r);
uint32_t ck_read_dword(CkReader *r);
int32_t ck_read_int(CkReader *r);
float ck_read_float(CkReader *r);
CkGuid ck_read_guid(CkReader *r);
/* Object reference: a file index with CHNK_OPTION_FILE (0xffffffff = none). */
uint32_t ck_read_object(CkReader *r);
/* Buffer: u32 byte size + data padded to dwords. Returns a pointer into the chunk and its size. */
const void *ck_read_buffer(CkReader *r, uint32_t *size);
/* String: same encoding as a buffer, NUL-terminated. Returns "" if empty. */
const char *ck_read_string(CkReader *r);
/* Sub-chunk (u32 dword size, then a serialized chunk). false if malformed. */
bool ck_read_subchunk(CkReader *r, CkChunk *out);
void ck_skip(CkReader *r, uint32_t n);
