/*
 * nod_lite: GameCube ISO/CISO reader implementing the subset of the nod C API used by Aurora.
 * See nod.h. All multi-byte disc fields are big-endian.
 */
#include "nod.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { H_DISC, H_PARTITION, H_FILE };

#define GCN_DISC_SIZE 1459978240ull /* standard GameCube DVD size */
#define CISO_HEADER_SIZE 0x8000u
#define CISO_MAP_SIZE (CISO_HEADER_SIZE - 8u)

typedef struct Disc {
  NodDiscStream stream;
  int refs;
  uint64_t size;
  /* CISO block map (NULL for plain ISO) */
  uint32_t ciso_block_size;
  uint32_t ciso_block_count;
  uint64_t* ciso_phys; /* physical offset per logical block, UINT64_MAX = zero-filled */
  /* partition metadata */
  uint8_t boot[0x440];
  uint8_t bi2[0x2000];
  uint8_t* apploader;
  size_t apploader_size;
  uint8_t* dol;
  size_t dol_size;
  uint8_t* fst;
  size_t fst_size;
} Disc;

struct NodHandle {
  int type;
  Disc* disc;
  uint64_t base;
  uint64_t size;
  uint64_t pos;
};

static __thread const char* s_error = "";
const char* nod_error_message(void) { return s_error; }

static uint32_t be32(const uint8_t* p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static uint32_t le32(const uint8_t* p) {
  return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0];
}

/* Read logical disc bytes, resolving the CISO block map. Returns bytes read or -1. */
static int64_t disc_read_at(Disc* d, uint64_t off, uint8_t* out, size_t len) {
  if (off >= d->size) {
    return 0;
  }
  if (off + len > d->size) {
    len = (size_t)(d->size - off);
  }
  if (!d->ciso_phys) {
    size_t total = 0;
    while (total < len) {
      int64_t n = d->stream.read_at(d->stream.user_data, off + total, out + total, len - total);
      if (n <= 0) {
        return n < 0 ? -1 : (int64_t)total;
      }
      total += (size_t)n;
    }
    return (int64_t)total;
  }
  size_t total = 0;
  while (total < len) {
    const uint64_t cur = off + total;
    const uint32_t block = (uint32_t)(cur / d->ciso_block_size);
    const uint32_t inner = (uint32_t)(cur % d->ciso_block_size);
    size_t chunk = d->ciso_block_size - inner;
    if (chunk > len - total) {
      chunk = len - total;
    }
    if (block >= d->ciso_block_count || d->ciso_phys[block] == UINT64_MAX) {
      memset(out + total, 0, chunk);
    } else {
      size_t got = 0;
      while (got < chunk) {
        int64_t n = d->stream.read_at(d->stream.user_data, d->ciso_phys[block] + inner + got, out + total + got,
                                      chunk - got);
        if (n <= 0) {
          return -1;
        }
        got += (size_t)n;
      }
    }
    total += chunk;
  }
  return (int64_t)total;
}

static uint8_t* read_blob(Disc* d, uint64_t off, size_t len) {
  uint8_t* buf = (uint8_t*)malloc(len ? len : 1);
  if (!buf) {
    return NULL;
  }
  if (disc_read_at(d, off, buf, len) != (int64_t)len) {
    free(buf);
    return NULL;
  }
  return buf;
}

static void disc_release(Disc* d) {
  if (!d || --d->refs > 0) {
    return;
  }
  if (d->stream.close) {
    d->stream.close(d->stream.user_data);
  }
  free(d->ciso_phys);
  free(d->apploader);
  free(d->dol);
  free(d->fst);
  free(d);
}

static NodHandle* new_handle(int type, Disc* d, uint64_t base, uint64_t size) {
  NodHandle* h = (NodHandle*)calloc(1, sizeof(NodHandle));
  if (!h) {
    return NULL;
  }
  h->type = type;
  h->disc = d;
  h->base = base;
  h->size = size;
  d->refs++;
  return h;
}

static NodResult load_ciso(Disc* d, int64_t phys_len) {
  uint8_t* hdr = (uint8_t*)malloc(CISO_HEADER_SIZE);
  if (!hdr) {
    return NOD_RESULT_ERR_OTHER;
  }
  if (d->stream.read_at(d->stream.user_data, 0, hdr, CISO_HEADER_SIZE) != CISO_HEADER_SIZE) {
    free(hdr);
    return NOD_RESULT_ERR_IO;
  }
  d->ciso_block_size = le32(hdr + 4);
  if (d->ciso_block_size == 0 || (d->ciso_block_size & (d->ciso_block_size - 1)) != 0) {
    free(hdr);
    s_error = "CISO: invalid block size";
    return NOD_RESULT_ERR_FORMAT;
  }
  d->ciso_block_count = CISO_MAP_SIZE;
  d->ciso_phys = (uint64_t*)malloc(sizeof(uint64_t) * CISO_MAP_SIZE);
  if (!d->ciso_phys) {
    free(hdr);
    return NOD_RESULT_ERR_OTHER;
  }
  uint64_t phys = CISO_HEADER_SIZE;
  uint32_t last = 0;
  for (uint32_t i = 0; i < CISO_MAP_SIZE; ++i) {
    if (hdr[8 + i]) {
      d->ciso_phys[i] = phys;
      phys += d->ciso_block_size;
      last = i + 1;
    } else {
      d->ciso_phys[i] = UINT64_MAX;
    }
  }
  free(hdr);
  if (phys > (uint64_t)phys_len) {
    s_error = "CISO: truncated image";
    return NOD_RESULT_ERR_FORMAT;
  }
  d->size = (uint64_t)last * d->ciso_block_size;
  if (d->size < GCN_DISC_SIZE) {
    d->size = GCN_DISC_SIZE;
  }
  return NOD_RESULT_OK;
}

static size_t dol_size(const uint8_t* h) {
  size_t end = 0x100;
  for (int i = 0; i < 18; ++i) {
    const uint32_t off = be32(h + i * 4);
    const uint32_t sz = be32(h + 0x90 + i * 4);
    if (off && sz && off + sz > end) {
      end = off + sz;
    }
  }
  return end;
}

NodResult nod_disc_open_stream(const NodDiscStream* stream, const NodDiscOptions* options, NodHandle** out) {
  (void)options;
  *out = NULL;
  if (!stream || !stream->read_at) {
    return NOD_RESULT_ERR_INVALID_HANDLE;
  }
  Disc* d = (Disc*)calloc(1, sizeof(Disc));
  if (!d) {
    return NOD_RESULT_ERR_OTHER;
  }
  d->stream = *stream;
  d->refs = 1;
  const int64_t phys_len = stream->stream_len ? stream->stream_len(stream->user_data) : -1;

  uint8_t magic[4];
  if (stream->read_at(stream->user_data, 0, magic, 4) != 4) {
    s_error = "failed to read disc header";
    disc_release(d);
    return NOD_RESULT_ERR_IO;
  }
  NodResult res = NOD_RESULT_OK;
  if (memcmp(magic, "CISO", 4) == 0) {
    res = load_ciso(d, phys_len);
  } else {
    d->size = phys_len > 0 ? (uint64_t)phys_len : GCN_DISC_SIZE;
  }
  if (res != NOD_RESULT_OK) {
    disc_release(d);
    return res;
  }

  if (disc_read_at(d, 0, d->boot, sizeof(d->boot)) != (int64_t)sizeof(d->boot) ||
      disc_read_at(d, 0x440, d->bi2, sizeof(d->bi2)) != (int64_t)sizeof(d->bi2)) {
    s_error = "failed to read boot.bin/bi2.bin";
    disc_release(d);
    return NOD_RESULT_ERR_IO;
  }
  if (be32(d->boot + 0x1C) != 0xC2339F3Du) {
    s_error = be32(d->boot + 0x18) == 0x5D1C9EA3u ? "Wii discs are not supported" : "not a GameCube disc image";
    disc_release(d);
    return NOD_RESULT_ERR_FORMAT;
  }

  uint8_t app_hdr[0x20];
  if (disc_read_at(d, 0x2440, app_hdr, sizeof(app_hdr)) == (int64_t)sizeof(app_hdr)) {
    d->apploader_size = 0x20 + be32(app_hdr + 0x14) + be32(app_hdr + 0x18);
    d->apploader = read_blob(d, 0x2440, d->apploader_size);
  }
  const uint32_t dol_off = be32(d->boot + 0x420);
  uint8_t dol_hdr[0x100];
  if (dol_off && disc_read_at(d, dol_off, dol_hdr, sizeof(dol_hdr)) == (int64_t)sizeof(dol_hdr)) {
    d->dol_size = dol_size(dol_hdr);
    d->dol = read_blob(d, dol_off, d->dol_size);
  }
  const uint32_t fst_off = be32(d->boot + 0x424);
  d->fst_size = be32(d->boot + 0x428);
  d->fst = (fst_off && d->fst_size >= 12) ? read_blob(d, fst_off, d->fst_size) : NULL;
  if (!d->fst || !d->dol || !d->apploader) {
    s_error = "failed to read apploader/DOL/FST";
    disc_release(d);
    return NOD_RESULT_ERR_FORMAT;
  }

  NodHandle* h = new_handle(H_DISC, d, 0, d->size);
  disc_release(d); /* handle holds the reference */
  if (!h) {
    return NOD_RESULT_ERR_OTHER;
  }
  *out = h;
  return NOD_RESULT_OK;
}

static int64_t file_read_at(void* ud, uint64_t off, void* out, size_t len) {
  FILE* f = (FILE*)ud;
  if (fseeko(f, (off_t)off, SEEK_SET) != 0) {
    return -1;
  }
  return (int64_t)fread(out, 1, len, f);
}
static int64_t file_len(void* ud) {
  FILE* f = (FILE*)ud;
  if (fseeko(f, 0, SEEK_END) != 0) {
    return -1;
  }
  return (int64_t)ftello(f);
}
static void file_close(void* ud) { fclose((FILE*)ud); }

NodResult nod_disc_open(const char* path, const NodDiscOptions* options, NodHandle** out) {
  FILE* f = fopen(path, "rb");
  if (!f) {
    s_error = "failed to open disc image";
    *out = NULL;
    return NOD_RESULT_ERR_IO;
  }
  const NodDiscStream s = {f, file_read_at, file_len, file_close};
  return nod_disc_open_stream(&s, options, out);
}

NodResult nod_disc_open_partition_kind(NodHandle* disc, uint32_t kind, const NodPartitionOptions* options,
                                       NodHandle** out) {
  (void)options;
  *out = NULL;
  if (!disc || disc->type != H_DISC) {
    return NOD_RESULT_ERR_INVALID_HANDLE;
  }
  if (kind != NOD_PARTITION_KIND_DATA) {
    s_error = "GameCube discs only have a data partition";
    return NOD_RESULT_ERR_NOT_FOUND;
  }
  *out = new_handle(H_PARTITION, disc->disc, 0, disc->disc->size);
  return *out ? NOD_RESULT_OK : NOD_RESULT_ERR_OTHER;
}

static uint32_t fst_count(const Disc* d) {
  const uint32_t n = be32(d->fst + 8);
  return (uint64_t)n * 12 <= d->fst_size ? n : 0;
}

NodResult nod_partition_open_file(NodHandle* partition, uint32_t fst_index, NodHandle** out) {
  *out = NULL;
  if (!partition || partition->type != H_PARTITION) {
    return NOD_RESULT_ERR_INVALID_HANDLE;
  }
  const Disc* d = partition->disc;
  if (fst_index == 0 || fst_index >= fst_count(d)) {
    return NOD_RESULT_ERR_NOT_FOUND;
  }
  const uint8_t* e = d->fst + fst_index * 12;
  if (e[0] != 0) {
    s_error = "FST entry is a directory";
    return NOD_RESULT_ERR_FORMAT;
  }
  *out = new_handle(H_FILE, partition->disc, be32(e + 4), be32(e + 8));
  return *out ? NOD_RESULT_OK : NOD_RESULT_ERR_OTHER;
}

void nod_free(NodHandle* handle) {
  if (!handle) {
    return;
  }
  disc_release(handle->disc);
  free(handle);
}

int64_t nod_read(NodHandle* h, uint8_t* buf, size_t len) {
  if (!h) {
    return -1;
  }
  if (h->pos >= h->size) {
    return 0;
  }
  if (len > h->size - h->pos) {
    len = (size_t)(h->size - h->pos);
  }
  const int64_t n = disc_read_at(h->disc, h->base + h->pos, buf, len);
  if (n > 0) {
    h->pos += (uint64_t)n;
  }
  return n;
}

int64_t nod_seek(NodHandle* h, int64_t offset, int32_t whence) {
  if (!h) {
    return -1;
  }
  int64_t base = 0;
  switch (whence) {
  case SEEK_SET: base = 0; break;
  case SEEK_CUR: base = (int64_t)h->pos; break;
  case SEEK_END: base = (int64_t)h->size; break;
  default: return -1;
  }
  const int64_t np = base + offset;
  if (np < 0) {
    return -1;
  }
  h->pos = (uint64_t)np;
  return np;
}

NodResult nod_disc_header(const NodHandle* disc, NodDiscHeader* out) {
  if (!disc || !out) {
    return NOD_RESULT_ERR_INVALID_HANDLE;
  }
  memcpy(out, disc->disc->boot, sizeof(*out));
  return NOD_RESULT_OK;
}

uint64_t nod_disc_size(const NodHandle* disc) { return disc ? disc->disc->size : 0; }

bool nod_partition_is_wii(const NodHandle* partition) {
  (void)partition;
  return false;
}

NodResult nod_partition_meta(const NodHandle* partition, NodPartitionMeta* out) {
  if (!partition || partition->type != H_PARTITION || !out) {
    return NOD_RESULT_ERR_INVALID_HANDLE;
  }
  const Disc* d = partition->disc;
  memset(out, 0, sizeof(*out));
  out->raw_boot = (NodBlob){d->boot, sizeof(d->boot)};
  out->raw_bi2 = (NodBlob){d->bi2, sizeof(d->bi2)};
  out->raw_apploader = (NodBlob){d->apploader, d->apploader_size};
  out->raw_dol = (NodBlob){d->dol, d->dol_size};
  out->raw_fst = (NodBlob){d->fst, d->fst_size};
  return NOD_RESULT_OK;
}

static int name_eq(const char* a, const char* b, size_t blen) {
  for (size_t i = 0; i < blen; ++i) {
    char ca = a[i], cb = b[i];
    if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
    if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
    if (ca != cb || ca == 0) {
      return 0;
    }
  }
  return a[blen] == 0;
}

uint32_t nod_partition_find_file(const NodHandle* partition, const char* path, NodNodeKind* out_kind,
                                 uint32_t* out_length) {
  if (!partition || partition->type != H_PARTITION || !path) {
    return UINT32_MAX;
  }
  const Disc* d = partition->disc;
  const uint32_t count = fst_count(d);
  const char* strings = (const char*)d->fst + count * 12;
  uint32_t dir = 0, idx = 1, end = count;
  while (*path == '/') path++;
  while (*path) {
    const char* sep = strchr(path, '/');
    const size_t seg = sep ? (size_t)(sep - path) : strlen(path);
    uint32_t found = UINT32_MAX;
    for (uint32_t i = idx; i < end;) {
      const uint8_t* e = d->fst + i * 12;
      const uint32_t name_off = be32(e) & 0xFFFFFF;
      if (strings + name_off < (const char*)d->fst + d->fst_size && name_eq(strings + name_off, path, seg)) {
        found = i;
        break;
      }
      i = e[0] ? be32(e + 8) : i + 1;
    }
    if (found == UINT32_MAX) {
      return UINT32_MAX;
    }
    const uint8_t* e = d->fst + found * 12;
    path += seg;
    while (*path == '/') path++;
    if (*path) {
      if (!e[0]) {
        return UINT32_MAX;
      }
      dir = found;
      idx = found + 1;
      end = be32(e + 8);
    } else {
      if (out_kind) *out_kind = e[0] ? NOD_NODE_KIND_DIRECTORY : NOD_NODE_KIND_FILE;
      if (out_length) *out_length = be32(e + 8);
      return found;
    }
  }
  (void)dir;
  return UINT32_MAX;
}

void nod_partition_iterate_fst(const NodHandle* partition, NodFstCallback callback, void* user_data) {
  if (!partition || partition->type != H_PARTITION || !callback) {
    return;
  }
  const Disc* d = partition->disc;
  const uint32_t count = fst_count(d);
  const char* strings = (const char*)d->fst + count * 12;
  const char* limit = (const char*)d->fst + d->fst_size;
  uint32_t idx = 1; /* skip root */
  while (idx < count) {
    const uint8_t* e = d->fst + idx * 12;
    const uint32_t name_off = be32(e) & 0xFFFFFF;
    const char* name = strings + name_off < limit ? strings + name_off : "<invalid>";
    /* Like nod, report the raw length field: file size, or the end index for directories. */
    idx = callback(idx, e[0] ? NOD_NODE_KIND_DIRECTORY : NOD_NODE_KIND_FILE, name, be32(e + 8), user_data);
  }
}
