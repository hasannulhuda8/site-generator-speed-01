// engine.c - the makefun-site 1.0.0 build, reimplemented as a freestanding
// WebAssembly module working in the byte domain. The JavaScript wrapper
// packs one case (site, templates, files) into a byte pack in linear
// memory; build() renders the whole output tree and serialises it back,
// either as a length-prefixed table (mode 0) or as a JSON object in
// JSON-escaped byte form (mode 1). No libc: memory comes from a bump
// arena over the module's own memory, reset for every case.

typedef unsigned char u8;
typedef unsigned int u32;
typedef int i32;
typedef unsigned long long u64;

// ------------------------------------------------------------ libc bits

void *memcpy(void *d, const void *s, unsigned long n) {
  // Chunked by hand: a __builtin_memcpy here would call itself.
  u8 *dd = (u8 *)d;
  const u8 *ss = (const u8 *)s;
  unsigned long i = 0;
  while (n - i >= 8) {
    __builtin_memcpy_inline(dd + i, ss + i, 8);
    i += 8;
  }
  if (n - i >= 4) {
    __builtin_memcpy_inline(dd + i, ss + i, 4);
    i += 4;
  }
  if (n - i >= 2) {
    __builtin_memcpy_inline(dd + i, ss + i, 2);
    i += 2;
  }
  if (i < n) dd[i] = ss[i];
  return d;
}
void *memmove(void *d, const void *s, unsigned long n) {
  u8 *dd = (u8 *)d;
  const u8 *ss = (const u8 *)s;
  if (dd < ss) {
    for (unsigned long i = 0; i < n; i++) dd[i] = ss[i];
  } else {
    for (unsigned long i = n; i > 0; i--) dd[i - 1] = ss[i - 1];
  }
  return d;
}
void *memset(void *d, int v, unsigned long n) {
  u8 *dd = (u8 *)d;
  u64 pat = (u8)v;
  pat |= pat << 8;
  pat |= pat << 16;
  pat |= pat << 32;
  unsigned long i = 0;
  while (n - i >= 8) {
    __builtin_memcpy_inline(dd + i, &pat, 8);
    i += 8;
  }
  while (i < n) dd[i++] = (u8)v;
  return d;
}
int memcmp(const void *a, const void *b, unsigned long n) {
  const u8 *x = (const u8 *)a, *y = (const u8 *)b;
  for (unsigned long i = 0; i < n; i++)
    if (x[i] != y[i]) return (int)x[i] - (int)y[i];
  return 0;
}

// ------------------------------------------------------------ arena

static u8 *arena_base;   // start of the bump region (after the input pack)
static u8 *arena_ptr;    // bump cursor
static u8 *arena_end;    // current memory end

static u32 mem_size(void) { return __builtin_wasm_memory_size(0) << 16; }

static void arena_init(u8 *base) {
  arena_base = base;
  arena_ptr = base;
  arena_end = (u8 *)mem_size();
}

static void *arena_alloc(u32 n, u32 align) {
  u32 p = (u32)arena_ptr;
  u32 a = (p + (align - 1)) & ~(align - 1);
  u32 need = a + n;
  if ((u8 *)need > arena_end) {
    u32 want = need - (u32)arena_end;
    u32 pages = (want + 65535) >> 16;
    if (__builtin_wasm_memory_grow(0, pages) == (u32)-1) __builtin_trap();
    arena_end = (u8 *)mem_size();
  }
  arena_ptr = (u8 *)need;
  return (void *)a;
}

static void *arena_alloc_zero(u32 n, u32 align) {
  void *p = arena_alloc(n, align);
  memset(p, 0, n);
  return p;
}

// ------------------------------------------------------------ strings

typedef struct {
  const u8 *p;
  u32 n;
} Str;

static Str str_of(const char *s) {
  Str r;
  r.p = (const u8 *)s;
  r.n = 0;
  while (s[r.n]) r.n++;
  return r;
}

static int str_eq(Str a, Str b) {
  if (a.n != b.n) return 0;
  return memcmp(a.p, b.p, a.n) == 0;
}

static int str_eq_c(Str a, const char *b) { return str_eq(a, str_of(b)); }

// Code-unit (byte) order, matching JS string comparison for BMP text.
static int str_cmp(Str a, Str b) {
  u32 m = a.n < b.n ? a.n : b.n;
  int c = memcmp(a.p, b.p, m);
  if (c != 0) return c;
  return a.n < b.n ? -1 : a.n > b.n ? 1 : 0;
}

static int str_starts(Str a, const char *pre) {
  Str b = str_of(pre);
  if (a.n < b.n) return 0;
  return memcmp(a.p, b.p, b.n) == 0;
}

static int str_ends(Str a, const char *suf) {
  Str b = str_of(suf);
  if (a.n < b.n) return 0;
  return memcmp(a.p + (a.n - b.n), b.p, b.n) == 0;
}

// ------------------------------------------------------------ byte buffer

typedef struct {
  u8 *p;
  u32 n;
  u32 cap;
} Buf;

static void buf_init(Buf *b) {
  b->cap = 256;
  b->p = (u8 *)arena_alloc(b->cap, 8);
  b->n = 0;
}

static void buf_reserve(Buf *b, u32 extra) {
  if (b->n + extra <= b->cap) return;
  u32 cap = b->cap;
  while (cap < b->n + extra) cap *= 2;
  u8 *np = (u8 *)arena_alloc(cap, 8);
  memcpy(np, b->p, b->n);
  b->p = np;
  b->cap = cap;
}

static void buf_put(Buf *b, const void *src, u32 n) {
  buf_reserve(b, n);
  memcpy(b->p + b->n, src, n);
  b->n += n;
}

static void buf_str(Buf *b, Str s) { buf_put(b, s.p, s.n); }
static void buf_c(Buf *b, const char *s) { buf_put(b, s, str_of(s).n); }
static void buf_ch(Buf *b, u8 c) {
  buf_reserve(b, 1);
  b->p[b->n++] = c;
}

static void buf_u32dec(Buf *b, u32 v) {
  char tmp[10];
  int k = 0;
  if (v == 0) {
    buf_ch(b, '0');
    return;
  }
  while (v > 0) {
    tmp[k++] = (char)('0' + (v % 10));
    v /= 10;
  }
  while (k > 0) buf_ch(b, (u8)tmp[--k]);
}

static Str buf_str_view(Buf *b) {
  Str s;
  s.p = b->p;
  s.n = b->n;
  return s;
}

// ------------------------------------------------------------ hash map
// Open addressing, Str keys -> i32 values, with insertion order kept in a
// side array. Sized generously and never shrunk; one map per use, built
// on the arena and dropped with it.

typedef struct {
  Str key;
  i32 val;
  int used;
} MapSlot;

typedef struct {
  MapSlot *slots;
  u32 mask;
  Str *order;   // keys in insertion order
  i32 *ovals;   // values in insertion order
  u32 count;
  u32 order_cap;
} Map;

static u32 hash_str(Str s) {
  u32 h = 2166136261u;
  for (u32 i = 0; i < s.n; i++) {
    h ^= s.p[i];
    h *= 16777619u;
  }
  return h;
}

static void map_init(Map *m, u32 pow2) {
  m->slots = (MapSlot *)arena_alloc_zero(sizeof(MapSlot) << pow2, 8);
  m->mask = (1u << pow2) - 1;
  m->order_cap = 64;
  m->order = (Str *)arena_alloc(sizeof(Str) * m->order_cap, 8);
  m->ovals = (i32 *)arena_alloc(sizeof(i32) * m->order_cap, 4);
  m->count = 0;
}

// Find the slot for `key`; when `create` and absent, insert with val -1.
static MapSlot *map_slot(Map *m, Str key, int create) {
  u32 i = hash_str(key) & m->mask;
  while (1) {
    MapSlot *s = &m->slots[i];
    if (!s->used) {
      if (!create) return 0;
      s->used = 1;
      s->key = key;
      s->val = -1;
      if (m->count == m->order_cap) {
        u32 ncap = m->order_cap * 2;
        Str *no = (Str *)arena_alloc(sizeof(Str) * ncap, 8);
        i32 *nv = (i32 *)arena_alloc(sizeof(i32) * ncap, 4);
        memcpy(no, m->order, sizeof(Str) * m->count);
        memcpy(nv, m->ovals, sizeof(i32) * m->count);
        m->order = no;
        m->ovals = nv;
        m->order_cap = ncap;
      }
      m->order[m->count] = key;
      m->ovals[m->count] = -1;
      m->count++;
      return s;
    }
    if (str_eq(s->key, key)) return s;
    i = (i + 1) & m->mask;
  }
}

static i32 map_get(Map *m, Str key) {
  MapSlot *s = map_slot(m, key, 0);
  return s ? s->val : -1;
}

static void map_set(Map *m, Str key, i32 val) {
  MapSlot *s = map_slot(m, key, 1);
  s->val = val;
  // keep the insertion-order value copy in sync
  for (u32 i = 0; i < m->count; i++)
    if (str_eq(m->order[i], key)) {
      m->ovals[i] = val;
      break;
    }
}

// ------------------------------------------------------------ utf-8

// Decode the codepoint at byte i; returns its value and sets *len.
// Invalid bytes decode as their own value with length 1 (inputs are
// valid UTF-8 produced by the wrapper's TextEncoder).
static u32 utf8_decode(const u8 *p, u32 n, u32 i, u32 *len) {
  u8 c = p[i];
  if (c < 0x80) {
    *len = 1;
    return c;
  }
  if ((c & 0xE0) == 0xC0 && i + 1 < n) {
    *len = 2;
    return ((u32)(c & 0x1F) << 6) | (u32)(p[i + 1] & 0x3F);
  }
  if ((c & 0xF0) == 0xE0 && i + 2 < n) {
    *len = 3;
    return ((u32)(c & 0x0F) << 12) | ((u32)(p[i + 1] & 0x3F) << 6) |
           (u32)(p[i + 2] & 0x3F);
  }
  if ((c & 0xF8) == 0xF0 && i + 3 < n) {
    *len = 4;
    return ((u32)(c & 0x07) << 18) | ((u32)(p[i + 1] & 0x3F) << 12) |
           ((u32)(p[i + 2] & 0x3F) << 6) | (u32)(p[i + 3] & 0x3F);
  }
  *len = 1;
  return c;
}

static u32 utf8_encode(u8 *out, u32 cp) {
  if (cp < 0x80) {
    out[0] = (u8)cp;
    return 1;
  }
  if (cp < 0x800) {
    out[0] = (u8)(0xC0 | (cp >> 6));
    out[1] = (u8)(0x80 | (cp & 0x3F));
    return 2;
  }
  if (cp < 0x10000) {
    out[0] = (u8)(0xE0 | (cp >> 12));
    out[1] = (u8)(0x80 | ((cp >> 6) & 0x3F));
    out[2] = (u8)(0x80 | (cp & 0x3F));
    return 3;
  }
  out[0] = (u8)(0xF0 | (cp >> 18));
  out[1] = (u8)(0x80 | ((cp >> 12) & 0x3F));
  out[2] = (u8)(0x80 | ((cp >> 6) & 0x3F));
  out[3] = (u8)(0x80 | (cp & 0x3F));
  return 4;
}

// Whether a codepoint is JavaScript \s whitespace (the BMP set).
static int is_space_cp(u32 cp) {
  if (cp >= 9 && cp <= 13) return 1;
  if (cp == 32 || cp == 0xA0 || cp == 0x1680) return 1;
  if (cp >= 0x2000 && cp <= 0x200A) return 1;
  return cp == 0x2028 || cp == 0x2029 || cp == 0x202F || cp == 0x205F ||
         cp == 0x3000 || cp == 0xFEFF;
}

// ------------------------------------------------------------ escaping

#include <wasm_simd128.h>

// The offset of the first HTML-special byte in s[from..n), or n.
static u32 html_special_at(Str s, u32 from) {
  u32 i = from;
  while (i + 16 <= s.n) {
    v128_t v = wasm_v128_load(s.p + i);
    v128_t hit = wasm_i8x16_eq(v, wasm_i8x16_splat('&'));
    hit = wasm_v128_or(hit, wasm_i8x16_eq(v, wasm_i8x16_splat('<')));
    hit = wasm_v128_or(hit, wasm_i8x16_eq(v, wasm_i8x16_splat('>')));
    hit = wasm_v128_or(hit, wasm_i8x16_eq(v, wasm_i8x16_splat('"')));
    hit = wasm_v128_or(hit, wasm_i8x16_eq(v, wasm_i8x16_splat('\'')));
    u32 mask = wasm_i8x16_bitmask(hit);
    if (mask) return i + (u32)__builtin_ctz(mask);
    i += 16;
  }
  while (i < s.n) {
    u8 c = s.p[i];
    if (c == '&' || c == '<' || c == '>' || c == '"' || c == '\'') return i;
    i++;
  }
  return s.n;
}

// HTML-escape into b: & < > " ' become entities, in bulk runs.
static void buf_escape_html(Buf *b, Str s) {
  u32 i = 0;
  while (i < s.n) {
    u32 j = html_special_at(s, i);
    if (j > i) buf_put(b, s.p + i, j - i);
    if (j >= s.n) break;
    u8 c = s.p[j];
    if (c == '&') buf_c(b, "&amp;");
    else if (c == '<') buf_c(b, "&lt;");
    else if (c == '>') buf_c(b, "&gt;");
    else if (c == '"') buf_c(b, "&quot;");
    else buf_c(b, "&#39;");
    i = j + 1;
  }
}

// JSON-string-escape into b (the string body only, no quotes), matching
// JSON.stringify: " and \ escaped, the usual short escapes, other
// control characters as \u00XX, everything else raw (UTF-8 passes
// through, as JSON.stringify leaves non-ASCII unescaped).
// The offset of the first JSON-special byte in s[from..n), or n:
// '"' or '\\' or a control character below 0x20 (unsigned compare via
// the xor-0x80 trick, so UTF-8 high bytes never match).
static u32 json_out_special_at(Str s, u32 from) {
  u32 i = from;
  v128_t bias = wasm_i8x16_splat((char)0x80);
  v128_t limit = wasm_i8x16_splat((char)(0x20 ^ 0x80));
  while (i + 16 <= s.n) {
    v128_t v = wasm_v128_load(s.p + i);
    v128_t hit = wasm_i8x16_eq(v, wasm_i8x16_splat('"'));
    hit = wasm_v128_or(hit, wasm_i8x16_eq(v, wasm_i8x16_splat('\\')));
    v128_t ctrl = wasm_i8x16_lt(wasm_v128_xor(v, bias), limit);
    hit = wasm_v128_or(hit, ctrl);
    u32 mask = wasm_i8x16_bitmask(hit);
    if (mask) return i + (u32)__builtin_ctz(mask);
    i += 16;
  }
  while (i < s.n) {
    u8 c = s.p[i];
    if (c == '"' || c == '\\' || c < 0x20) return i;
    i++;
  }
  return s.n;
}

static void buf_escape_json(Buf *b, Str s) {
  static const char *hexd = "0123456789abcdef";
  u32 i = 0, run = 0;
  while (i < s.n) {
    u8 c = s.p[i];
    if (c != '"' && c != '\\' && c >= 0x20) {
      u32 j = json_out_special_at(s, i);
      i = j;
      continue;
    }
    const char *esc = 0;
    char uesc[6];
    if (c == '"') esc = "\\\"";
    else if (c == '\\') esc = "\\\\";
    else if (c == '\n') esc = "\\n";
    else if (c == '\r') esc = "\\r";
    else if (c == '\t') esc = "\\t";
    else if (c == '\b') esc = "\\b";
    else if (c == '\f') esc = "\\f";
    else if (c < 0x20) {
      uesc[0] = '\\';
      uesc[1] = 'u';
      uesc[2] = '0';
      uesc[3] = '0';
      uesc[4] = hexd[c >> 4];
      uesc[5] = hexd[c & 15];
      if (i > run) buf_put(b, s.p + run, i - run);
      buf_put(b, uesc, 6);
      i++;
      run = i;
      continue;
    }
    if (esc) {
      if (i > run) buf_put(b, s.p + run, i - run);
      buf_c(b, esc);
      i++;
      run = i;
    } else {
      i++;
    }
  }
  if (s.n > run) buf_put(b, s.p + run, s.n - run);
}

// ------------------------------------------------------------ slug

// Whether a codepoint is a Unicode mark (the common \p{M} blocks); such
// codepoints vanish between NFKD and the slug loop in the reference.
static int is_mark_cp(u32 cp) {
  if (cp >= 0x300 && cp <= 0x36F) return 1;
  if (cp >= 0x483 && cp <= 0x489) return 1;
  if (cp >= 0x591 && cp <= 0x5BD) return 1;
  if (cp == 0x5BF) return 1;
  if (cp >= 0x5C1 && cp <= 0x5C2) return 1;
  if (cp >= 0x5C4 && cp <= 0x5C7) return 1;
  if (cp >= 0x610 && cp <= 0x61A) return 1;
  if (cp >= 0x64B && cp <= 0x65F) return 1;
  if (cp == 0x670) return 1;
  if (cp >= 0x6D6 && cp <= 0x6DC) return 1;
  if (cp >= 0x6DF && cp <= 0x6E4) return 1;
  if (cp >= 0x6E7 && cp <= 0x6E8) return 1;
  if (cp >= 0x6EA && cp <= 0x6ED) return 1;
  if (cp >= 0x711 && cp <= 0x730) return 1;
  if (cp >= 0x74A && cp <= 0x7B0) return 1;
  if (cp >= 0x7FD && cp <= 0x7FD) return 1;
  if (cp >= 0x81B && cp <= 0x824) return 1;
  if (cp >= 0x898 && cp <= 0x902) return 1;
  if (cp >= 0x93A && cp <= 0x954) return 1;
  if (cp >= 0x962 && cp <= 0x963) return 1;
  if (cp >= 0x981 && cp <= 0x9C4) return 1;
  if (cp >= 0x1AB0 && cp <= 0x1AFF) return 1;
  if (cp >= 0x1DC0 && cp <= 0x1DFF) return 1;
  if (cp >= 0x20D0 && cp <= 0x20FF) return 1;
  if (cp >= 0xFE00 && cp <= 0xFE0F) return 1;
  if (cp >= 0xFE20 && cp <= 0xFE2F) return 1;
  return 0;
}

// The non-mark codepoints of cp's NFKD decomposition. The table was
// generated from the reference runtime's own normaliser over the Latin
// ranges (plus ligatures and fullwidth forms): every codepoint whose
// decomposition can contribute a kept slug character or a separator
// that matters. Anything else decomposes to characters that are all
// dropped by the slug loop anyway, which is indistinguishable from the
// codepoint itself being dropped, so the default is the codepoint.
typedef struct {
  u32 cp;
  u32 out[4];
  u32 n;
} NfkdEntry;

static const NfkdEntry NFKD_TABLE[] = {
  {0xA0, {0x20}, 1},
  {0xA8, {0x20}, 1},
  {0xAA, {0x61}, 1},
  {0xAF, {0x20}, 1},
  {0xB2, {0x32}, 1},
  {0xB3, {0x33}, 1},
  {0xB4, {0x20}, 1},
  {0xB5, {0x3BC}, 1},
  {0xB8, {0x20}, 1},
  {0xB9, {0x31}, 1},
  {0xBA, {0x6F}, 1},
  {0xBC, {0x31, 0x2044, 0x34}, 3},
  {0xBD, {0x31, 0x2044, 0x32}, 3},
  {0xBE, {0x33, 0x2044, 0x34}, 3},
  {0xC0, {0x41}, 1},
  {0xC1, {0x41}, 1},
  {0xC2, {0x41}, 1},
  {0xC3, {0x41}, 1},
  {0xC4, {0x41}, 1},
  {0xC5, {0x41}, 1},
  {0xC7, {0x43}, 1},
  {0xC8, {0x45}, 1},
  {0xC9, {0x45}, 1},
  {0xCA, {0x45}, 1},
  {0xCB, {0x45}, 1},
  {0xCC, {0x49}, 1},
  {0xCD, {0x49}, 1},
  {0xCE, {0x49}, 1},
  {0xCF, {0x49}, 1},
  {0xD1, {0x4E}, 1},
  {0xD2, {0x4F}, 1},
  {0xD3, {0x4F}, 1},
  {0xD4, {0x4F}, 1},
  {0xD5, {0x4F}, 1},
  {0xD6, {0x4F}, 1},
  {0xD9, {0x55}, 1},
  {0xDA, {0x55}, 1},
  {0xDB, {0x55}, 1},
  {0xDC, {0x55}, 1},
  {0xDD, {0x59}, 1},
  {0xE0, {0x61}, 1},
  {0xE1, {0x61}, 1},
  {0xE2, {0x61}, 1},
  {0xE3, {0x61}, 1},
  {0xE4, {0x61}, 1},
  {0xE5, {0x61}, 1},
  {0xE7, {0x63}, 1},
  {0xE8, {0x65}, 1},
  {0xE9, {0x65}, 1},
  {0xEA, {0x65}, 1},
  {0xEB, {0x65}, 1},
  {0xEC, {0x69}, 1},
  {0xED, {0x69}, 1},
  {0xEE, {0x69}, 1},
  {0xEF, {0x69}, 1},
  {0xF1, {0x6E}, 1},
  {0xF2, {0x6F}, 1},
  {0xF3, {0x6F}, 1},
  {0xF4, {0x6F}, 1},
  {0xF5, {0x6F}, 1},
  {0xF6, {0x6F}, 1},
  {0xF9, {0x75}, 1},
  {0xFA, {0x75}, 1},
  {0xFB, {0x75}, 1},
  {0xFC, {0x75}, 1},
  {0xFD, {0x79}, 1},
  {0xFF, {0x79}, 1},
  {0x100, {0x41}, 1},
  {0x101, {0x61}, 1},
  {0x102, {0x41}, 1},
  {0x103, {0x61}, 1},
  {0x104, {0x41}, 1},
  {0x105, {0x61}, 1},
  {0x106, {0x43}, 1},
  {0x107, {0x63}, 1},
  {0x108, {0x43}, 1},
  {0x109, {0x63}, 1},
  {0x10A, {0x43}, 1},
  {0x10B, {0x63}, 1},
  {0x10C, {0x43}, 1},
  {0x10D, {0x63}, 1},
  {0x10E, {0x44}, 1},
  {0x10F, {0x64}, 1},
  {0x112, {0x45}, 1},
  {0x113, {0x65}, 1},
  {0x114, {0x45}, 1},
  {0x115, {0x65}, 1},
  {0x116, {0x45}, 1},
  {0x117, {0x65}, 1},
  {0x118, {0x45}, 1},
  {0x119, {0x65}, 1},
  {0x11A, {0x45}, 1},
  {0x11B, {0x65}, 1},
  {0x11C, {0x47}, 1},
  {0x11D, {0x67}, 1},
  {0x11E, {0x47}, 1},
  {0x11F, {0x67}, 1},
  {0x120, {0x47}, 1},
  {0x121, {0x67}, 1},
  {0x122, {0x47}, 1},
  {0x123, {0x67}, 1},
  {0x124, {0x48}, 1},
  {0x125, {0x68}, 1},
  {0x128, {0x49}, 1},
  {0x129, {0x69}, 1},
  {0x12A, {0x49}, 1},
  {0x12B, {0x69}, 1},
  {0x12C, {0x49}, 1},
  {0x12D, {0x69}, 1},
  {0x12E, {0x49}, 1},
  {0x12F, {0x69}, 1},
  {0x130, {0x49}, 1},
  {0x132, {0x49, 0x4A}, 2},
  {0x133, {0x69, 0x6A}, 2},
  {0x134, {0x4A}, 1},
  {0x135, {0x6A}, 1},
  {0x136, {0x4B}, 1},
  {0x137, {0x6B}, 1},
  {0x139, {0x4C}, 1},
  {0x13A, {0x6C}, 1},
  {0x13B, {0x4C}, 1},
  {0x13C, {0x6C}, 1},
  {0x13D, {0x4C}, 1},
  {0x13E, {0x6C}, 1},
  {0x13F, {0x4C, 0xB7}, 2},
  {0x140, {0x6C, 0xB7}, 2},
  {0x143, {0x4E}, 1},
  {0x144, {0x6E}, 1},
  {0x145, {0x4E}, 1},
  {0x146, {0x6E}, 1},
  {0x147, {0x4E}, 1},
  {0x148, {0x6E}, 1},
  {0x149, {0x2BC, 0x6E}, 2},
  {0x14C, {0x4F}, 1},
  {0x14D, {0x6F}, 1},
  {0x14E, {0x4F}, 1},
  {0x14F, {0x6F}, 1},
  {0x150, {0x4F}, 1},
  {0x151, {0x6F}, 1},
  {0x154, {0x52}, 1},
  {0x155, {0x72}, 1},
  {0x156, {0x52}, 1},
  {0x157, {0x72}, 1},
  {0x158, {0x52}, 1},
  {0x159, {0x72}, 1},
  {0x15A, {0x53}, 1},
  {0x15B, {0x73}, 1},
  {0x15C, {0x53}, 1},
  {0x15D, {0x73}, 1},
  {0x15E, {0x53}, 1},
  {0x15F, {0x73}, 1},
  {0x160, {0x53}, 1},
  {0x161, {0x73}, 1},
  {0x162, {0x54}, 1},
  {0x163, {0x74}, 1},
  {0x164, {0x54}, 1},
  {0x165, {0x74}, 1},
  {0x168, {0x55}, 1},
  {0x169, {0x75}, 1},
  {0x16A, {0x55}, 1},
  {0x16B, {0x75}, 1},
  {0x16C, {0x55}, 1},
  {0x16D, {0x75}, 1},
  {0x16E, {0x55}, 1},
  {0x16F, {0x75}, 1},
  {0x170, {0x55}, 1},
  {0x171, {0x75}, 1},
  {0x172, {0x55}, 1},
  {0x173, {0x75}, 1},
  {0x174, {0x57}, 1},
  {0x175, {0x77}, 1},
  {0x176, {0x59}, 1},
  {0x177, {0x79}, 1},
  {0x178, {0x59}, 1},
  {0x179, {0x5A}, 1},
  {0x17A, {0x7A}, 1},
  {0x17B, {0x5A}, 1},
  {0x17C, {0x7A}, 1},
  {0x17D, {0x5A}, 1},
  {0x17E, {0x7A}, 1},
  {0x17F, {0x73}, 1},
  {0x1A0, {0x4F}, 1},
  {0x1A1, {0x6F}, 1},
  {0x1AF, {0x55}, 1},
  {0x1B0, {0x75}, 1},
  {0x1C4, {0x44, 0x5A}, 2},
  {0x1C5, {0x44, 0x7A}, 2},
  {0x1C6, {0x64, 0x7A}, 2},
  {0x1C7, {0x4C, 0x4A}, 2},
  {0x1C8, {0x4C, 0x6A}, 2},
  {0x1C9, {0x6C, 0x6A}, 2},
  {0x1CA, {0x4E, 0x4A}, 2},
  {0x1CB, {0x4E, 0x6A}, 2},
  {0x1CC, {0x6E, 0x6A}, 2},
  {0x1CD, {0x41}, 1},
  {0x1CE, {0x61}, 1},
  {0x1CF, {0x49}, 1},
  {0x1D0, {0x69}, 1},
  {0x1D1, {0x4F}, 1},
  {0x1D2, {0x6F}, 1},
  {0x1D3, {0x55}, 1},
  {0x1D4, {0x75}, 1},
  {0x1D5, {0x55}, 1},
  {0x1D6, {0x75}, 1},
  {0x1D7, {0x55}, 1},
  {0x1D8, {0x75}, 1},
  {0x1D9, {0x55}, 1},
  {0x1DA, {0x75}, 1},
  {0x1DB, {0x55}, 1},
  {0x1DC, {0x75}, 1},
  {0x1DE, {0x41}, 1},
  {0x1DF, {0x61}, 1},
  {0x1E0, {0x41}, 1},
  {0x1E1, {0x61}, 1},
  {0x1E2, {0xC6}, 1},
  {0x1E3, {0xE6}, 1},
  {0x1E6, {0x47}, 1},
  {0x1E7, {0x67}, 1},
  {0x1E8, {0x4B}, 1},
  {0x1E9, {0x6B}, 1},
  {0x1EA, {0x4F}, 1},
  {0x1EB, {0x6F}, 1},
  {0x1EC, {0x4F}, 1},
  {0x1ED, {0x6F}, 1},
  {0x1EE, {0x1B7}, 1},
  {0x1EF, {0x292}, 1},
  {0x1F0, {0x6A}, 1},
  {0x1F1, {0x44, 0x5A}, 2},
  {0x1F2, {0x44, 0x7A}, 2},
  {0x1F3, {0x64, 0x7A}, 2},
  {0x1F4, {0x47}, 1},
  {0x1F5, {0x67}, 1},
  {0x1F8, {0x4E}, 1},
  {0x1F9, {0x6E}, 1},
  {0x1FA, {0x41}, 1},
  {0x1FB, {0x61}, 1},
  {0x1FC, {0xC6}, 1},
  {0x1FD, {0xE6}, 1},
  {0x1FE, {0xD8}, 1},
  {0x1FF, {0xF8}, 1},
  {0x200, {0x41}, 1},
  {0x201, {0x61}, 1},
  {0x202, {0x41}, 1},
  {0x203, {0x61}, 1},
  {0x204, {0x45}, 1},
  {0x205, {0x65}, 1},
  {0x206, {0x45}, 1},
  {0x207, {0x65}, 1},
  {0x208, {0x49}, 1},
  {0x209, {0x69}, 1},
  {0x20A, {0x49}, 1},
  {0x20B, {0x69}, 1},
  {0x20C, {0x4F}, 1},
  {0x20D, {0x6F}, 1},
  {0x20E, {0x4F}, 1},
  {0x20F, {0x6F}, 1},
  {0x210, {0x52}, 1},
  {0x211, {0x72}, 1},
  {0x212, {0x52}, 1},
  {0x213, {0x72}, 1},
  {0x214, {0x55}, 1},
  {0x215, {0x75}, 1},
  {0x216, {0x55}, 1},
  {0x217, {0x75}, 1},
  {0x218, {0x53}, 1},
  {0x219, {0x73}, 1},
  {0x21A, {0x54}, 1},
  {0x21B, {0x74}, 1},
  {0x21E, {0x48}, 1},
  {0x21F, {0x68}, 1},
  {0x226, {0x41}, 1},
  {0x227, {0x61}, 1},
  {0x228, {0x45}, 1},
  {0x229, {0x65}, 1},
  {0x22A, {0x4F}, 1},
  {0x22B, {0x6F}, 1},
  {0x22C, {0x4F}, 1},
  {0x22D, {0x6F}, 1},
  {0x22E, {0x4F}, 1},
  {0x22F, {0x6F}, 1},
  {0x230, {0x4F}, 1},
  {0x231, {0x6F}, 1},
  {0x232, {0x59}, 1},
  {0x233, {0x79}, 1},
  {0x1E00, {0x41}, 1},
  {0x1E01, {0x61}, 1},
  {0x1E02, {0x42}, 1},
  {0x1E03, {0x62}, 1},
  {0x1E04, {0x42}, 1},
  {0x1E05, {0x62}, 1},
  {0x1E06, {0x42}, 1},
  {0x1E07, {0x62}, 1},
  {0x1E08, {0x43}, 1},
  {0x1E09, {0x63}, 1},
  {0x1E0A, {0x44}, 1},
  {0x1E0B, {0x64}, 1},
  {0x1E0C, {0x44}, 1},
  {0x1E0D, {0x64}, 1},
  {0x1E0E, {0x44}, 1},
  {0x1E0F, {0x64}, 1},
  {0x1E10, {0x44}, 1},
  {0x1E11, {0x64}, 1},
  {0x1E12, {0x44}, 1},
  {0x1E13, {0x64}, 1},
  {0x1E14, {0x45}, 1},
  {0x1E15, {0x65}, 1},
  {0x1E16, {0x45}, 1},
  {0x1E17, {0x65}, 1},
  {0x1E18, {0x45}, 1},
  {0x1E19, {0x65}, 1},
  {0x1E1A, {0x45}, 1},
  {0x1E1B, {0x65}, 1},
  {0x1E1C, {0x45}, 1},
  {0x1E1D, {0x65}, 1},
  {0x1E1E, {0x46}, 1},
  {0x1E1F, {0x66}, 1},
  {0x1E20, {0x47}, 1},
  {0x1E21, {0x67}, 1},
  {0x1E22, {0x48}, 1},
  {0x1E23, {0x68}, 1},
  {0x1E24, {0x48}, 1},
  {0x1E25, {0x68}, 1},
  {0x1E26, {0x48}, 1},
  {0x1E27, {0x68}, 1},
  {0x1E28, {0x48}, 1},
  {0x1E29, {0x68}, 1},
  {0x1E2A, {0x48}, 1},
  {0x1E2B, {0x68}, 1},
  {0x1E2C, {0x49}, 1},
  {0x1E2D, {0x69}, 1},
  {0x1E2E, {0x49}, 1},
  {0x1E2F, {0x69}, 1},
  {0x1E30, {0x4B}, 1},
  {0x1E31, {0x6B}, 1},
  {0x1E32, {0x4B}, 1},
  {0x1E33, {0x6B}, 1},
  {0x1E34, {0x4B}, 1},
  {0x1E35, {0x6B}, 1},
  {0x1E36, {0x4C}, 1},
  {0x1E37, {0x6C}, 1},
  {0x1E38, {0x4C}, 1},
  {0x1E39, {0x6C}, 1},
  {0x1E3A, {0x4C}, 1},
  {0x1E3B, {0x6C}, 1},
  {0x1E3C, {0x4C}, 1},
  {0x1E3D, {0x6C}, 1},
  {0x1E3E, {0x4D}, 1},
  {0x1E3F, {0x6D}, 1},
  {0x1E40, {0x4D}, 1},
  {0x1E41, {0x6D}, 1},
  {0x1E42, {0x4D}, 1},
  {0x1E43, {0x6D}, 1},
  {0x1E44, {0x4E}, 1},
  {0x1E45, {0x6E}, 1},
  {0x1E46, {0x4E}, 1},
  {0x1E47, {0x6E}, 1},
  {0x1E48, {0x4E}, 1},
  {0x1E49, {0x6E}, 1},
  {0x1E4A, {0x4E}, 1},
  {0x1E4B, {0x6E}, 1},
  {0x1E4C, {0x4F}, 1},
  {0x1E4D, {0x6F}, 1},
  {0x1E4E, {0x4F}, 1},
  {0x1E4F, {0x6F}, 1},
  {0x1E50, {0x4F}, 1},
  {0x1E51, {0x6F}, 1},
  {0x1E52, {0x4F}, 1},
  {0x1E53, {0x6F}, 1},
  {0x1E54, {0x50}, 1},
  {0x1E55, {0x70}, 1},
  {0x1E56, {0x50}, 1},
  {0x1E57, {0x70}, 1},
  {0x1E58, {0x52}, 1},
  {0x1E59, {0x72}, 1},
  {0x1E5A, {0x52}, 1},
  {0x1E5B, {0x72}, 1},
  {0x1E5C, {0x52}, 1},
  {0x1E5D, {0x72}, 1},
  {0x1E5E, {0x52}, 1},
  {0x1E5F, {0x72}, 1},
  {0x1E60, {0x53}, 1},
  {0x1E61, {0x73}, 1},
  {0x1E62, {0x53}, 1},
  {0x1E63, {0x73}, 1},
  {0x1E64, {0x53}, 1},
  {0x1E65, {0x73}, 1},
  {0x1E66, {0x53}, 1},
  {0x1E67, {0x73}, 1},
  {0x1E68, {0x53}, 1},
  {0x1E69, {0x73}, 1},
  {0x1E6A, {0x54}, 1},
  {0x1E6B, {0x74}, 1},
  {0x1E6C, {0x54}, 1},
  {0x1E6D, {0x74}, 1},
  {0x1E6E, {0x54}, 1},
  {0x1E6F, {0x74}, 1},
  {0x1E70, {0x54}, 1},
  {0x1E71, {0x74}, 1},
  {0x1E72, {0x55}, 1},
  {0x1E73, {0x75}, 1},
  {0x1E74, {0x55}, 1},
  {0x1E75, {0x75}, 1},
  {0x1E76, {0x55}, 1},
  {0x1E77, {0x75}, 1},
  {0x1E78, {0x55}, 1},
  {0x1E79, {0x75}, 1},
  {0x1E7A, {0x55}, 1},
  {0x1E7B, {0x75}, 1},
  {0x1E7C, {0x56}, 1},
  {0x1E7D, {0x76}, 1},
  {0x1E7E, {0x56}, 1},
  {0x1E7F, {0x76}, 1},
  {0x1E80, {0x57}, 1},
  {0x1E81, {0x77}, 1},
  {0x1E82, {0x57}, 1},
  {0x1E83, {0x77}, 1},
  {0x1E84, {0x57}, 1},
  {0x1E85, {0x77}, 1},
  {0x1E86, {0x57}, 1},
  {0x1E87, {0x77}, 1},
  {0x1E88, {0x57}, 1},
  {0x1E89, {0x77}, 1},
  {0x1E8A, {0x58}, 1},
  {0x1E8B, {0x78}, 1},
  {0x1E8C, {0x58}, 1},
  {0x1E8D, {0x78}, 1},
  {0x1E8E, {0x59}, 1},
  {0x1E8F, {0x79}, 1},
  {0x1E90, {0x5A}, 1},
  {0x1E91, {0x7A}, 1},
  {0x1E92, {0x5A}, 1},
  {0x1E93, {0x7A}, 1},
  {0x1E94, {0x5A}, 1},
  {0x1E95, {0x7A}, 1},
  {0x1E96, {0x68}, 1},
  {0x1E97, {0x74}, 1},
  {0x1E98, {0x77}, 1},
  {0x1E99, {0x79}, 1},
  {0x1E9A, {0x61, 0x2BE}, 2},
  {0x1E9B, {0x73}, 1},
  {0x1EA0, {0x41}, 1},
  {0x1EA1, {0x61}, 1},
  {0x1EA2, {0x41}, 1},
  {0x1EA3, {0x61}, 1},
  {0x1EA4, {0x41}, 1},
  {0x1EA5, {0x61}, 1},
  {0x1EA6, {0x41}, 1},
  {0x1EA7, {0x61}, 1},
  {0x1EA8, {0x41}, 1},
  {0x1EA9, {0x61}, 1},
  {0x1EAA, {0x41}, 1},
  {0x1EAB, {0x61}, 1},
  {0x1EAC, {0x41}, 1},
  {0x1EAD, {0x61}, 1},
  {0x1EAE, {0x41}, 1},
  {0x1EAF, {0x61}, 1},
  {0x1EB0, {0x41}, 1},
  {0x1EB1, {0x61}, 1},
  {0x1EB2, {0x41}, 1},
  {0x1EB3, {0x61}, 1},
  {0x1EB4, {0x41}, 1},
  {0x1EB5, {0x61}, 1},
  {0x1EB6, {0x41}, 1},
  {0x1EB7, {0x61}, 1},
  {0x1EB8, {0x45}, 1},
  {0x1EB9, {0x65}, 1},
  {0x1EBA, {0x45}, 1},
  {0x1EBB, {0x65}, 1},
  {0x1EBC, {0x45}, 1},
  {0x1EBD, {0x65}, 1},
  {0x1EBE, {0x45}, 1},
  {0x1EBF, {0x65}, 1},
  {0x1EC0, {0x45}, 1},
  {0x1EC1, {0x65}, 1},
  {0x1EC2, {0x45}, 1},
  {0x1EC3, {0x65}, 1},
  {0x1EC4, {0x45}, 1},
  {0x1EC5, {0x65}, 1},
  {0x1EC6, {0x45}, 1},
  {0x1EC7, {0x65}, 1},
  {0x1EC8, {0x49}, 1},
  {0x1EC9, {0x69}, 1},
  {0x1ECA, {0x49}, 1},
  {0x1ECB, {0x69}, 1},
  {0x1ECC, {0x4F}, 1},
  {0x1ECD, {0x6F}, 1},
  {0x1ECE, {0x4F}, 1},
  {0x1ECF, {0x6F}, 1},
  {0x1ED0, {0x4F}, 1},
  {0x1ED1, {0x6F}, 1},
  {0x1ED2, {0x4F}, 1},
  {0x1ED3, {0x6F}, 1},
  {0x1ED4, {0x4F}, 1},
  {0x1ED5, {0x6F}, 1},
  {0x1ED6, {0x4F}, 1},
  {0x1ED7, {0x6F}, 1},
  {0x1ED8, {0x4F}, 1},
  {0x1ED9, {0x6F}, 1},
  {0x1EDA, {0x4F}, 1},
  {0x1EDB, {0x6F}, 1},
  {0x1EDC, {0x4F}, 1},
  {0x1EDD, {0x6F}, 1},
  {0x1EDE, {0x4F}, 1},
  {0x1EDF, {0x6F}, 1},
  {0x1EE0, {0x4F}, 1},
  {0x1EE1, {0x6F}, 1},
  {0x1EE2, {0x4F}, 1},
  {0x1EE3, {0x6F}, 1},
  {0x1EE4, {0x55}, 1},
  {0x1EE5, {0x75}, 1},
  {0x1EE6, {0x55}, 1},
  {0x1EE7, {0x75}, 1},
  {0x1EE8, {0x55}, 1},
  {0x1EE9, {0x75}, 1},
  {0x1EEA, {0x55}, 1},
  {0x1EEB, {0x75}, 1},
  {0x1EEC, {0x55}, 1},
  {0x1EED, {0x75}, 1},
  {0x1EEE, {0x55}, 1},
  {0x1EEF, {0x75}, 1},
  {0x1EF0, {0x55}, 1},
  {0x1EF1, {0x75}, 1},
  {0x1EF2, {0x59}, 1},
  {0x1EF3, {0x79}, 1},
  {0x1EF4, {0x59}, 1},
  {0x1EF5, {0x79}, 1},
  {0x1EF6, {0x59}, 1},
  {0x1EF7, {0x79}, 1},
  {0x1EF8, {0x59}, 1},
  {0x1EF9, {0x79}, 1},
  {0x2C7C, {0x6A}, 1},
  {0x2C7D, {0x56}, 1},
  {0xA770, {0xA76F}, 1},
  {0xA7F1, {0x53}, 1},
  {0xA7F2, {0x43}, 1},
  {0xA7F3, {0x46}, 1},
  {0xA7F4, {0x51}, 1},
  {0xA7F8, {0x126}, 1},
  {0xA7F9, {0x153}, 1},
  {0xFB00, {0x66, 0x66}, 2},
  {0xFB01, {0x66, 0x69}, 2},
  {0xFB02, {0x66, 0x6C}, 2},
  {0xFB03, {0x66, 0x66, 0x69}, 3},
  {0xFB04, {0x66, 0x66, 0x6C}, 3},
  {0xFB05, {0x73, 0x74}, 2},
  {0xFB06, {0x73, 0x74}, 2},
  {0xFF01, {0x21}, 1},
  {0xFF02, {0x22}, 1},
  {0xFF03, {0x23}, 1},
  {0xFF04, {0x24}, 1},
  {0xFF05, {0x25}, 1},
  {0xFF06, {0x26}, 1},
  {0xFF07, {0x27}, 1},
  {0xFF08, {0x28}, 1},
  {0xFF09, {0x29}, 1},
  {0xFF0A, {0x2A}, 1},
  {0xFF0B, {0x2B}, 1},
  {0xFF0C, {0x2C}, 1},
  {0xFF0D, {0x2D}, 1},
  {0xFF0E, {0x2E}, 1},
  {0xFF0F, {0x2F}, 1},
  {0xFF10, {0x30}, 1},
  {0xFF11, {0x31}, 1},
  {0xFF12, {0x32}, 1},
  {0xFF13, {0x33}, 1},
  {0xFF14, {0x34}, 1},
  {0xFF15, {0x35}, 1},
  {0xFF16, {0x36}, 1},
  {0xFF17, {0x37}, 1},
  {0xFF18, {0x38}, 1},
  {0xFF19, {0x39}, 1},
  {0xFF1A, {0x3A}, 1},
  {0xFF1B, {0x3B}, 1},
  {0xFF1C, {0x3C}, 1},
  {0xFF1D, {0x3D}, 1},
  {0xFF1E, {0x3E}, 1},
  {0xFF1F, {0x3F}, 1},
  {0xFF20, {0x40}, 1},
  {0xFF21, {0x41}, 1},
  {0xFF22, {0x42}, 1},
  {0xFF23, {0x43}, 1},
  {0xFF24, {0x44}, 1},
  {0xFF25, {0x45}, 1},
  {0xFF26, {0x46}, 1},
  {0xFF27, {0x47}, 1},
  {0xFF28, {0x48}, 1},
  {0xFF29, {0x49}, 1},
  {0xFF2A, {0x4A}, 1},
  {0xFF2B, {0x4B}, 1},
  {0xFF2C, {0x4C}, 1},
  {0xFF2D, {0x4D}, 1},
  {0xFF2E, {0x4E}, 1},
  {0xFF2F, {0x4F}, 1},
  {0xFF30, {0x50}, 1},
  {0xFF31, {0x51}, 1},
  {0xFF32, {0x52}, 1},
  {0xFF33, {0x53}, 1},
  {0xFF34, {0x54}, 1},
  {0xFF35, {0x55}, 1},
  {0xFF36, {0x56}, 1},
  {0xFF37, {0x57}, 1},
  {0xFF38, {0x58}, 1},
  {0xFF39, {0x59}, 1},
  {0xFF3A, {0x5A}, 1},
  {0xFF3B, {0x5B}, 1},
  {0xFF3C, {0x5C}, 1},
  {0xFF3D, {0x5D}, 1},
  {0xFF3E, {0x5E}, 1},
  {0xFF3F, {0x5F}, 1},
  {0xFF40, {0x60}, 1},
  {0xFF41, {0x61}, 1},
  {0xFF42, {0x62}, 1},
  {0xFF43, {0x63}, 1},
  {0xFF44, {0x64}, 1},
  {0xFF45, {0x65}, 1},
  {0xFF46, {0x66}, 1},
  {0xFF47, {0x67}, 1},
  {0xFF48, {0x68}, 1},
  {0xFF49, {0x69}, 1},
  {0xFF4A, {0x6A}, 1},
  {0xFF4B, {0x6B}, 1},
  {0xFF4C, {0x6C}, 1},
  {0xFF4D, {0x6D}, 1},
  {0xFF4E, {0x6E}, 1},
  {0xFF4F, {0x6F}, 1},
  {0xFF50, {0x70}, 1},
  {0xFF51, {0x71}, 1},
  {0xFF52, {0x72}, 1},
  {0xFF53, {0x73}, 1},
  {0xFF54, {0x74}, 1},
  {0xFF55, {0x75}, 1},
  {0xFF56, {0x76}, 1},
  {0xFF57, {0x77}, 1},
  {0xFF58, {0x78}, 1},
  {0xFF59, {0x79}, 1},
  {0xFF5A, {0x7A}, 1},
  {0xFF5B, {0x7B}, 1},
  {0xFF5C, {0x7C}, 1},
  {0xFF5D, {0x7D}, 1},
  {0xFF5E, {0x7E}, 1},
};

static u32 nfkd_emit(u32 cp, u32 out[5]) {
  if (cp < 0x80) {
    out[0] = cp;
    return 1;
  }
  u32 lo = 0, hi = sizeof(NFKD_TABLE) / sizeof(NfkdEntry);
  while (lo < hi) {
    u32 mid = (lo + hi) / 2;
    if (NFKD_TABLE[mid].cp < cp) lo = mid + 1;
    else hi = mid;
  }
  if (lo < sizeof(NFKD_TABLE) / sizeof(NfkdEntry) && NFKD_TABLE[lo].cp == cp) {
    const NfkdEntry *e = &NFKD_TABLE[lo];
    for (u32 i = 0; i < e->n; i++) out[i] = e->out[i];
    return e->n;
  }
  out[0] = cp;
  return 1;
}

#define SLUG_MAX 64

// slugify into out (arena buffer, reset by the caller): NFKD, marks
// dropped, A-Z lowered, runs of other characters one "-", none at the
// ends, cut at SLUG_MAX with a trailing "-" dropped, "untitled" if empty.
static void slugify_into(Buf *out, Str text) {
  int gap = 0;
  u32 i = 0;
  while (i < text.n) {
    u32 len;
    u32 cp = utf8_decode(text.p, text.n, i, &len);
    i += len;
    if (is_mark_cp(cp)) continue;
    u32 emit[5];
    u32 cnt = nfkd_emit(cp, emit);
    for (u32 k = 0; k < cnt; k++) {
      u32 e = emit[k];
      i32 keep = -1;
      if (e >= 'A' && e <= 'Z') keep = (i32)(e + 32);
      else if ((e >= 'a' && e <= 'z') || (e >= '0' && e <= '9')) keep = (i32)e;
      if (keep < 0) {
        gap = out->n > 0;
        continue;
      }
      if (gap) buf_ch(out, '-');
      gap = 0;
      buf_ch(out, (u8)keep);
    }
  }
  if (out->n > SLUG_MAX) out->n = SLUG_MAX;
  while (out->n > 0 && out->p[out->n - 1] == '-') out->n--;
  if (out->n == 0) buf_c(out, "untitled");
}

// The number of words in text: runs of non-whitespace codepoints.
static u32 count_words(Str text) {
  u32 n = 0;
  int in_word = 0;
  u32 i = 0;
  while (i < text.n) {
    u32 len;
    u32 cp = utf8_decode(text.p, text.n, i, &len);
    i += len;
    if (is_space_cp(cp)) {
      in_word = 0;
    } else if (!in_word) {
      in_word = 1;
      n++;
    }
  }
  return n;
}

static const char *MONTH_NAMES[12] = {
  "January", "February", "March", "April", "May", "June",
  "July", "August", "September", "October", "November", "December"
};

// "5 November 2026" from a validated YYYY-MM-DD date.
static void long_date_into(Buf *out, Str date) {
  u32 d1 = 0;
  while (d1 < date.n && date.p[d1] != '-') d1++;
  u32 d2 = d1 + 1;
  while (d2 < date.n && date.p[d2] != '-') d2++;
  u32 m = 0, d = 0;
  for (u32 i = d1 + 1; i < d2; i++) m = m * 10 + (u32)(date.p[i] - '0');
  for (u32 i = d2 + 1; i < date.n; i++) d = d * 10 + (u32)(date.p[i] - '0');
  buf_u32dec(out, d);
  buf_ch(out, ' ');
  if (m >= 1 && m <= 12) buf_c(out, MONTH_NAMES[m - 1]);
  buf_ch(out, ' ');
  buf_put(out, date.p, d1);
}

// ------------------------------------------------------------ front matter

typedef struct {
  int has_title, has_date, has_tags, has_draft, has_layout, has_slug,
      has_summary;
  Str title, date, layout, slug, summary;
  Str *tags;
  u32 ntags;
  int draft;
} FMData;

static int days_in(u32 y, u32 m) {
  if (m == 2) return ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0) ? 29 : 28;
  return (m == 4 || m == 6 || m == 9 || m == 11) ? 30 : 31;
}

static int is_digit(u8 c) { return c >= '0' && c <= '9'; }

// Trim spaces and tabs at both ends.
static Str trim_st(Str s) {
  u32 a = 0, b = s.n;
  while (a < b && (s.p[a] == ' ' || s.p[a] == '\t')) a++;
  while (b > a && (s.p[b - 1] == ' ' || s.p[b - 1] == '\t')) b--;
  Str r = {s.p + a, b - a};
  return r;
}

// A string value into out: quoted with \" and \\ escapes only, or bare.
// On error returns 0 and sets *err to the bare reason (no key prefix).
static int fm_string(Str raw, Buf *out, const char **err) {
  if (raw.n > 0 && raw.p[0] == '"') {
    if (raw.n < 2 || raw.p[raw.n - 1] != '"') {
      *err = "a quoted string is not closed";
      return 0;
    }
    for (u32 i = 1; i + 1 < raw.n; i++) {
      u8 c = raw.p[i];
      if (c == '\\') {
        u8 nx = raw.p[i + 1];
        if ((nx != '"' && nx != '\\') || i + 1 == raw.n - 1) {
          *err = "a quoted string holds an escape other than \\\" or \\\\";
          return 0;
        }
        buf_ch(out, nx);
        i++;
      } else if (c == '"') {
        *err = "a quoted string holds a stray quote";
        return 0;
      } else {
        buf_ch(out, c);
      }
    }
    return 1;
  }
  if (raw.n > 0 && raw.p[0] == '[') {
    *err = "a bare string opens on [";
    return 0;
  }
  buf_str(out, raw);
  return 1;
}

// Parse one key's value. kind: 0 title/summary (string in val), 1 date,
// 2 tags, 3 draft, 4 layout, 5 slug. On error returns 0, errbuf filled
// with the full `key "..."` message.
static int fm_value(int kind, Str key, Str raw, FMData *d, Buf *errbuf) {
  Buf pre;
  buf_init(&pre);
  buf_c(&pre, "key \"");
  buf_str(&pre, key);
  buf_c(&pre, "\": ");
  switch (kind) {
    case 0: {
      Buf val;
      buf_init(&val);
      const char *err = 0;
      if (!fm_string(raw, &val, &err)) {
        buf_str(errbuf, buf_str_view(&pre));
        buf_c(errbuf, err);
        return 0;
      }
      if (key.p[0] == 't') {
        d->has_title = 1;
        d->title = buf_str_view(&val);
      } else {
        d->has_summary = 1;
        d->summary = buf_str_view(&val);
      }
      return 1;
    }
    case 1: {  // date
      int ok = raw.n == 10 && raw.p[4] == '-' && raw.p[7] == '-';
      u32 y = 0, mo = 0, da = 0;
      if (ok) {
        for (int i = 0; i < 10; i++) {
          if (i == 4 || i == 7) continue;
          if (!is_digit(raw.p[i])) {
            ok = 0;
            break;
          }
        }
      }
      if (ok) {
        y = (u32)(raw.p[0] - '0') * 1000 + (u32)(raw.p[1] - '0') * 100 +
            (u32)(raw.p[2] - '0') * 10 + (u32)(raw.p[3] - '0');
        mo = (u32)(raw.p[5] - '0') * 10 + (u32)(raw.p[6] - '0');
        da = (u32)(raw.p[8] - '0') * 10 + (u32)(raw.p[9] - '0');
        if (y < 1970 || y > 2099 || mo < 1 || mo > 12 || da < 1 ||
            da > (u32)days_in(y, mo))
          ok = 0;
      }
      if (!ok) {
        buf_str(errbuf, buf_str_view(&pre));
        buf_c(errbuf,
              "not a date from 1970-01-01 to 2099-12-31 (YYYY-MM-DD)");
        return 0;
      }
      d->has_date = 1;
      d->date = raw;
      return 1;
    }
    case 2: {  // tags
      if (raw.n < 2 || raw.p[0] != '[' || raw.p[raw.n - 1] != ']') {
        buf_str(errbuf, buf_str_view(&pre));
        buf_c(errbuf, "not a list [a, b]");
        return 0;
      }
      Str inner = {raw.p + 1, raw.n - 2};
      if (trim_st(inner).n == 0) {
        d->has_tags = 1;
        d->tags = 0;
        d->ntags = 0;
        return 1;
      }
      // count items
      u32 count = 1;
      for (u32 i = 0; i < inner.n; i++)
        if (inner.p[i] == ',') count++;
      Str *tags = (Str *)arena_alloc(sizeof(Str) * count, 8);
      u32 start = 0, ti = 0;
      for (u32 i = 0; i <= inner.n; i++) {
        if (i == inner.n || inner.p[i] == ',') {
          Str item = {(inner.p + start), i - start};
          item = trim_st(item);
          int bad = item.n == 0;
          for (u32 k = 0; k < item.n && !bad; k++)
            if (item.p[k] == '"' || item.p[k] == '[' || item.p[k] == ']')
              bad = 1;
          if (bad) {
            buf_str(errbuf, buf_str_view(&pre));
            buf_c(errbuf, "an empty tag, or one holding \" [ or ]");
            return 0;
          }
          tags[ti++] = item;
          start = i + 1;
        }
      }
      d->has_tags = 1;
      d->tags = tags;
      d->ntags = ti;
      return 1;
    }
    case 3: {  // draft
      if (str_eq_c(raw, "true")) {
        d->has_draft = 1;
        d->draft = 1;
        return 1;
      }
      if (str_eq_c(raw, "false")) {
        d->has_draft = 1;
        d->draft = 0;
        return 1;
      }
      buf_str(errbuf, buf_str_view(&pre));
      buf_c(errbuf, "true or false");
      return 0;
    }
    case 4: {  // layout: [a-z0-9-]+
      int ok = raw.n > 0;
      for (u32 i = 0; i < raw.n && ok; i++) {
        u8 c = raw.p[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'))
          ok = 0;
      }
      if (!ok) {
        buf_str(errbuf, buf_str_view(&pre));
        buf_c(errbuf, "not a template name");
        return 0;
      }
      d->has_layout = 1;
      d->layout = raw;
      return 1;
    }
    case 5: {  // slug: [a-z0-9]+(-[a-z0-9]+)* and length <= 64
      int ok = raw.n > 0 && raw.n <= 64;
      int seg = 0;  // chars in current segment
      for (u32 i = 0; i < raw.n && ok; i++) {
        u8 c = raw.p[i];
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
          seg++;
        } else if (c == '-') {
          if (seg == 0) ok = 0;
          seg = 0;
        } else {
          ok = 0;
        }
      }
      if (ok && seg == 0) ok = 0;
      if (!ok) {
        buf_str(errbuf, buf_str_view(&pre));
        buf_c(errbuf, "not a slug");
        return 0;
      }
      d->has_slug = 1;
      d->slug = raw;
      return 1;
    }
  }
  return 0;
}

// Parse front matter: returns 1 and fills d + body, or 0 with errbuf set.
static int parse_front_matter(Str text, FMData *d, Str *body, Buf *errbuf) {
  memset(d, 0, sizeof(*d));
  // first line
  u32 first_nl = 0;
  while (first_nl < text.n && text.p[first_nl] != '\n') first_nl++;
  Str first = {text.p, first_nl < text.n ? first_nl : text.n};
  if (!str_eq_c(first, "---")) {
    buf_c(errbuf, "no front matter");
    return 0;
  }
  if (first_nl >= text.n) {
    buf_c(errbuf, "the front matter is not closed");
    return 0;
  }
  // collect lines until the closing fence
  u32 cap = 16, nlines = 0;
  Str *lines = (Str *)arena_alloc(sizeof(Str) * cap, 8);
  i32 body_start = -1;
  u32 pos = first_nl + 1;
  while (pos <= text.n) {
    u32 nl = pos;
    while (nl < text.n && text.p[nl] != '\n') nl++;
    u32 end = nl < text.n ? nl : text.n;
    Str line = {text.p + pos, end - pos};
    if (str_eq_c(line, "---")) {
      body_start = nl < text.n ? (i32)(nl + 1) : (i32)text.n;
      break;
    }
    if (nlines == cap) {
      Str *nl2 = (Str *)arena_alloc(sizeof(Str) * cap * 2, 8);
      memcpy(nl2, lines, sizeof(Str) * nlines);
      lines = nl2;
      cap *= 2;
    }
    lines[nlines++] = line;
    if (nl >= text.n) break;
    pos = nl + 1;
  }
  if (body_start < 0) {
    buf_c(errbuf, "the front matter is not closed");
    return 0;
  }
  static const char *KEYS[7] = {"title", "date", "tags", "draft",
                                "layout", "slug", "summary"};
  for (u32 k = 0; k < nlines; k++) {
    Str line = lines[k];
    int blank = 1;
    for (u32 i = 0; i < line.n; i++)
      if (line.p[i] != ' ' && line.p[i] != '\t') {
        blank = 0;
        break;
      }
    if (blank || (line.n > 0 && line.p[0] == '#')) continue;
    // FM_LINE: ^([a-z_]+):(?: (.*))?$
    u32 ki = 0;
    while (ki < line.n && line.p[ki] >= 'a' && line.p[ki] <= 'z') ki++;
    // note: [a-z_] also allows '_' after the first char; the reference
    // class is [a-z_]+ for the whole key
    ki = 0;
    while (ki < line.n &&
           ((line.p[ki] >= 'a' && line.p[ki] <= 'z') || line.p[ki] == '_'))
      ki++;
    Str raw = {0, 0};
    int matched = 0;
    if (ki > 0 && ki < line.n && line.p[ki] == ':') {
      if (ki + 1 == line.n) {
        matched = 1;
        raw.p = line.p + line.n;
        raw.n = 0;
      } else if (line.p[ki + 1] == ' ') {
        matched = 1;
        raw.p = line.p + ki + 2;
        raw.n = line.n - (ki + 2);
      }
    }
    if (!matched) {
      buf_c(errbuf, "line ");
      buf_u32dec(errbuf, k + 2);
      buf_c(errbuf, " is not \"key: value\"");
      return 0;
    }
    Str key = {line.p, ki};
    // trim trailing spaces/tabs of raw
    while (raw.n > 0 && (raw.p[raw.n - 1] == ' ' || raw.p[raw.n - 1] == '\t'))
      raw.n--;
    int kind = -1;
    for (int j = 0; j < 7; j++)
      if (str_eq_c(key, KEYS[j])) {
        kind = j == 0   ? 0    // title
               : j == 1 ? 1    // date
               : j == 2 ? 2    // tags
               : j == 3 ? 3    // draft
               : j == 4 ? 4    // layout
               : j == 5 ? 5    // slug
                        : 0;   // summary (string like title)
        break;
      }
    if (kind < 0) {
      buf_c(errbuf, "unknown key \"");
      buf_str(errbuf, key);
      buf_ch(errbuf, '"');
      return 0;
    }
    int seen = (kind == 0 && key.p[0] == 't')   ? d->has_title
               : (kind == 0)                    ? d->has_summary
               : kind == 1                      ? d->has_date
               : kind == 2                      ? d->has_tags
               : kind == 3                      ? d->has_draft
               : kind == 4                      ? d->has_layout
                                                : d->has_slug;
    if (seen) {
      buf_c(errbuf, "key \"");
      buf_str(errbuf, key);
      buf_c(errbuf, "\" is given twice");
      return 0;
    }
    if (raw.n == 0) {
      buf_c(errbuf, "key \"");
      buf_str(errbuf, key);
      buf_c(errbuf, "\" has no value");
      return 0;
    }
    if (!fm_value(kind, key, raw, d, errbuf)) return 0;
  }
  if (!d->has_title) {
    buf_c(errbuf, "no title");
    return 0;
  }
  body->p = text.p + body_start;
  body->n = text.n - (u32)body_start;
  return 1;
}

// ------------------------------------------------------------ markdown

// Link resolution context, filled in by the build before rendering.
typedef struct {
  Str dir;          // the source's directory ("" at the root)
  Str source_path;  // the source being rendered (for broken-link lines)
  Str base;         // the site base url, trailing slashes stripped
  Map *by_source;   // path -> source index
  Map *static_paths;
  Buf *broken;      // one Str view per broken-link line is appended
  u32 nbroken;
} LinkCtx;

// Forward declarations for the build's data (defined below).
typedef struct Source Source;
static Source *g_sources;
static Str source_url_of(i32 idx);

static int is_escapable(u8 c) {
  switch (c) {
    case '\\': case '`': case '*': case '_': case '[': case ']':
    case '(': case ')': case '#': case '+': case '-': case '.':
    case '!': case '>':
      return 1;
  }
  return 0;
}

// resolvePath: rel resolved against dir, or -1 when it climbs out.
static int resolve_path(Str dir, Str rel, Buf *out) {
  // parts stack in a scratch area of out itself is awkward; use a
  // temporary arena array of segment views.
  u32 cap = 16, n = 0;
  Str *parts = (Str *)arena_alloc(sizeof(Str) * cap, 8);
  if (dir.n > 0) {
    u32 start = 0;
    for (u32 i = 0; i <= dir.n; i++) {
      if (i == dir.n || dir.p[i] == '/') {
        if (n == cap) return -1;
        Str seg = {dir.p + start, i - start};
        parts[n++] = seg;
        start = i + 1;
      }
    }
  }
  u32 start = 0;
  for (u32 i = 0; i <= rel.n; i++) {
    if (i == rel.n || rel.p[i] == '/') {
      Str seg = {rel.p + start, i - start};
      start = i + 1;
      if (seg.n == 0 || str_eq_c(seg, ".")) continue;
      if (str_eq_c(seg, "..")) {
        if (n == 0) return -1;
        n--;
        continue;
      }
      if (n == cap) {
        Str *np = (Str *)arena_alloc(sizeof(Str) * cap * 2, 8);
        memcpy(np, parts, sizeof(Str) * n);
        parts = np;
        cap *= 2;
      }
      parts[n++] = seg;
    }
  }
  for (u32 i = 0; i < n; i++) {
    if (i) buf_ch(out, '/');
    buf_str(out, parts[i]);
  }
  return 0;
}

static int has_scheme(Str t) {
  if (t.n == 0) return 0;
  u8 c0 = t.p[0];
  if (!((c0 >= 'A' && c0 <= 'Z') || (c0 >= 'a' && c0 <= 'z'))) return 0;
  for (u32 i = 1; i < t.n; i++) {
    u8 c = t.p[i];
    if (c == ':') return 1;
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.'))
      return 0;
  }
  return 0;
}

// The href for a link target, written into out (which the caller resets
// per call); broken links are recorded on the context.
static void link_href(LinkCtx *lc, Str target, Buf *out) {
  if (has_scheme(target) || (target.n > 0 && target.p[0] == '/') ||
      (target.n > 0 && target.p[0] == '#')) {
    buf_str(out, target);
    return;
  }
  u32 hash = target.n;
  for (u32 i = 0; i < target.n; i++)
    if (target.p[i] == '#') {
      hash = i;
      break;
    }
  Str rel = {target.p, hash};
  Str frag = {target.p + hash, target.n - hash};
  int resolved = 0;
  if (rel.n > 0) {
    Buf path;
    buf_init(&path);
    if (resolve_path(lc->dir, rel, &path) == 0) {
      Str p = buf_str_view(&path);
      i32 idx = map_get(lc->by_source, p);
      if (idx >= 0) {
        buf_str(out, source_url_of(idx));
        buf_str(out, frag);
        resolved = 1;
      } else if (map_get(lc->static_paths, p) >= 0) {
        buf_str(out, lc->base);
        buf_ch(out, '/');
        buf_str(out, p);
        buf_str(out, frag);
        resolved = 1;
      }
    }
  }
  if (!resolved) {
    Buf *line = (Buf *)arena_alloc(sizeof(Buf), 8);
    buf_init(line);
    buf_c(line, "broken-link ");
    buf_str(line, lc->source_path);
    buf_c(line, " -> ");
    buf_str(line, target);
    // append the line buffer's pointer to the broken array
    buf_put(lc->broken, &line, sizeof(Buf *));
    lc->nbroken++;
    buf_str(out, target);
  }
}

// The first index from `from` where s holds the byte string `what`
// outside a backslash escape, or -1.
static i32 find_unescaped(Str s, const char *what, u32 wlen, u32 from) {
  for (u32 j = from; j + wlen <= s.n; j++) {
    if (s.p[j] == '\\' && j + 1 < s.n && is_escapable(s.p[j + 1])) {
      j++;
      continue;
    }
    if (memcmp(s.p + j, what, wlen) == 0) return (i32)j;
  }
  return -1;
}

// The first single * from `from` (one whose run is not **), or -1.
static i32 find_single_star(Str s, u32 from) {
  for (u32 j = from; j < s.n; j++) {
    u8 c = s.p[j];
    if (c == '\\' && j + 1 < s.n && is_escapable(s.p[j + 1])) {
      j++;
      continue;
    }
    if (c != '*') continue;
    if (j + 1 < s.n && s.p[j + 1] == '*') {
      j++;
      continue;
    }
    return (i32)j;
  }
  return -1;
}

static void render_inline(Str s, LinkCtx *lc, Buf *out);

static void flush_plain(Buf *out, Str s, u32 from, u32 to) {
  if (to > from) {
    Str seg = {s.p + from, to - from};
    buf_escape_html(out, seg);
  }
}

// The offset of the first inline-markup byte in s[from..n), or n:
// backslash, backtick, star or open bracket.
static u32 inline_special_at(Str s, u32 from) {
  u32 i = from;
  while (i + 16 <= s.n) {
    v128_t v = wasm_v128_load(s.p + i);
    v128_t hit = wasm_i8x16_eq(v, wasm_i8x16_splat('\\'));
    hit = wasm_v128_or(hit, wasm_i8x16_eq(v, wasm_i8x16_splat('`')));
    hit = wasm_v128_or(hit, wasm_i8x16_eq(v, wasm_i8x16_splat('*')));
    hit = wasm_v128_or(hit, wasm_i8x16_eq(v, wasm_i8x16_splat('[')));
    u32 mask = wasm_i8x16_bitmask(hit);
    if (mask) return i + (u32)__builtin_ctz(mask);
    i += 16;
  }
  while (i < s.n) {
    u8 c = s.p[i];
    if (c == '\\' || c == '`' || c == '*' || c == '[') return i;
    i++;
  }
  return s.n;
}

static void render_inline(Str s, LinkCtx *lc, Buf *out) {
  u32 plain = 0, i = 0, n = s.n;
  while (i < n) {
    u8 c = s.p[i];
    if (c != '\\' && c != '`' && c != '*' && c != '[') {
      i = inline_special_at(s, i);
      continue;
    }
    if (c == '\\') {
      if (i + 1 < n && is_escapable(s.p[i + 1])) {
        flush_plain(out, s, plain, i);
        if (s.p[i + 1] == '>')
          buf_c(out, "&gt;");
        else
          buf_ch(out, s.p[i + 1]);
        i += 2;
        plain = i;
      } else {
        i++;
      }
      continue;
    }
    if (c == '`') {
      i32 j = -1;
      for (u32 k = i + 1; k < n; k++)
        if (s.p[k] == '`') {
          j = (i32)k;
          break;
        }
      if (j > (i32)i + 1) {
        flush_plain(out, s, plain, i);
        buf_c(out, "<code>");
        Str inner = {s.p + i + 1, (u32)j - i - 1};
        buf_escape_html(out, inner);
        buf_c(out, "</code>");
        i = (u32)j + 1;
        plain = i;
      } else {
        i++;
      }
      continue;
    }
    if (c == '*') {
      if (i + 1 < n && s.p[i + 1] == '*') {
        i32 j = find_unescaped(s, "**", 2, i + 2);
        if (j > (i32)i + 2) {
          flush_plain(out, s, plain, i);
          buf_c(out, "<strong>");
          Str inner = {s.p + i + 2, (u32)j - i - 2};
          render_inline(inner, lc, out);
          buf_c(out, "</strong>");
          i = (u32)j + 2;
          plain = i;
        } else {
          i += 2;
        }
      } else {
        i32 j = find_single_star(s, i + 1);
        if (j > (i32)i + 1) {
          flush_plain(out, s, plain, i);
          buf_c(out, "<em>");
          Str inner = {s.p + i + 1, (u32)j - i - 1};
          render_inline(inner, lc, out);
          buf_c(out, "</em>");
          i = (u32)j + 1;
          plain = i;
        } else {
          i++;
        }
      }
      continue;
    }
    if (c == '[') {
      i32 k = find_unescaped(s, "]", 1, i + 1);
      i32 m = -1;
      if (k != -1 && (u32)k + 1 < n && s.p[k + 1] == '(') {
        for (u32 q = (u32)k + 2; q < n; q++)
          if (s.p[q] == ')') {
            m = (i32)q;
            break;
          }
      }
      Str target = {0, 0};
      int ok = 0;
      if (m != -1) {
        target.p = s.p + k + 2;
        target.n = (u32)m - ((u32)k + 2);
        if (target.n > 0) {
          ok = 1;
          u32 q = 0;
          while (q < target.n) {
            u32 len;
            u32 cp = utf8_decode(target.p, target.n, q, &len);
            if (is_space_cp(cp)) {
              ok = 0;
              break;
            }
            q += len;
          }
        }
      }
      if (ok) {
        flush_plain(out, s, plain, i);
        Buf href;
        buf_init(&href);
        link_href(lc, target, &href);
        buf_c(out, "<a href=\"");
        buf_escape_html(out, buf_str_view(&href));
        buf_c(out, "\">");
        Str inner = {s.p + i + 1, (u32)k - i - 1};
        render_inline(inner, lc, out);
        buf_c(out, "</a>");
        i = (u32)m + 1;
        plain = i;
      } else {
        i++;
      }
      continue;
    }
    i++;
  }
  flush_plain(out, s, plain, n);
}

// ------------------------------------------------------------ markdown blocks

static int line_is_blank(Str line) {
  for (u32 i = 0; i < line.n; i++)
    if (line.p[i] != ' ' && line.p[i] != '\t') return 0;
  return 1;
}

static Str line_trim(Str line) { return trim_st(line); }

static int line_has_separator(Str line) {
  // U+2028 / U+2029 in UTF-8: E2 80 A8 / E2 80 A9
  for (u32 i = 0; i + 2 < line.n; i++)
    if (line.p[i] == 0xE2 && line.p[i + 1] == 0x80 &&
        (line.p[i + 2] == 0xA8 || line.p[i + 2] == 0xA9))
      return 1;
  return 0;
}

// A fence line's language: returns 1 and sets *lang (possibly empty).
static int fence_lang(Str line, Str *lang) {
  if (!str_starts(line, "```")) return 0;
  u32 i = 3;
  while (i < line.n) {
    u8 c = line.p[i];
    int ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
             (c >= '0' && c <= '9') || c == '_' || c == '+' || c == '-';
    if (!ok) break;
    i++;
  }
  lang->p = line.p + 3;
  lang->n = i - 3;
  while (i < line.n) {
    if (line.p[i] != ' ' && line.p[i] != '\t') return 0;
    i++;
  }
  return 1;
}

static int is_fence_close(Str line) {
  if (!str_starts(line, "```")) return 0;
  for (u32 i = 3; i < line.n; i++)
    if (line.p[i] != ' ' && line.p[i] != '\t') return 0;
  return 1;
}

// A heading: returns depth 1-6 and sets *text, or 0.
static int heading_of(Str line, Str *text) {
  u32 depth = 0;
  while (depth < line.n && line.p[depth] == '#') depth++;
  if (depth == 0 || depth > 6) return 0;
  if (depth >= line.n) return 0;
  if (line.p[depth] != ' ' && line.p[depth] != '\t') return 0;
  if (line_has_separator(line)) return 0;
  u32 a = depth;
  while (a < line.n && (line.p[a] == ' ' || line.p[a] == '\t')) a++;
  u32 b = line.n;
  while (b > a && (line.p[b - 1] == ' ' || line.p[b - 1] == '\t')) b--;
  if (b == a) return 0;
  text->p = line.p + a;
  text->n = b - a;
  return (int)depth;
}

static int is_break(Str line) {
  u32 b = line.n;
  while (b > 0 && (line.p[b - 1] == ' ' || line.p[b - 1] == '\t')) b--;
  if (b != 3) return 0;
  return (line.p[0] == '-' && line.p[1] == '-' && line.p[2] == '-') ||
         (line.p[0] == '*' && line.p[1] == '*' && line.p[2] == '*');
}

// A bullet item's text, or 0 (sets *text when 1).
static int bullet_text(Str line, Str *text) {
  if (line.n < 2) return 0;
  u8 c = line.p[0];
  if ((c == '-' || c == '*') && line.p[1] == ' ') {
    if (line_has_separator(line)) return 0;
    text->p = line.p + 2;
    text->n = line.n - 2;
    return 1;
  }
  return 0;
}

// An ordered item: returns 1 with *start and *text set.
static int ordered_item(Str line, u32 *start, Str *text) {
  u32 d = 0;
  while (d < line.n && line.p[d] >= '0' && line.p[d] <= '9') d++;
  if (d == 0 || d > 9) return 0;
  if (d + 1 >= line.n) return 0;
  if (line.p[d] != '.' || line.p[d + 1] != ' ') return 0;
  if (line_has_separator(line)) return 0;
  u32 v = 0;
  for (u32 i = 0; i < d; i++) v = v * 10 + (u32)(line.p[i] - '0');
  *start = v;
  text->p = line.p + d + 2;
  text->n = line.n - (d + 2);
  return 1;
}

static int is_continuation(Str line) {
  if (line.n < 2 || line.p[0] != ' ' || line.p[1] != ' ') return 0;
  u32 i = 2;
  while (i < line.n && line.p[i] == ' ') i++;
  if (i >= line.n) return 0;
  u32 len;
  u32 cp = utf8_decode(line.p, line.n, i, &len);
  (void)len;
  return !is_space_cp(cp);
}

typedef struct {
  i32 level;
  Str id;
  Str html;
} TocEntry;

// slugify with the per-case memo (key: raw text, value: index into the
// case's slug arena list). Implemented by the build; declared here.
static Str slugify_cached(Map *cache, Str text);

static void join_lines(Buf *out, Str *lines, u32 n) {
  for (u32 i = 0; i < n; i++) {
    if (i) buf_ch(out, '\n');
    buf_str(out, lines[i]);
  }
}

// Render one source body. html/toc/excerpt are arena results.
static void render_markdown(Str src, LinkCtx *lc, Map *slug_cache,
                            Buf *html, TocEntry **toc_out, u32 *ntoc_out,
                            Str *excerpt_out) {
  // line index, one pass with a doubling array
  u32 lines_cap = 64, nlines = 0;
  Str *lines = (Str *)arena_alloc(sizeof(Str) * lines_cap, 8);
  {
    u32 start = 0;
    for (u32 i = 0; i <= src.n; i++) {
      if (i == src.n || src.p[i] == '\n') {
        if (nlines == lines_cap) {
          Str *nl = (Str *)arena_alloc(sizeof(Str) * lines_cap * 2, 8);
          memcpy(nl, lines, sizeof(Str) * nlines);
          lines = nl;
          lines_cap *= 2;
        }
        lines[nlines].p = src.p + start;
        lines[nlines].n = i - start;
        nlines++;
        start = i + 1;
      }
    }
  }
  u32 toc_cap = 16, ntoc = 0;
  TocEntry *toc = (TocEntry *)arena_alloc(sizeof(TocEntry) * toc_cap, 8);
  Map ids;
  map_init(&ids, 10);
  int have_excerpt = 0;
  Str excerpt = {0, 0};
  u32 i = 0;
  while (i < nlines) {
    Str line = lines[i];
    if (line_is_blank(line)) {
      i++;
      continue;
    }
    Str lang;
    if (fence_lang(line, &lang)) {
      i++;
      buf_c(html, "<pre><code");
      if (lang.n > 0) {
        buf_c(html, " class=\"language-");
        buf_str(html, lang);
        buf_ch(html, '"');
      }
      buf_ch(html, '>');
      while (i < nlines && !is_fence_close(lines[i])) {
        buf_escape_html(html, lines[i]);
        buf_ch(html, '\n');
        i++;
      }
      i++;
      buf_c(html, "</code></pre>\n");
      continue;
    }
    Str htext;
    int depth = heading_of(line, &htext);
    if (depth > 0) {
      Str slug = slugify_cached(slug_cache, htext);
      // unique against this document's ids
      Buf idb;
      buf_init(&idb);
      buf_str(&idb, slug);
      u32 nn = 2;
      while (map_get(&ids, buf_str_view(&idb)) >= 0) {
        idb.n = slug.n;
        buf_ch(&idb, '-');
        buf_u32dec(&idb, nn++);
      }
      Str id = buf_str_view(&idb);
      map_set(&ids, id, 0);
      Buf inner;
      buf_init(&inner);
      render_inline(htext, lc, &inner);
      buf_c(html, "<h");
      buf_ch(html, (u8)('0' + depth));
      buf_c(html, " id=\"");
      buf_str(html, id);
      buf_c(html, "\">");
      buf_str(html, buf_str_view(&inner));
      buf_c(html, "</h");
      buf_ch(html, (u8)('0' + depth));
      buf_c(html, ">\n");
      if (depth == 2 || depth == 3) {
        if (ntoc == toc_cap) {
          TocEntry *nt = (TocEntry *)arena_alloc(sizeof(TocEntry) * toc_cap * 2, 8);
          memcpy(nt, toc, sizeof(TocEntry) * ntoc);
          toc = nt;
          toc_cap *= 2;
        }
        toc[ntoc].level = depth;
        toc[ntoc].id = id;
        toc[ntoc].html = buf_str_view(&inner);
        ntoc++;
      }
      i++;
      continue;
    }
    if (is_break(line)) {
      buf_c(html, "<hr>\n");
      i++;
      continue;
    }
    if (line.n > 0 && line.p[0] == '>') {
      u32 cap = 8, nb = 0;
      Str *body = (Str *)arena_alloc(sizeof(Str) * cap, 8);
      while (i < nlines && lines[i].n > 0 && lines[i].p[0] == '>') {
        Str l = {lines[i].p + 1, lines[i].n - 1};
        if (l.n > 0 && l.p[0] == ' ') {
          l.p++;
          l.n--;
        }
        if (nb == cap) {
          Str *nb2 = (Str *)arena_alloc(sizeof(Str) * cap * 2, 8);
          memcpy(nb2, body, sizeof(Str) * nb);
          body = nb2;
          cap *= 2;
        }
        body[nb++] = l;
        i++;
      }
      u32 nk = 0;
      for (u32 k = 0; k < nb; k++) {
        Str t = line_trim(body[k]);
        if (t.n > 0) body[nk++] = t;
      }
      if (nk == 0) {
        buf_c(html, "<blockquote>\n</blockquote>\n");
      } else {
        Buf joined;
        buf_init(&joined);
        join_lines(&joined, body, nk);
        buf_c(html, "<blockquote>\n<p>");
        render_inline(buf_str_view(&joined), lc, html);
        buf_c(html, "</p>\n</blockquote>\n");
      }
      continue;
    }
    Str btext;
    u32 ostart = 0;
    Str otext;
    int is_bullet = bullet_text(line, &btext);
    int is_ordered = !is_bullet && ordered_item(line, &ostart, &otext);
    if (is_bullet || is_ordered) {
      // items: arrays of line-lists; store as (lines ptr, count) pairs
      u32 cap = 8, nitems = 0;
      Str **items = (Str **)arena_alloc(sizeof(Str *) * cap, 8);
      u32 *counts = (u32 *)arena_alloc(sizeof(u32) * cap, 4);
      u32 first_start = ostart;
      while (i < nlines) {
        Str l = lines[i];
        Str t;
        int ok;
        if (is_ordered) {
          u32 st;
          ok = ordered_item(l, &st, &t);
        } else {
          ok = bullet_text(l, &t);
        }
        if (ok) {
          if (nitems == cap) {
            Str **ni = (Str **)arena_alloc(sizeof(Str *) * cap * 2, 8);
            u32 *nc = (u32 *)arena_alloc(sizeof(u32) * cap * 2, 4);
            memcpy(ni, items, sizeof(Str *) * nitems);
            memcpy(nc, counts, sizeof(u32) * nitems);
            items = ni;
            counts = nc;
            cap *= 2;
          }
          Str *one = (Str *)arena_alloc(sizeof(Str) * 4, 8);
          one[0] = line_trim(t);
          items[nitems] = one;
          counts[nitems] = 1;
          nitems++;
          i++;
        } else if (nitems > 0 && is_continuation(l)) {
          u32 c = counts[nitems - 1];
          Str *grown = (Str *)arena_alloc(sizeof(Str) * (c + 1), 8);
          memcpy(grown, items[nitems - 1], sizeof(Str) * c);
          grown[c] = line_trim(l);
          items[nitems - 1] = grown;
          counts[nitems - 1] = c + 1;
          i++;
        } else {
          break;
        }
      }
      if (is_ordered && first_start != 1) {
        buf_c(html, "<ol start=\"");
        buf_u32dec(html, first_start);
        buf_c(html, "\">\n");
      } else {
        buf_c(html, is_ordered ? "<ol>\n" : "<ul>\n");
      }
      for (u32 k = 0; k < nitems; k++) {
        Buf joined;
        buf_init(&joined);
        join_lines(&joined, items[k], counts[k]);
        buf_c(html, "<li>");
        render_inline(buf_str_view(&joined), lc, html);
        buf_c(html, "</li>\n");
      }
      buf_c(html, is_ordered ? "</ol>\n" : "</ul>\n");
      continue;
    }
    // paragraph
    {
      u32 cap = 8, nb = 0;
      Str *body = (Str *)arena_alloc(sizeof(Str) * cap, 8);
      body[nb++] = line_trim(line);
      i++;
      while (i < nlines) {
        Str l = lines[i];
        Str dummy;
        if (line_is_blank(l) || fence_lang(l, &dummy) ||
            heading_of(l, &dummy) > 0 || is_break(l))
          break;
        if (nb == cap) {
          Str *nb2 = (Str *)arena_alloc(sizeof(Str) * cap * 2, 8);
          memcpy(nb2, body, sizeof(Str) * nb);
          body = nb2;
          cap *= 2;
        }
        body[nb++] = line_trim(l);
        i++;
      }
      Buf joined;
      buf_init(&joined);
      join_lines(&joined, body, nb);
      Buf inner;
      buf_init(&inner);
      render_inline(buf_str_view(&joined), lc, &inner);
      if (!have_excerpt) {
        excerpt = buf_str_view(&inner);
        have_excerpt = 1;
      }
      buf_c(html, "<p>");
      buf_str(html, buf_str_view(&inner));
      buf_c(html, "</p>\n");
    }
  }
  *toc_out = toc;
  *ntoc_out = ntoc;
  *excerpt_out = excerpt;
}

// ------------------------------------------------------------ templates

enum { N_TEXT, N_VAR, N_RAW, N_IF, N_FOR, N_INCLUDE };

typedef struct Node Node;
struct Node {
  int kind;
  Str text;                 // N_TEXT
  Str *path;                // VAR/RAW/IF/FOR path segments
  u32 npath;
  int not_flag;             // N_IF
  Str name;                 // N_FOR variable / N_INCLUDE template
  Node **body;
  u32 nbody, bodycap;       // N_FOR body; root list for a template
  Node **thenb;
  u32 nthen, thencap;       // N_IF
  Node **elseb;
  u32 nelse, elsecap;       // N_IF
  int has_else;             // N_IF
};

// Nodes are allocated one by one from the arena so pointers stay
// stable; child lists are pointer vectors that may move as they grow.
static Node *node_new(int kind) {
  Node *nd = (Node *)arena_alloc_zero(sizeof(Node), 8);
  nd->kind = kind;
  return nd;
}

static void ptrvec_push(Node ***arr, u32 *n, u32 *cap, Node *nd) {
  if (*n == *cap) {
    u32 ncap = *cap ? *cap * 2 : 8;
    Node **ni = (Node **)arena_alloc(sizeof(Node *) * ncap, 8);
    if (*arr) memcpy(ni, *arr, sizeof(Node *) * *n);
    *arr = ni;
    *cap = ncap;
  }
  (*arr)[(*n)++] = nd;
}

// JS .trim() (whitespace per \s) on a string.
static Str trim_js(Str s) {
  u32 a = 0, b = s.n;
  while (a < b) {
    u32 len;
    u32 cp = utf8_decode(s.p, s.n, a, &len);
    if (!is_space_cp(cp)) break;
    a += len;
  }
  while (b > a) {
    // find the last codepoint start
    u32 i = b - 1;
    while (i > a && (s.p[i] & 0xC0) == 0x80) i--;
    u32 len;
    u32 cp = utf8_decode(s.p, s.n, i, &len);
    if (!is_space_cp(cp)) break;
    b = i;
  }
  Str r = {s.p + a, b - a};
  return r;
}

// Validate a dotted path and split it; on failure returns 0 and the
// caller reports it with the original text.
static int tpl_path_of(Str text, Str **segs_out, u32 *n_out) {
  if (text.n == 0) return 0;
  u32 nseg = 1;
  for (u32 i = 0; i < text.n; i++)
    if (text.p[i] == '.') nseg++;
  Str *segs = (Str *)arena_alloc(sizeof(Str) * nseg, 8);
  u32 start = 0, si = 0;
  for (u32 i = 0; i <= text.n; i++) {
    if (i == text.n || text.p[i] == '.') {
      Str seg = {text.p + start, i - start};
      if (seg.n == 0) return 0;
      u8 c0 = seg.p[0];
      if (!((c0 >= 'A' && c0 <= 'Z') || (c0 >= 'a' && c0 <= 'z') || c0 == '_'))
        return 0;
      for (u32 k = 1; k < seg.n; k++) {
        u8 c = seg.p[k];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_'))
          return 0;
      }
      segs[si++] = seg;
      start = i + 1;
    }
  }
  *segs_out = segs;
  *n_out = si;
  return 1;
}

static void tpl_error(Buf *errbuf, Str name, const char *msg) {
  buf_c(errbuf, "template \"");
  buf_str(errbuf, name);
  buf_c(errbuf, "\": ");
  buf_c(errbuf, msg);
}

static void tpl_error_path(Buf *errbuf, Str name, Str text) {
  buf_c(errbuf, "template \"");
  buf_str(errbuf, name);
  buf_c(errbuf, "\": \"");
  buf_escape_json(errbuf, text);
  buf_c(errbuf, "\" is not a path");
}

static int span_is_blank(Str src, u32 from, u32 to) {
  for (u32 i = from; i < to; i++)
    if (src.p[i] != ' ' && src.p[i] != '\t') return 0;
  return 1;
}

static i32 find_bytes(Str s, const char *what, u32 wlen, u32 from) {
  if (s.n < wlen) return -1;
  for (u32 i = from; i + wlen <= s.n; i++)
    if (memcmp(s.p + i, what, wlen) == 0) return (i32)i;
  return -1;
}

enum { T_TEXT, T_VAR, T_RAW, T_TAG };

typedef struct {
  int kind;
  Str text;      // T_TEXT text / T_TAG tag text
  Str *path;     // T_VAR / T_RAW
  u32 npath;
} Token;

typedef struct {
  Token *items;
  u32 n, cap;
} TokenVec;

static void tokenvec_push(TokenVec *v, Token t) {
  if (v->n == v->cap) {
    u32 ncap = v->cap ? v->cap * 2 : 16;
    Token *ni = (Token *)arena_alloc(sizeof(Token) * ncap, 8);
    if (v->items) memcpy(ni, v->items, sizeof(Token) * v->n);
    v->items = ni;
    v->cap = ncap;
  }
  v->items[v->n++] = t;
}

// Tokenize; returns 1 or 0 with errbuf set.
static int tpl_tokenize(Str name, Str src, TokenVec *out, Buf *errbuf) {
  Buf text;
  buf_init(&text);
  u32 i = 0;
  while (i < src.n) {
    i32 open = find_bytes(src, "{", 1, i);
    if (open < 0 || (u32)open == src.n - 1) {
      Str rest = {src.p + i, src.n - i};
      buf_str(&text, rest);
      break;
    }
    u8 next = src.p[open + 1];
    if (next != '{' && next != '%') {
      Str seg = {src.p + i, (u32)open + 1 - i};
      buf_str(&text, seg);
      i = (u32)open + 1;
      continue;
    }
    Str seg = {src.p + i, (u32)open - i};
    buf_str(&text, seg);
    if (next == '{') {
      int raw = (u32)open + 2 < src.n && src.p[open + 2] == '{';
      i32 close = find_bytes(src, raw ? "}}}" : "}}", raw ? 3 : 2,
                             (u32)open + (raw ? 3 : 2));
      if (close < 0) {
        tpl_error(errbuf, name,
                  raw ? "a {{{ is not closed" : "a {{ is not closed");
        return 0;
      }
      if (text.n > 0) {
        Token t = {T_TEXT, buf_str_view(&text), 0, 0};
        tokenvec_push(out, t);
        buf_init(&text);
      }
      Str inner = {src.p + open + (raw ? 3 : 2),
                   (u32)close - ((u32)open + (raw ? 3 : 2))};
      inner = trim_js(inner);
      Str *segs;
      u32 nseg;
      if (!tpl_path_of(inner, &segs, &nseg)) {
        tpl_error_path(errbuf, name, inner);
        return 0;
      }
      Token t = {raw ? T_RAW : T_VAR, {0, 0}, segs, nseg};
      tokenvec_push(out, t);
      i = (u32)close + (raw ? 3 : 2);
      continue;
    }
    // {% ... %}
    i32 close = find_bytes(src, "%}", 2, (u32)open + 2);
    if (close < 0) {
      tpl_error(errbuf, name, "a {% is not closed");
      return 0;
    }
    u32 end = (u32)close + 2;
    u32 line_start = 0;
    if (open > 0) {
      i32 ls = -1;
      for (u32 k = (u32)open; k > 0; k--)
        if (src.p[k - 1] == '\n') {
          ls = (i32)(k - 1);
          break;
        }
      line_start = ls < 0 ? 0 : (u32)ls + 1;
    }
    i32 nl = find_bytes(src, "\n", 1, end);
    u32 line_end = nl < 0 ? src.n : (u32)nl;
    int standalone = span_is_blank(src, line_start, (u32)open) &&
                     span_is_blank(src, end, line_end);
    if (standalone) text.n -= (u32)open - line_start;
    if (text.n > 0) {
      Token t = {T_TEXT, buf_str_view(&text), 0, 0};
      tokenvec_push(out, t);
      buf_init(&text);
    }
    Str tag = {src.p + open + 2, (u32)close - ((u32)open + 2)};
    tag = trim_js(tag);
    Token t = {T_TAG, tag, 0, 0};
    tokenvec_push(out, t);
    i = standalone ? (nl < 0 ? src.n : (u32)nl + 1) : end;
  }
  if (text.n > 0) {
    Token t = {T_TEXT, buf_str_view(&text), 0, 0};
    tokenvec_push(out, t);
  }
  return 1;
}

// Tag matchers -----------------------------------------------------------

static int ident_ok(Str s, u32 a, u32 b) {
  if (b <= a) return 0;
  u8 c0 = s.p[a];
  if (!((c0 >= 'A' && c0 <= 'Z') || (c0 >= 'a' && c0 <= 'z') || c0 == '_'))
    return 0;
  for (u32 i = a + 1; i < b; i++) {
    u8 c = s.p[i];
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9') || c == '_'))
      return 0;
  }
  return 1;
}

static int has_ws(Str s) {
  u32 i = 0;
  while (i < s.n) {
    u32 len;
    u32 cp = utf8_decode(s.p, s.n, i, &len);
    if (is_space_cp(cp)) return 1;
    i += len;
  }
  return 0;
}

// "for NAME in PATH" — returns 1 with name/path set.
static int tag_for(Str tag, Str *name, Str *path) {
  if (!str_starts(tag, "for ")) return 0;
  u32 i = 4;
  u32 start = i;
  while (i < tag.n && tag.p[i] != ' ') i++;
  if (!ident_ok(tag, start, i)) return 0;
  if (i + 4 > tag.n || memcmp(tag.p + i, " in ", 4) != 0) return 0;
  Str rest = {tag.p + i + 4, tag.n - (i + 4)};
  if (rest.n == 0 || has_ws(rest)) return 0;
  *name = (Str){tag.p + start, i - start};
  *path = rest;
  return 1;
}

// "if [not] PATH" — returns 1 with not_flag/path set.
static int tag_if(Str tag, int *not_flag, Str *path) {
  if (!str_starts(tag, "if ")) return 0;
  Str rest = {tag.p + 3, tag.n - 3};
  *not_flag = 0;
  if (str_starts(rest, "not ")) {
    *not_flag = 1;
    rest.p += 4;
    rest.n -= 4;
  }
  if (rest.n == 0 || has_ws(rest)) return 0;
  *path = rest;
  return 1;
}

// 'include "name"' with name [a-z0-9-]+.
static int tag_include(Str tag, Str *name) {
  if (!str_starts(tag, "include \"")) return 0;
  if (tag.n < 10 || tag.p[tag.n - 1] != '"') return 0;
  Str inner = {tag.p + 9, tag.n - 10};
  if (inner.n == 0) return 0;
  for (u32 i = 0; i < inner.n; i++) {
    u8 c = inner.p[i];
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'))
      return 0;
  }
  *name = inner;
  return 1;
}

// Parse a template into a node list; 0 with errbuf on failure.
static int parse_template(Str name, Str src, Node ***nodes_out, u32 *n_out,
                          Buf *errbuf) {
  TokenVec toks = {0, 0, 0};
  if (!tpl_tokenize(name, src, &toks, errbuf)) return 0;
  Node *root = node_new(0);  // kind 0 unused; body holds the top list
  typedef struct {
    Node *node;
    int kind;  // 0 root, N_FOR, N_IF
  } Frame;
  Frame stack[256];
  u32 nstack = 1;
  stack[0] = (Frame){root, 0};
  for (u32 ti = 0; ti < toks.n; ti++) {
    Token *tok = &toks.items[ti];
    Frame *top = &stack[nstack - 1];
    Node ***arr;
    u32 *cnt, *cap;
    if (top->kind == 0) {
      arr = &top->node->body;
      cnt = &top->node->nbody;
      cap = &top->node->bodycap;
    } else if (top->kind == N_FOR) {
      arr = &top->node->body;
      cnt = &top->node->nbody;
      cap = &top->node->bodycap;
    } else if (!top->node->has_else) {
      arr = &top->node->thenb;
      cnt = &top->node->nthen;
      cap = &top->node->thencap;
    } else {
      arr = &top->node->elseb;
      cnt = &top->node->nelse;
      cap = &top->node->elsecap;
    }
    if (tok->kind == T_TEXT) {
      Node *nd = node_new(N_TEXT);
      nd->text = tok->text;
      ptrvec_push(arr, cnt, cap, nd);
    } else if (tok->kind == T_VAR || tok->kind == T_RAW) {
      Node *nd = node_new(tok->kind == T_VAR ? N_VAR : N_RAW);
      nd->path = tok->path;
      nd->npath = tok->npath;
      ptrvec_push(arr, cnt, cap, nd);
    } else {
      Str tag = tok->text;
      Str fname, fpath;
      int inot;
      Str ipath;
      if (tag_for(tag, &fname, &fpath)) {
        Str *segs;
        u32 nseg;
        if (!tpl_path_of(fpath, &segs, &nseg)) {
          tpl_error_path(errbuf, name, fpath);
          return 0;
        }
        Node *nd = node_new(N_FOR);
        nd->name = fname;
        nd->path = segs;
        nd->npath = nseg;
        ptrvec_push(arr, cnt, cap, nd);
        stack[nstack++] = (Frame){nd, N_FOR};
      } else if (tag_if(tag, &inot, &ipath)) {
        Str *segs;
        u32 nseg;
        if (!tpl_path_of(ipath, &segs, &nseg)) {
          tpl_error_path(errbuf, name, ipath);
          return 0;
        }
        Node *nd = node_new(N_IF);
        nd->not_flag = inot;
        nd->path = segs;
        nd->npath = nseg;
        ptrvec_push(arr, cnt, cap, nd);
        stack[nstack++] = (Frame){nd, N_IF};
      } else if (str_eq_c(tag, "else")) {
        if (top->kind != N_IF || top->node->has_else) {
          tpl_error(errbuf, name, "an else outside an if");
          return 0;
        }
        top->node->has_else = 1;
      } else if (str_eq_c(tag, "endfor") || str_eq_c(tag, "endif")) {
        int want = str_eq_c(tag, "endfor") ? N_FOR : N_IF;
        if (top->kind != want) {
          errbuf->n = 0;
          buf_c(errbuf, "template \"");
          buf_str(errbuf, name);
          buf_c(errbuf, "\": an ");
          buf_str(errbuf, tag);
          buf_c(errbuf, " with no open ");
          buf_c(errbuf, want == N_FOR ? "for" : "if");
          return 0;
        }
        nstack--;
      } else if (tag_include(tag, &fname)) {
        Node *nd = node_new(N_INCLUDE);
        nd->name = fname;
        ptrvec_push(arr, cnt, cap, nd);
      } else {
        errbuf->n = 0;
        buf_c(errbuf, "template \"");
        buf_str(errbuf, name);
        buf_c(errbuf, "\": {% ");
        buf_str(errbuf, tag);
        buf_c(errbuf, " %} is not a tag");
        return 0;
      }
    }
  }
  if (nstack > 1) {
    Frame *top = &stack[nstack - 1];
    errbuf->n = 0;
    buf_c(errbuf, "template \"");
    buf_str(errbuf, name);
    buf_c(errbuf, "\": a ");
    buf_c(errbuf, top->kind == N_FOR ? "for" : "if");
    buf_c(errbuf, " is not closed");
    return 0;
  }
  *nodes_out = root->body;
  *n_out = root->nbody;
  return 1;
}

// ------------------------------------------------------------ template values

enum { V_NONE, V_STR, V_INT, V_BOOL, V_ARR, V_OBJ };

typedef struct Value Value;
typedef struct {
  Str key;
  Value *val;
} Pair;

struct Value {
  int kind;
  Str s;             // V_STR
  i32 num;           // V_INT / V_BOOL
  Value **items;     // V_ARR
  u32 nitems;
  Pair *pairs;       // V_OBJ
  u32 npairs;
};

static Value *val_new(int kind) {
  Value *v = (Value *)arena_alloc_zero(sizeof(Value), 8);
  v->kind = kind;
  return v;
}

static Value *val_str(Str s) {
  Value *v = val_new(V_STR);
  v->s = s;
  return v;
}

static Value *val_int(i32 n) {
  Value *v = val_new(V_INT);
  v->num = n;
  return v;
}

static Value *val_bool(int b) {
  Value *v = val_new(V_BOOL);
  v->num = b;
  return v;
}

static Value *val_arr(Value **items, u32 n) {
  Value *v = val_new(V_ARR);
  v->items = items;
  v->nitems = n;
  return v;
}

static Value *val_obj(Pair *pairs, u32 n) {
  Value *v = val_new(V_OBJ);
  v->pairs = pairs;
  v->npairs = n;
  return v;
}

static Value *obj_get(Value *o, Str key) {
  if (!o || o->kind != V_OBJ) return 0;
  for (u32 i = 0; i < o->npairs; i++)
    if (str_eq(o->pairs[i].key, key)) return o->pairs[i].val;
  return 0;
}

static int obj_has(Value *o, Str key) {
  if (!o || o->kind != V_OBJ) return 0;
  for (u32 i = 0; i < o->npairs; i++)
    if (str_eq(o->pairs[i].key, key)) return 1;
  return 0;
}

// The value at path in the scope stack (scopes are V_OBJ values,
// innermost last); 0 when absent.
// The pair index of key in o, or -1 (one scan; obj_get/obj_has each
// scanned again, and tpl_lookup used to pay that per segment).
static i32 obj_find(Value *o, Str key) {
  if (!o || o->kind != V_OBJ) return -1;
  for (u32 i = 0; i < o->npairs; i++)
    if (str_eq(o->pairs[i].key, key)) return (i32)i;
  return -1;
}

static Value *tpl_lookup(Value **scopes, u32 nscopes, Str *path, u32 npath) {
  for (i32 s = (i32)nscopes - 1; s >= 0; s--) {
    Value *scope = scopes[s];
    i32 i0 = obj_find(scope, path[0]);
    if (i0 < 0) continue;
    Value *v = scope->pairs[i0].val;
    int miss = 0;
    for (u32 k = 1; k < npath; k++) {
      i32 ik = obj_find(v, path[k]);
      if (ik < 0) {
        miss = 1;
        break;
      }
      v = v->pairs[ik].val;
    }
    return miss ? 0 : v;
  }
  return 0;
}

static void tpl_text_of(Value *v, Buf *out) {
  if (!v) return;
  switch (v->kind) {
    case V_STR:
      buf_str(out, v->s);
      break;
    case V_INT: {
      if (v->num < 0) {
        buf_ch(out, '-');
        buf_u32dec(out, (u32)(-(v->num + 1)) + 1);
      } else {
        buf_u32dec(out, (u32)v->num);
      }
      break;
    }
    case V_BOOL:
      buf_c(out, v->num ? "true" : "false");
      break;
    case V_ARR:
      for (u32 i = 0; i < v->nitems; i++) {
        if (i) buf_c(out, ", ");
        tpl_text_of(v->items[i], out);
      }
      break;
    default:
      break;
  }
}

static int tpl_truthy(Value *v) {
  if (!v) return 0;
  switch (v->kind) {
    case V_ARR: return v->nitems > 0;
    case V_STR: return v->s.n > 0;
    case V_INT: return v->num != 0;
    case V_BOOL: return v->num != 0;
    case V_OBJ: return 1;
  }
  return 0;
}

#define INCLUDE_DEPTH 8

typedef struct {
  Map *by_name;    // template name -> index into the templates array
  Node ***roots;   // per index: node list (array of Node*)
  u32 *counts;     // per index: node count
} TplSet;

static void render_nodes(Node **nodes, u32 n, Value **scopes, u32 nscopes,
                         TplSet *tpls, int depth, Buf *out) {
  for (u32 idx = 0; idx < n; idx++) {
    Node *nd = nodes[idx];
    switch (nd->kind) {
      case N_TEXT:
        buf_str(out, nd->text);
        break;
      case N_VAR: {
        // Fast path: a plain string value escapes straight from its
        // storage; anything else renders to a scratch buffer first
        // (the general path, unchanged).
        Value *v = tpl_lookup(scopes, nscopes, nd->path, nd->npath);
        if (v && v->kind == V_STR) {
          buf_escape_html(out, v->s);
        } else {
          Buf tmp;
          buf_init(&tmp);
          tpl_text_of(v, &tmp);
          buf_escape_html(out, buf_str_view(&tmp));
        }
        break;
      }
      case N_RAW:
        tpl_text_of(tpl_lookup(scopes, nscopes, nd->path, nd->npath), out);
        break;
      case N_IF: {
        int v = tpl_truthy(tpl_lookup(scopes, nscopes, nd->path, nd->npath));
        if (v != nd->not_flag) {
          if (nd->nthen)
            render_nodes(nd->thenb, nd->nthen, scopes, nscopes, tpls, depth,
                         out);
        } else if (nd->has_else && nd->nelse) {
          render_nodes(nd->elseb, nd->nelse, scopes, nscopes, tpls, depth,
                       out);
        }
        break;
      }
      case N_FOR: {
        Value *xs = tpl_lookup(scopes, nscopes, nd->path, nd->npath);
        if (!xs || xs->kind != V_ARR) break;
        for (u32 k = 0; k < xs->nitems; k++) {
          Pair *loop_pairs = (Pair *)arena_alloc(sizeof(Pair) * 4, 8);
          loop_pairs[0] = (Pair){str_of("index"), val_int((i32)k + 1)};
          loop_pairs[1] = (Pair){str_of("first"), val_bool(k == 0)};
          loop_pairs[2] = (Pair){str_of("last"),
                                 val_bool(k == xs->nitems - 1)};
          loop_pairs[3] = (Pair){str_of("length"),
                                 val_int((i32)xs->nitems)};
          Pair *frame_pairs = (Pair *)arena_alloc(sizeof(Pair) * 2, 8);
          frame_pairs[0] = (Pair){nd->name, xs->items[k]};
          frame_pairs[1] = (Pair){str_of("loop"), val_obj(loop_pairs, 4)};
          scopes[nscopes] = val_obj(frame_pairs, 2);
          render_nodes(nd->body, nd->nbody, scopes, nscopes + 1, tpls,
                       depth, out);
        }
        break;
      }
      case N_INCLUDE: {
        if (depth < INCLUDE_DEPTH) {
          i32 ti = map_get(tpls->by_name, nd->name);
          if (ti >= 0)
            render_nodes(tpls->roots[ti], tpls->counts[ti], scopes, nscopes,
                         tpls, depth + 1, out);
        }
        break;
      }
    }
  }
}

// ------------------------------------------------------------ the build

struct Source {
  Str path;
  Str text;  // line-ending-normalised content (.md sources)
  FMData data;
  Str body;
  int is_post;
  Str layout;
  Str slug;
  Str out_dir;
  Str url;
  Str html;
  TocEntry *toc;
  u32 ntoc;
  Str excerpt;
  i32 newer, older;      // source indices among posts, -1 when none
  i32 *tag_refs;         // indices into the tag array
  u32 ntag_refs;
  Value *card;           // built once
};

static Source *g_sources = 0;
static Str source_url_of(i32 idx) { return g_sources[idx].url; }

typedef struct {
  Str name, slug, url;
  i32 *posts;
  u32 nposts, capposts;
} Tag;

static Str dir_of(Str path) {
  i32 last = -1;
  for (u32 i = 0; i < path.n; i++)
    if (path.p[i] == '/') last = (i32)i;
  if (last < 0) return (Str){path.p, 0};
  return (Str){path.p, (u32)last};
}

// Stable merge sort of an index array with a global comparator.
static int (*g_cmp)(i32, i32);
static void msort_rec(i32 *a, i32 *tmp, u32 lo, u32 hi) {
  if (hi - lo < 2) return;
  u32 mid = (lo + hi) / 2;
  msort_rec(a, tmp, lo, mid);
  msort_rec(a, tmp, mid, hi);
  u32 i = lo, j = mid, k = lo;
  while (i < mid && j < hi) tmp[k++] = g_cmp(a[i], a[j]) <= 0 ? a[i++] : a[j++];
  while (i < mid) tmp[k++] = a[i++];
  while (j < hi) tmp[k++] = a[j++];
  for (i = lo; i < hi; i++) a[i] = tmp[i];
}
static void sort_indices(i32 *a, u32 n, int (*cmp)(i32, i32)) {
  if (n < 2) return;
  i32 *tmp = (i32 *)arena_alloc(sizeof(i32) * n, 4);
  g_cmp = cmp;
  msort_rec(a, tmp, 0, n);
}

// slugify with the per-case memo.
static Str *g_slug_list = 0;
static u32 g_slug_count = 0, g_slug_cap = 0;
static Map *g_slug_cache = 0;

static Str slugify_cached(Map *cache, Str text) {
  (void)cache;
  i32 idx = map_get(g_slug_cache, text);
  if (idx >= 0) return g_slug_list[idx];
  Buf b;
  buf_init(&b);
  slugify_into(&b, text);
  Str s = buf_str_view(&b);
  if (g_slug_count == g_slug_cap) {
    u32 ncap = g_slug_cap ? g_slug_cap * 2 : 64;
    Str *nl = (Str *)arena_alloc(sizeof(Str) * ncap, 8);
    if (g_slug_list) memcpy(nl, g_slug_list, sizeof(Str) * g_slug_count);
    g_slug_list = nl;
    g_slug_cap = ncap;
  }
  map_set(g_slug_cache, text, (i32)g_slug_count);
  g_slug_list[g_slug_count++] = s;
  return s;
}

// Long-date memo.
static Map *g_date_cache = 0;
static Str *g_date_list = 0;
static u32 g_date_count = 0, g_date_cap = 0;

static Str long_date_cached(Str date) {
  i32 idx = map_get(g_date_cache, date);
  if (idx >= 0) return g_date_list[idx];
  Buf b;
  buf_init(&b);
  long_date_into(&b, date);
  Str s = buf_str_view(&b);
  if (g_date_count == g_date_cap) {
    u32 ncap = g_date_cap ? g_date_cap * 2 : 32;
    Str *nl = (Str *)arena_alloc(sizeof(Str) * ncap, 8);
    if (g_date_list) memcpy(nl, g_date_list, sizeof(Str) * g_date_count);
    g_date_list = nl;
    g_date_cap = ncap;
  }
  map_set(g_date_cache, date, (i32)g_date_count);
  g_date_list[g_date_count++] = s;
  return s;
}

// A report under construction: line views in generation order.
typedef struct {
  Str *items;
  u32 n, cap;
} StrVec;

static void strvec_push(StrVec *v, Str s) {
  if (v->n == v->cap) {
    u32 ncap = v->cap ? v->cap * 2 : 32;
    Str *ni = (Str *)arena_alloc(sizeof(Str) * ncap, 8);
    if (v->items) memcpy(ni, v->items, sizeof(Str) * v->n);
    v->items = ni;
    v->cap = ncap;
  }
  v->items[v->n++] = s;
}

// ------------------------------------------------------------ pack input

typedef struct {
  const u8 *p;
  u32 n, pos;
} Pack;

static u32 pack_u32(Pack *pk) {
  u32 v = (u32)pk->p[pk->pos] | ((u32)pk->p[pk->pos + 1] << 8) |
          ((u32)pk->p[pk->pos + 2] << 16) | ((u32)pk->p[pk->pos + 3] << 24);
  pk->pos += 4;
  return v;
}

static Str pack_str(Pack *pk) {
  u32 n = pack_u32(pk);
  Str s = {pk->p + pk->pos, n};
  pk->pos += n;
  return s;
}

// ------------------------------------------------------------ outputs

typedef struct {
  Str path, text;
} OutPair;

typedef struct {
  OutPair *items;
  u32 n, cap;
  Map by_path;
} Outputs;

static void outputs_init(Outputs *o) {
  o->items = 0;
  o->n = 0;
  o->cap = 0;
  map_init(&o->by_path, 12);
}

static void outputs_add(Outputs *o, Str path, Str text) {
  i32 idx = map_get(&o->by_path, path);
  if (idx >= 0) {
    o->items[idx].text = text;
    return;
  }
  if (o->n == o->cap) {
    u32 ncap = o->cap ? o->cap * 2 : 64;
    OutPair *ni = (OutPair *)arena_alloc(sizeof(OutPair) * ncap, 8);
    if (o->items) memcpy(ni, o->items, sizeof(OutPair) * o->n);
    o->items = ni;
    o->cap = ncap;
  }
  map_set(&o->by_path, path, (i32)o->n);
  o->items[o->n].path = path;
  o->items[o->n].text = text;
  o->n++;
}

static int outputs_has(Outputs *o, Str path) {
  return map_get(&o->by_path, path) >= 0;
}

// ------------------------------------------------------------ comparators

static Source *C;  // sources array for comparators
static Tag *T;     // tags array for comparators

static int cmp_posts(i32 a, i32 b) {
  int c = str_cmp(C[b].data.date, C[a].data.date);
  if (c != 0) return c;
  return str_cmp(C[a].slug, C[b].slug);
}

static StrVec *g_lines;
static int cmp_lines(i32 a, i32 b) {
  return str_cmp(g_lines->items[a], g_lines->items[b]);
}

static int cmp_tags(i32 a, i32 b) {
  if (T[a].nposts != T[b].nposts) return T[a].nposts > T[b].nposts ? -1 : 1;
  return str_cmp(T[a].slug, T[b].slug);
}

// ------------------------------------------------------------ value builders

static Value *g_none = 0;

static Value *link_value(i32 src_idx) {
  if (src_idx < 0) return g_none;
  Source *s = &C[src_idx];
  Pair *pairs = (Pair *)arena_alloc(sizeof(Pair) * 4, 8);
  pairs[0] = (Pair){str_of("title"), val_str(s->data.title)};
  pairs[1] = (Pair){str_of("url"), val_str(s->url)};
  pairs[2] = (Pair){str_of("date"),
                    val_str(s->data.has_date ? s->data.date : str_of(""))};
  pairs[3] = (Pair){str_of("date_long"),
                    val_str(s->data.has_date ? long_date_cached(s->data.date)
                                             : str_of(""))};
  return val_obj(pairs, 4);
}

static Tag *g_tags = 0;

static Value *card_value(i32 src_idx) {
  Source *s = &C[src_idx];
  if (s->card) return s->card;
  Pair *pairs = (Pair *)arena_alloc(sizeof(Pair) * 7, 8);
  pairs[0] = (Pair){str_of("title"), val_str(s->data.title)};
  pairs[1] = (Pair){str_of("url"), val_str(s->url)};
  pairs[2] = (Pair){str_of("date"),
                    val_str(s->data.has_date ? s->data.date : str_of(""))};
  pairs[3] = (Pair){str_of("date_long"),
                    val_str(s->data.has_date ? long_date_cached(s->data.date)
                                             : str_of(""))};
  pairs[4] = (Pair){str_of("summary"),
                    val_str(s->data.has_summary ? s->data.summary
                                                : str_of(""))};
  pairs[5] = (Pair){str_of("excerpt"), val_str(s->excerpt)};
  u32 words = count_words(s->body);
  u32 mins = (words + 199) / 200;
  if (mins < 1) mins = 1;
  pairs[6] = (Pair){str_of("reading_minutes"), val_int((i32)mins)};
  // tags array is a separate pair appended below (8 pairs total with it)
  Pair *all = (Pair *)arena_alloc(sizeof(Pair) * 8, 8);
  memcpy(all, pairs, sizeof(Pair) * 7);
  Value **titems =
      (Value **)arena_alloc(sizeof(Value *) * (s->ntag_refs ? s->ntag_refs : 1), 8);
  for (u32 i = 0; i < s->ntag_refs; i++) {
    Tag *t = &g_tags[s->tag_refs[i]];
    Pair *tp = (Pair *)arena_alloc(sizeof(Pair) * 3, 8);
    tp[0] = (Pair){str_of("name"), val_str(t->name)};
    tp[1] = (Pair){str_of("slug"), val_str(t->slug)};
    tp[2] = (Pair){str_of("url"), val_str(t->url)};
    titems[i] = val_obj(tp, 3);
  }
  all[7] = (Pair){str_of("tags"), val_arr(titems, s->ntag_refs)};
  s->card = val_obj(all, 8);
  return s->card;
}

static Value *page_value(i32 src_idx) {
  Source *s = &C[src_idx];
  Value *card = card_value(src_idx);
  Pair *pairs = (Pair *)arena_alloc(sizeof(Pair) * 14, 8);
  u32 n = 0;
  for (u32 i = 0; i < card->npairs; i++) pairs[n++] = card->pairs[i];
  pairs[n++] = (Pair){str_of("slug"), val_str(s->slug)};
  pairs[n++] = (Pair){str_of("content"), val_str(s->html)};
  Value **toc_items =
      (Value **)arena_alloc(sizeof(Value *) * (s->ntoc ? s->ntoc : 1), 8);
  for (u32 i = 0; i < s->ntoc; i++) {
    Pair *tp = (Pair *)arena_alloc(sizeof(Pair) * 3, 8);
    tp[0] = (Pair){str_of("level"), val_int(s->toc[i].level)};
    tp[1] = (Pair){str_of("id"), val_str(s->toc[i].id)};
    tp[2] = (Pair){str_of("html"), val_str(s->toc[i].html)};
    toc_items[i] = val_obj(tp, 3);
  }
  pairs[n++] = (Pair){str_of("toc"), val_arr(toc_items, s->ntoc)};
  pairs[n++] = (Pair){str_of("newer"), link_value(s->newer)};
  pairs[n++] = (Pair){str_of("older"), link_value(s->older)};
  pairs[n++] = (Pair){str_of("is_post"), val_bool(s->is_post)};
  return val_obj(pairs, n);
}

// ------------------------------------------------------------ the build itself

// One parsed case, ready to build. Arrays are in code-unit order of
// template name / file path (the caller sorts).
typedef struct {
  Str *tpl_names, *tpl_srcs;
  u32 n_templates;
  Str site_title, site_base_raw;
  u32 per_page, recent_n;
  Str *file_paths, *file_contents;
  u32 n_files;
} CaseIn;

static void build_core(CaseIn *in, u32 mode, Buf *result) {
  u32 n_templates = in->n_templates;
  Str *tpl_names = in->tpl_names;
  Str *tpl_srcs = in->tpl_srcs;
  Str site_title = in->site_title;
  Str site_base_raw = in->site_base_raw;
  u32 per_page = in->per_page;
  u32 recent_n = in->recent_n;
  u32 n_files = in->n_files;
  Str *file_paths = in->file_paths;
  Str *file_contents = in->file_contents;

  StrVec report = {0, 0, 0};

  // Templates in the given (name-sorted) order.
  Map tpl_by_name;
  map_init(&tpl_by_name, 6);
  Node ***tpl_lists = (Node ***)arena_alloc(sizeof(Node **) * (n_templates ? n_templates : 1), 8);
  u32 *tpl_counts = (u32 *)arena_alloc(sizeof(u32) * (n_templates ? n_templates : 1), 4);
  TplSet tpls;
  tpls.by_name = &tpl_by_name;
  tpls.roots = tpl_lists;
  tpls.counts = tpl_counts;
  for (u32 i = 0; i < n_templates; i++) {
    Buf err;
    buf_init(&err);
    Node **nodes;
    u32 nn;
    if (parse_template(tpl_names[i], tpl_srcs[i], &nodes, &nn, &err)) {
      map_set(&tpl_by_name, tpl_names[i], (i32)i);
      tpl_lists[i] = nodes;
      tpl_counts[i] = nn;
    } else {
      strvec_push(&report, buf_str_view(&err));
      tpl_lists[i] = 0;
      tpl_counts[i] = 0;
    }
  }

  Str base = site_base_raw;
  while (base.n > 0 && base.p[base.n - 1] == '/') base.n--;

  // Split files into sources (.md, normalised) and statics (raw).
  Source *sources = (Source *)arena_alloc_zero(sizeof(Source) * (n_files ? n_files : 1), 8);
  u32 n_sources_raw = 0;
  typedef struct {
    Str path, content;
  } StaticF;
  StaticF *statics = (StaticF *)arena_alloc(sizeof(StaticF) * (n_files ? n_files : 1), 8);
  u32 n_statics = 0;
  for (u32 i = 0; i < n_files; i++) {
    if (str_ends(file_paths[i], ".md")) {
      Source *s = &sources[n_sources_raw++];
      s->path = file_paths[i];
      Buf nb;
      buf_init(&nb);
      Str c = file_contents[i];
      for (u32 k = 0; k < c.n; k++) {
        if (c.p[k] == '\r') {
          buf_ch(&nb, '\n');
          if (k + 1 < c.n && c.p[k + 1] == '\n') k++;
        } else {
          buf_ch(&nb, c.p[k]);
        }
      }
      s->text = buf_str_view(&nb);
      s->newer = -1;
      s->older = -1;
    } else {
      statics[n_statics].path = file_paths[i];
      statics[n_statics].content = file_contents[i];
      n_statics++;
    }
  }

  // Per-case memos.
  Map slug_cache;
  map_init(&slug_cache, 13);
  g_slug_cache = &slug_cache;
  g_slug_list = 0;
  g_slug_count = 0;
  g_slug_cap = 0;
  Map date_cache;
  map_init(&date_cache, 11);
  g_date_cache = &date_cache;
  g_date_list = 0;
  g_date_count = 0;
  g_date_cap = 0;
  g_none = val_new(V_NONE);

  // Kept filter.
  Source *kept = (Source *)arena_alloc_zero(sizeof(Source) * (n_sources_raw ? n_sources_raw : 1), 8);
  u32 n_kept = 0;
  for (u32 i = 0; i < n_sources_raw; i++) {
    Source *s = &sources[i];
    Buf err;
    buf_init(&err);
    Str body;
    int fm_ok = parse_front_matter(s->text, &s->data, &body, &err);
    if (!fm_ok) {
      Buf line;
      buf_init(&line);
      buf_c(&line, "refused ");
      buf_str(&line, s->path);
      buf_c(&line, ": ");
      buf_str(&line, buf_str_view(&err));
      strvec_push(&report, buf_str_view(&line));
      continue;
    }
    s->body = body;
    s->is_post = str_starts(s->path, "posts/");
    const char *own = 0;
    if (str_starts(s->path, "tags/")) own = "tags";
    else if (str_starts(s->path, "page/")) own = "page";
    if (own) {
      Buf line;
      buf_init(&line);
      buf_c(&line, "refused ");
      buf_str(&line, s->path);
      buf_c(&line, ": ");
      buf_c(&line, own);
      buf_c(&line, "/ is the generator's own");
      strvec_push(&report, buf_str_view(&line));
      continue;
    }
    if (s->is_post && !s->data.has_date) {
      Buf line;
      buf_init(&line);
      buf_c(&line, "refused ");
      buf_str(&line, s->path);
      buf_c(&line, ": a post needs a date");
      strvec_push(&report, buf_str_view(&line));
      continue;
    }
    Str layout = s->data.has_layout ? s->data.layout
                                    : str_of(s->is_post ? "post" : "page");
    if (map_get(&tpl_by_name, layout) < 0) {
      Buf line;
      buf_init(&line);
      buf_c(&line, "refused ");
      buf_str(&line, s->path);
      buf_c(&line, ": layout \"");
      buf_str(&line, layout);
      buf_c(&line, "\" is no template that parses");
      strvec_push(&report, buf_str_view(&line));
      continue;
    }
    s->layout = layout;
    if (s->data.has_draft && s->data.draft) {
      Buf line;
      buf_init(&line);
      buf_c(&line, "draft ");
      buf_str(&line, s->path);
      strvec_push(&report, buf_str_view(&line));
      continue;
    }
    kept[n_kept++] = *s;
  }
  sources = kept;
  g_sources = sources;
  C = sources;
  u32 n_sources = n_kept;

  // Permalinks.
  Map dir_sets;  // dir -> index into an array of Map*
  map_init(&dir_sets, 11);
  Map **set_list = (Map **)arena_alloc(sizeof(Map *) * (n_sources + 1), 8);
  u32 n_sets = 0;
  Map by_source;
  map_init(&by_source, 11);
  for (u32 i = 0; i < n_sources; i++) {
    Source *s = &sources[i];
    Str dir = s->is_post ? str_of("posts") : dir_of(s->path);
    i32 set_idx = map_get(&dir_sets, dir);
    Map *set;
    if (set_idx < 0) {
      set = (Map *)arena_alloc(sizeof(Map), 8);
      map_init(set, 11);
      if (dir.n == 0) {
        map_set(set, str_of("posts"), 0);
        map_set(set, str_of("tags"), 0);
        map_set(set, str_of("page"), 0);
      }
      set_list[n_sets] = set;
      map_set(&dir_sets, dir, (i32)n_sets);
      n_sets++;
    } else {
      set = set_list[set_idx];
    }
    Str slug = s->data.has_slug ? s->data.slug
                                : slugify_cached(&slug_cache, s->data.title);
    Buf cand;
    buf_init(&cand);
    buf_str(&cand, slug);
    u32 nn = 2;
    while (map_get(set, buf_str_view(&cand)) >= 0) {
      cand.n = slug.n;
      buf_ch(&cand, '-');
      buf_u32dec(&cand, nn++);
    }
    Str final_slug = buf_str_view(&cand);
    map_set(set, final_slug, 0);
    s->slug = final_slug;
    Buf od;
    buf_init(&od);
    if (dir.n > 0) {
      buf_str(&od, dir);
      buf_ch(&od, '/');
    }
    buf_str(&od, final_slug);
    s->out_dir = buf_str_view(&od);
    Buf ub;
    buf_init(&ub);
    buf_str(&ub, base);
    buf_ch(&ub, '/');
    buf_str(&ub, s->out_dir);
    buf_ch(&ub, '/');
    s->url = buf_str_view(&ub);
    map_set(&by_source, s->path, (i32)i);
  }

  // Markdown.
  Map static_paths;
  map_init(&static_paths, 10);
  for (u32 i = 0; i < n_statics; i++) map_set(&static_paths, statics[i].path, 0);
  StrVec broken_lines = {0, 0, 0};
  for (u32 i = 0; i < n_sources; i++) {
    Source *s = &sources[i];
    Buf broken;
    buf_init(&broken);
    LinkCtx lc;
    lc.dir = dir_of(s->path);
    lc.source_path = s->path;
    lc.base = base;
    lc.by_source = &by_source;
    lc.static_paths = &static_paths;
    lc.broken = &broken;
    lc.nbroken = 0;
    Buf html;
    buf_init(&html);
    TocEntry *toc;
    u32 ntoc;
    Str excerpt;
    render_markdown(s->body, &lc, &slug_cache, &html, &toc, &ntoc, &excerpt);
    s->html = buf_str_view(&html);
    s->toc = toc;
    s->ntoc = ntoc;
    s->excerpt = excerpt;
    Buf **ptrs = (Buf **)broken.p;
    for (u32 k = 0; k < lc.nbroken; k++)
      strvec_push(&broken_lines, buf_str_view(ptrs[k]));
  }

  // Posts and their order.
  i32 *post_idx = (i32 *)arena_alloc(sizeof(i32) * (n_sources ? n_sources : 1), 4);
  u32 n_posts = 0;
  for (u32 i = 0; i < n_sources; i++)
    if (sources[i].is_post) post_idx[n_posts++] = (i32)i;
  sort_indices(post_idx, n_posts, cmp_posts);
  for (u32 k = 0; k < n_posts; k++) {
    sources[post_idx[k]].newer = k == 0 ? -1 : post_idx[k - 1];
    sources[post_idx[k]].older = k == n_posts - 1 ? -1 : post_idx[k + 1];
  }

  // Tags.
  u32 tag_cap = 16;
  for (u32 i = 0; i < n_sources; i++)
    if (sources[i].data.has_tags) tag_cap += sources[i].data.ntags;
  Tag *tags = (Tag *)arena_alloc_zero(sizeof(Tag) * tag_cap, 8);
  g_tags = tags;
  T = tags;
  u32 n_tags = 0;
  Map tag_by_slug;
  map_init(&tag_by_slug, 12);
  // own tags per source (names deduped by slug), computed on demand below
  for (u32 pk = 0; pk < n_posts; pk++) {
    Source *s = &sources[post_idx[pk]];
    u32 cap = s->data.has_tags ? s->data.ntags : 0;
    i32 *refs = (i32 *)arena_alloc(sizeof(i32) * (cap ? cap : 1), 4);
    u32 nrefs = 0;
    Map seen;
    map_init(&seen, 8);
    for (u32 ti = 0; ti < cap; ti++) {
      Str name = s->data.tags[ti];
      Str slug = slugify_cached(&slug_cache, name);
      if (map_get(&seen, slug) >= 0) continue;
      map_set(&seen, slug, 0);
      i32 tidx = map_get(&tag_by_slug, slug);
      if (tidx < 0) {
        tidx = (i32)n_tags;
        Tag *t = &tags[n_tags++];
        t->name = name;
        t->slug = slug;
        Buf ub;
        buf_init(&ub);
        buf_str(&ub, base);
        buf_c(&ub, "/tags/");
        buf_str(&ub, slug);
        buf_ch(&ub, '/');
        t->url = buf_str_view(&ub);
        t->posts = 0;
        t->nposts = 0;
        t->capposts = 0;
        map_set(&tag_by_slug, slug, tidx);
      }
      Tag *t = &tags[tidx];
      if (t->nposts == t->capposts) {
        u32 ncap = t->capposts ? t->capposts * 2 : 8;
        i32 *np = (i32 *)arena_alloc(sizeof(i32) * ncap, 4);
        if (t->posts) memcpy(np, t->posts, sizeof(i32) * t->nposts);
        t->posts = np;
        t->capposts = ncap;
      }
      t->posts[t->nposts++] = post_idx[pk];
      refs[nrefs++] = tidx;
    }
    s->tag_refs = refs;
    s->ntag_refs = nrefs;
  }
  u32 n_real_tags = n_tags;
  for (u32 i = 0; i < n_sources; i++) {
    Source *s = &sources[i];
    if (s->is_post) continue;
    u32 cap = s->data.has_tags ? s->data.ntags : 0;
    i32 *refs = (i32 *)arena_alloc(sizeof(i32) * (cap ? cap : 1), 4);
    u32 nrefs = 0;
    Map seen;
    map_init(&seen, 8);
    for (u32 ti = 0; ti < cap; ti++) {
      Str name = s->data.tags[ti];
      Str slug = slugify_cached(&slug_cache, name);
      if (map_get(&seen, slug) >= 0) continue;
      map_set(&seen, slug, 0);
      i32 tidx = map_get(&tag_by_slug, slug);
      if (tidx < 0) {
        tidx = (i32)n_tags;
        Tag *t = &tags[n_tags++];
        t->name = name;
        t->slug = slug;
        t->url = str_of("");
        t->posts = 0;
        t->nposts = 0;
        t->capposts = 0;
      }
      refs[nrefs++] = tidx;
    }
    s->tag_refs = refs;
    s->ntag_refs = nrefs;
  }
  i32 *tag_order = (i32 *)arena_alloc(sizeof(i32) * (n_real_tags ? n_real_tags : 1), 4);
  for (u32 i = 0; i < n_real_tags; i++) tag_order[i] = (i32)i;
  sort_indices(tag_order, n_real_tags, cmp_tags);

  // Shared template values.
  Pair *site_pairs = (Pair *)arena_alloc(sizeof(Pair) * 2, 8);
  site_pairs[0] = (Pair){str_of("title"), val_str(site_title)};
  site_pairs[1] = (Pair){str_of("base_url"), val_str(base)};
  Value *site_val = val_obj(site_pairs, 2);
  Value **all_tags = (Value **)arena_alloc(sizeof(Value *) * (n_real_tags ? n_real_tags : 1), 8);
  for (u32 i = 0; i < n_real_tags; i++) {
    Tag *t = &tags[tag_order[i]];
    Pair *tp = (Pair *)arena_alloc(sizeof(Pair) * 4, 8);
    tp[0] = (Pair){str_of("name"), val_str(t->name)};
    tp[1] = (Pair){str_of("slug"), val_str(t->slug)};
    tp[2] = (Pair){str_of("url"), val_str(t->url)};
    tp[3] = (Pair){str_of("count"), val_int((i32)t->nposts)};
    all_tags[i] = val_obj(tp, 4);
  }
  Value *all_tags_val = val_arr(all_tags, n_real_tags);
  u32 n_recent = recent_n < n_posts ? recent_n : n_posts;
  Value **recent_items = (Value **)arena_alloc(sizeof(Value *) * (n_recent ? n_recent : 1), 8);
  for (u32 i = 0; i < n_recent; i++) recent_items[i] = card_value(post_idx[i]);
  Value *recent_val = val_arr(recent_items, n_recent);

  Value *scope_buf[512];

  // Render helper state (file-scope via globals for want of closures):
  // renders template `name` with extra pairs; returns 1 + view, or 0.
  Outputs outputs;
  outputs_init(&outputs);

  // Source pages.
  for (u32 i = 0; i < n_sources; i++) {
    Source *s = &sources[i];
    i32 ti = map_get(&tpl_by_name, s->layout);
    if (ti < 0) continue;
    Pair *rp = (Pair *)arena_alloc(sizeof(Pair) * 4, 8);
    rp[0] = (Pair){str_of("site"), site_val};
    rp[1] = (Pair){str_of("tags"), all_tags_val};
    rp[2] = (Pair){str_of("recent"), recent_val};
    rp[3] = (Pair){str_of("page"), page_value((i32)i)};
    scope_buf[0] = val_obj(rp, 4);
    Buf out;
    buf_init(&out);
    render_nodes(tpl_lists[ti], tpl_counts[ti], scope_buf, 1, &tpls,
                 0, &out);
    Buf path;
    buf_init(&path);
    buf_str(&path, s->out_dir);
    buf_c(&path, "/index.html");
    outputs_add(&outputs, buf_str_view(&path), buf_str_view(&out));
  }

  // Index pages.
  u32 per = per_page;
  u32 page_count = per > 0 ? (n_posts + per - 1) / per : 1;
  if (page_count < 1) page_count = 1;
  {
    i32 ti = map_get(&tpl_by_name, str_of("index"));
    if (ti >= 0) {
      for (u32 k = 1; k <= page_count; k++) {
        u32 from = (k - 1) * per;
        u32 to = from + per < n_posts ? from + per : n_posts;
        Value **slice = (Value **)arena_alloc(sizeof(Value *) * (to - from ? to - from : 1), 8);
        for (u32 j = from; j < to; j++) slice[j - from] = card_value(post_idx[j]);
        Buf url_k, url_newer, url_older;
        buf_init(&url_k);
        buf_init(&url_newer);
        buf_init(&url_older);
        if (k == 1) {
          buf_str(&url_k, base);
          buf_ch(&url_k, '/');
        } else {
          buf_str(&url_k, base);
          buf_c(&url_k, "/page/");
          buf_u32dec(&url_k, k);
          buf_ch(&url_k, '/');
        }
        if (k > 1) {
          if (k - 1 == 1) {
            buf_str(&url_newer, base);
            buf_ch(&url_newer, '/');
          } else {
            buf_str(&url_newer, base);
            buf_c(&url_newer, "/page/");
            buf_u32dec(&url_newer, k - 1);
            buf_ch(&url_newer, '/');
          }
        }
        if (k < page_count) {
          buf_str(&url_older, base);
          buf_c(&url_older, "/page/");
          buf_u32dec(&url_older, k + 1);
          buf_ch(&url_older, '/');
        }
        Pair *pager_pairs = (Pair *)arena_alloc(sizeof(Pair) * 5, 8);
        pager_pairs[0] = (Pair){str_of("number"), val_int((i32)k)};
        pager_pairs[1] = (Pair){str_of("total"), val_int((i32)page_count)};
        pager_pairs[2] = (Pair){str_of("url"), val_str(buf_str_view(&url_k))};
        pager_pairs[3] = (Pair){str_of("newer_url"), val_str(buf_str_view(&url_newer))};
        pager_pairs[4] = (Pair){str_of("older_url"), val_str(buf_str_view(&url_older))};
        Pair *rp = (Pair *)arena_alloc(sizeof(Pair) * 5, 8);
        rp[0] = (Pair){str_of("site"), site_val};
        rp[1] = (Pair){str_of("tags"), all_tags_val};
        rp[2] = (Pair){str_of("recent"), recent_val};
        rp[3] = (Pair){str_of("posts"), val_arr(slice, to - from)};
        rp[4] = (Pair){str_of("pager"), val_obj(pager_pairs, 5)};
        scope_buf[0] = val_obj(rp, 5);
        Buf out;
        buf_init(&out);
        render_nodes(tpl_lists[ti], tpl_counts[ti], scope_buf, 1,
                     &tpls, 0, &out);
        Buf path;
        buf_init(&path);
        if (k == 1) {
          buf_c(&path, "index.html");
        } else {
          buf_c(&path, "page/");
          buf_u32dec(&path, k);
          buf_c(&path, "/index.html");
        }
        outputs_add(&outputs, buf_str_view(&path), buf_str_view(&out));
      }
    }
  }

  // Tag pages.
  {
    i32 ti = map_get(&tpl_by_name, str_of("tag"));
    if (ti >= 0) {
      for (u32 oi = 0; oi < n_real_tags; oi++) {
        Tag *t = &tags[tag_order[oi]];
        Value **slice = (Value **)arena_alloc(sizeof(Value *) * (t->nposts ? t->nposts : 1), 8);
        for (u32 j = 0; j < t->nposts; j++) slice[j] = card_value(t->posts[j]);
        Pair *tp = (Pair *)arena_alloc(sizeof(Pair) * 5, 8);
        tp[0] = (Pair){str_of("name"), val_str(t->name)};
        tp[1] = (Pair){str_of("slug"), val_str(t->slug)};
        tp[2] = (Pair){str_of("url"), val_str(t->url)};
        tp[3] = (Pair){str_of("count"), val_int((i32)t->nposts)};
        tp[4] = (Pair){str_of("posts"), val_arr(slice, t->nposts)};
        Pair *rp = (Pair *)arena_alloc(sizeof(Pair) * 4, 8);
        rp[0] = (Pair){str_of("site"), site_val};
        rp[1] = (Pair){str_of("tags"), all_tags_val};
        rp[2] = (Pair){str_of("recent"), recent_val};
        rp[3] = (Pair){str_of("tag"), val_obj(tp, 5)};
        scope_buf[0] = val_obj(rp, 4);
        Buf out;
        buf_init(&out);
        render_nodes(tpl_lists[ti], tpl_counts[ti], scope_buf, 1,
                     &tpls, 0, &out);
        Buf path;
        buf_init(&path);
        buf_c(&path, "tags/");
        buf_str(&path, t->slug);
        buf_c(&path, "/index.html");
        outputs_add(&outputs, buf_str_view(&path), buf_str_view(&out));
      }
    }
  }

  // Tags index.
  {
    i32 ti = map_get(&tpl_by_name, str_of("tags"));
    if (ti >= 0) {
      Pair *rp = (Pair *)arena_alloc(sizeof(Pair) * 3, 8);
      rp[0] = (Pair){str_of("site"), site_val};
      rp[1] = (Pair){str_of("tags"), all_tags_val};
      rp[2] = (Pair){str_of("recent"), recent_val};
      scope_buf[0] = val_obj(rp, 3);
      Buf out;
      buf_init(&out);
      render_nodes(tpl_lists[ti], tpl_counts[ti], scope_buf, 1,
                   &tpls, 0, &out);
      outputs_add(&outputs, str_of("tags/index.html"), buf_str_view(&out));
    }
  }

  // Statics.
  for (u32 i = 0; i < n_statics; i++) {
    if (outputs_has(&outputs, statics[i].path)) {
      Buf line;
      buf_init(&line);
      buf_c(&line, "shadowed ");
      buf_str(&line, statics[i].path);
      buf_c(&line, ": a built page has its path");
      strvec_push(&report, buf_str_view(&line));
    } else {
      outputs_add(&outputs, statics[i].path, statics[i].content);
    }
  }

  // Report. (Broken-link lines are a set in the reference: dedupe.)
  {
    Map seen_broken;
    map_init(&seen_broken, 11);
    for (u32 i = 0; i < broken_lines.n; i++) {
      if (map_get(&seen_broken, broken_lines.items[i]) >= 0) continue;
      map_set(&seen_broken, broken_lines.items[i], 0);
      strvec_push(&report, broken_lines.items[i]);
    }
  }
  g_lines = &report;
  i32 *line_order = (i32 *)arena_alloc(sizeof(i32) * (report.n ? report.n : 1), 4);
  for (u32 i = 0; i < report.n; i++) line_order[i] = (i32)i;
  sort_indices(line_order, report.n, cmp_lines);
  Buf rep;
  buf_init(&rep);
  for (u32 i = 0; i < report.n; i++) {
    buf_str(&rep, report.items[line_order[i]]);
    buf_ch(&rep, '\n');
  }
  u32 page_total = 0;
  for (u32 i = 0; i < n_sources; i++)
    if (!sources[i].is_post) page_total++;
  buf_c(&rep, "built ");
  buf_u32dec(&rep, page_total);
  buf_c(&rep, " pages, ");
  buf_u32dec(&rep, n_posts);
  buf_c(&rep, " posts, ");
  buf_u32dec(&rep, n_real_tags);
  buf_c(&rep, " tags and ");
  buf_u32dec(&rep, n_statics);
  buf_c(&rep, " static files\n");
  outputs_add(&outputs, str_of("build-report.txt"), buf_str_view(&rep));

  // Serialise into the caller's buffer.
  if (mode == 0) {
    // table: count, then per pair (path len, path, text len, text)
    u8 hdr[4];
    hdr[0] = (u8)(outputs.n & 0xFF);
    hdr[1] = (u8)((outputs.n >> 8) & 0xFF);
    hdr[2] = (u8)((outputs.n >> 16) & 0xFF);
    hdr[3] = (u8)((outputs.n >> 24) & 0xFF);
    buf_put(result, hdr, 4);
    for (u32 i = 0; i < outputs.n; i++) {
      u32 pl = outputs.items[i].path.n, tl = outputs.items[i].text.n;
      u8 h[4];
      h[0] = (u8)pl; h[1] = (u8)(pl >> 8); h[2] = (u8)(pl >> 16); h[3] = (u8)(pl >> 24);
      buf_put(result, h, 4);
      buf_str(result, outputs.items[i].path);
      h[0] = (u8)tl; h[1] = (u8)(tl >> 8); h[2] = (u8)(tl >> 16); h[3] = (u8)(tl >> 24);
      buf_put(result, h, 4);
      buf_str(result, outputs.items[i].text);
    }
  } else {
    buf_ch(result, '{');
    for (u32 i = 0; i < outputs.n; i++) {
      if (i) buf_ch(result, ',');
      buf_ch(result, '"');
      buf_escape_json(result, outputs.items[i].path);
      buf_c(result, "\":\"");
      buf_escape_json(result, outputs.items[i].text);
      buf_ch(result, '"');
    }
    buf_ch(result, '}');
  }
}

// ------------------------------------------------------------ entry points

// Pack path (solve.js run/runCaseJson): parse the pack, build, and
// leave the result pointer/length in the header at memory[0..8).
__attribute__((export_name("build_entry"))) void build_entry(u32 pack_ptr,
                                                             u32 pack_len,
                                                             u32 mode) {
  arena_init((u8 *)((pack_ptr + pack_len + 15) & ~15u));
  Pack pk = {(const u8 *)pack_ptr, pack_len, 0};
  CaseIn in;
  in.n_templates = pack_u32(&pk);
  in.tpl_names = (Str *)arena_alloc(sizeof(Str) * (in.n_templates ? in.n_templates : 1), 8);
  in.tpl_srcs = (Str *)arena_alloc(sizeof(Str) * (in.n_templates ? in.n_templates : 1), 8);
  for (u32 i = 0; i < in.n_templates; i++) {
    in.tpl_names[i] = pack_str(&pk);
    in.tpl_srcs[i] = pack_str(&pk);
  }
  in.site_title = pack_str(&pk);
  in.site_base_raw = pack_str(&pk);
  in.per_page = pack_u32(&pk);
  in.recent_n = pack_u32(&pk);
  in.n_files = pack_u32(&pk);
  in.file_paths = (Str *)arena_alloc(sizeof(Str) * (in.n_files ? in.n_files : 1), 8);
  in.file_contents = (Str *)arena_alloc(sizeof(Str) * (in.n_files ? in.n_files : 1), 8);
  for (u32 i = 0; i < in.n_files; i++) {
    in.file_paths[i] = pack_str(&pk);
    in.file_contents[i] = pack_str(&pk);
  }
  Buf result;
  buf_init(&result);
  build_core(&in, mode, &result);
  volatile u32 *hdr = (volatile u32 *)0;
  hdr[0] = (u32)result.p;
  hdr[1] = result.n;
}

// ------------------------------------------------------------ JSON input
// A small JSON reader for cases.json: it extracts the case inputs
// (site, templates, files) and skips everything else. Strings are
// unescaped into the arena (with \uXXXX and surrogate pairs decoded to
// UTF-8); numbers needed are non-negative integers.

typedef struct {
  const u8 *p;
  u32 n, pos;
} Json;

static void json_ws(Json *j) {
  while (j->pos < j->n) {
    u8 c = j->p[j->pos];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
      j->pos++;
    else
      break;
  }
}

static u32 json_hex4(Json *j) {
  u32 v = 0;
  for (int i = 0; i < 4 && j->pos < j->n; i++) {
    u8 c = j->p[j->pos++];
    v <<= 4;
    if (c >= '0' && c <= '9') v |= (u32)(c - '0');
    else if (c >= 'a' && c <= 'f') v |= (u32)(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') v |= (u32)(c - 'A' + 10);
  }
  return v;
}

// Parse a JSON string at the cursor into out (unescaped). Assumes the
// cursor is at the opening quote.
// The offset of the first '"' or '\\' in p[from..n), or n.
static u32 json_special_at(const u8 *p, u32 n, u32 from) {
  u32 i = from;
  while (i + 16 <= n) {
    v128_t v = wasm_v128_load(p + i);
    v128_t hit = wasm_i8x16_eq(v, wasm_i8x16_splat('"'));
    hit = wasm_v128_or(hit, wasm_i8x16_eq(v, wasm_i8x16_splat('\\')));
    u32 mask = wasm_i8x16_bitmask(hit);
    if (mask) return i + (u32)__builtin_ctz(mask);
    i += 16;
  }
  while (i < n) {
    if (p[i] == '"' || p[i] == '\\') return i;
    i++;
  }
  return n;
}

static void json_string(Json *j, Buf *out) {
  j->pos++;  // opening quote
  u32 run = j->pos;
  while (j->pos < j->n) {
    u8 c = j->p[j->pos];
    if (c != '"' && c != '\\') {
      j->pos = json_special_at(j->p, j->n, j->pos);
      continue;
    }
    if (c == '"') {
      if (j->pos > run) buf_put(out, j->p + run, j->pos - run);
      j->pos++;
      return;
    }
    if (c == '\\' && j->pos + 1 < j->n) {
      if (j->pos > run) buf_put(out, j->p + run, j->pos - run);
      u8 e = j->p[j->pos + 1];
      j->pos += 2;
      switch (e) {
        case '"': buf_ch(out, '"'); break;
        case '\\': buf_ch(out, '\\'); break;
        case '/': buf_ch(out, '/'); break;
        case 'b': buf_ch(out, '\b'); break;
        case 'f': buf_ch(out, '\f'); break;
        case 'n': buf_ch(out, '\n'); break;
        case 'r': buf_ch(out, '\r'); break;
        case 't': buf_ch(out, '\t'); break;
        case 'u': {
          u32 cp = json_hex4(j);
          if (cp >= 0xD800 && cp <= 0xDBFF && j->pos + 1 < j->n &&
              j->p[j->pos] == '\\' && j->p[j->pos + 1] == 'u') {
            j->pos += 2;
            u32 lo = json_hex4(j);
            if (lo >= 0xDC00 && lo <= 0xDFFF)
              cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          }
          u8 enc[4];
          u32 el = utf8_encode(enc, cp);
          buf_put(out, enc, el);
          break;
        }
        default: buf_ch(out, e); break;
      }
      run = j->pos;
      continue;
    }
    j->pos++;
  }
}

// Skip any JSON value.
static void json_skip(Json *j) {
  json_ws(j);
  if (j->pos >= j->n) return;
  u8 c = j->p[j->pos];
  if (c == '"') {
    Buf sink;
    sink.p = 0;
    sink.n = 0;
    sink.cap = 0;
    // skip without materialising: scan for the closing quote
    j->pos++;
    while (j->pos < j->n) {
      u8 ch = j->p[j->pos];
      if (ch == '\\') {
        j->pos += 2;
        continue;
      }
      j->pos++;
      if (ch == '"') break;
    }
    return;
  }
  if (c == '{' || c == '[') {
    u8 open = c, close = c == '{' ? '}' : ']';
    u32 depth = 0;
    while (j->pos < j->n) {
      u8 ch = j->p[j->pos];
      if (ch == '"') {
        // reuse the string skipper
        j->pos++;
        while (j->pos < j->n) {
          u8 s = j->p[j->pos];
          if (s == '\\') {
            j->pos += 2;
            continue;
          }
          j->pos++;
          if (s == '"') break;
        }
        continue;
      }
      if (ch == open) depth++;
      if (ch == close) {
        depth--;
        j->pos++;
        if (depth == 0) return;
        continue;
      }
      j->pos++;
    }
    return;
  }
  // literal: number, true, false, null
  while (j->pos < j->n) {
    u8 ch = j->p[j->pos];
    if (ch == ',' || ch == '}' || ch == ']' || ch == ' ' || ch == '\t' ||
        ch == '\n' || ch == '\r')
      break;
    j->pos++;
  }
}

static u32 json_uint(Json *j) {
  json_ws(j);
  u32 v = 0;
  while (j->pos < j->n && j->p[j->pos] >= '0' && j->p[j->pos] <= '9') {
    v = v * 10 + (u32)(j->p[j->pos] - '0');
    j->pos++;
  }
  // skip any fraction/exponent tail
  while (j->pos < j->n) {
    u8 c = j->p[j->pos];
    if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' ||
        c == '+' || c == '-')
      j->pos++;
    else
      break;
  }
  return v;
}

// Object member iteration helper: at the '{' of an object; calls the
// callback for each key with the value cursor positioned at the value.
// Returns 1 when the object was consumed.
typedef void (*JsonMemberFn)(void *ctx, Str key, Json *j);

static void json_object(Json *j, JsonMemberFn fn, void *ctx) {
  json_ws(j);
  if (j->pos >= j->n || j->p[j->pos] != '{') return;
  j->pos++;
  json_ws(j);
  if (j->pos < j->n && j->p[j->pos] == '}') {
    j->pos++;
    return;
  }
  while (j->pos < j->n) {
    json_ws(j);
    Buf key;
    buf_init(&key);
    json_string(j, &key);
    json_ws(j);
    if (j->pos < j->n && j->p[j->pos] == ':') j->pos++;
    fn(ctx, buf_str_view(&key), j);
    json_ws(j);
    if (j->pos < j->n && j->p[j->pos] == ',') {
      j->pos++;
      continue;
    }
    if (j->pos < j->n && j->p[j->pos] == '}') j->pos++;
    return;
  }
}

// A string value as a Str: a view into the raw input when it has no
// escapes (the common case: file contents and template sources are
// then copied nowhere), else decoded into a fresh buffer.
static Str json_string_view(Json *j) {
  u32 start = j->pos + 1;  // after the opening quote
  u32 hit = json_special_at(j->p, j->n, start);
  if (hit < j->n && j->p[hit] == '"') {
    j->pos = hit + 1;
    Str s = {j->p + start, hit - start};
    return s;
  }
  Buf b;
  buf_init(&b);
  j->pos = start - 1;
  json_string(j, &b);
  return buf_str_view(&b);
}

typedef struct {
  CaseIn in;
  Str *tpl_names, *tpl_srcs;
  u32 tpl_cap;
  Str *file_paths, *file_contents;
  u32 file_cap;
} CaseParse;

static void site_member(void *ctx, Str key, Json *j) {
  CaseIn *in = (CaseIn *)ctx;
  if (str_eq_c(key, "title")) {
    in->site_title = json_string_view(j);
  } else if (str_eq_c(key, "base_url")) {
    in->site_base_raw = json_string_view(j);
  } else if (str_eq_c(key, "per_page")) {
    in->per_page = json_uint(j);
  } else if (str_eq_c(key, "recent")) {
    in->recent_n = json_uint(j);
  } else {
    json_skip(j);
  }
}

static void template_member(void *ctx, Str key, Json *j) {
  CaseParse *cp = (CaseParse *)ctx;
  Str tv = json_string_view(j);
  if (cp->in.n_templates == cp->tpl_cap) {
    u32 ncap = cp->tpl_cap ? cp->tpl_cap * 2 : 32;
    Str *nn = (Str *)arena_alloc(sizeof(Str) * ncap, 8);
    Str *ns = (Str *)arena_alloc(sizeof(Str) * ncap, 8);
    if (cp->tpl_names) {
      memcpy(nn, cp->tpl_names, sizeof(Str) * cp->in.n_templates);
      memcpy(ns, cp->tpl_srcs, sizeof(Str) * cp->in.n_templates);
    }
    cp->tpl_names = nn;
    cp->tpl_srcs = ns;
    cp->tpl_cap = ncap;
  }
  cp->tpl_names[cp->in.n_templates] = key;
  cp->tpl_srcs[cp->in.n_templates] = tv;
  cp->in.n_templates++;
}

static void file_member(void *ctx, Str key, Json *j) {
  CaseParse *cp = (CaseParse *)ctx;
  // ctx points at the CaseParse; the current file index is n_files
  if (str_eq_c(key, "path")) {
    cp->file_paths[cp->in.n_files] = json_string_view(j);
  } else if (str_eq_c(key, "content")) {
    cp->file_contents[cp->in.n_files] = json_string_view(j);
  } else {
    json_skip(j);
  }
}

static void input_member(void *ctx, Str key, Json *j) {
  CaseParse *cp = (CaseParse *)ctx;
  if (str_eq_c(key, "site")) {
    json_object(j, site_member, &cp->in);
  } else if (str_eq_c(key, "templates")) {
    json_object(j, template_member, cp);
  } else if (str_eq_c(key, "files")) {
    // array of {path, content} objects
    json_ws(j);
    if (j->pos < j->n && j->p[j->pos] == '[') {
      j->pos++;
      json_ws(j);
      if (j->pos < j->n && j->p[j->pos] == ']') {
        j->pos++;
        return;
      }
      while (j->pos < j->n) {
        if (cp->in.n_files == cp->file_cap) {
          u32 ncap = cp->file_cap ? cp->file_cap * 2 : 256;
          Str *np = (Str *)arena_alloc(sizeof(Str) * ncap, 8);
          Str *nc = (Str *)arena_alloc(sizeof(Str) * ncap, 8);
          if (cp->file_paths) {
            memcpy(np, cp->file_paths, sizeof(Str) * cp->in.n_files);
            memcpy(nc, cp->file_contents, sizeof(Str) * cp->in.n_files);
          }
          cp->file_paths = np;
          cp->file_contents = nc;
          cp->file_cap = ncap;
        }
        json_object(j, file_member, cp);
        cp->in.n_files++;
        json_ws(j);
        if (j->pos < j->n && j->p[j->pos] == ',') {
          j->pos++;
          continue;
        }
        if (j->pos < j->n && j->p[j->pos] == ']') j->pos++;
        break;
      }
    }
  } else {
    json_skip(j);
  }
}

static void case_member(void *ctx, Str key, Json *j) {
  CaseParse *cp = (CaseParse *)ctx;
  if (str_eq_c(key, "input")) {
    json_object(j, input_member, cp);
  } else {
    json_skip(j);
  }
}

// Sorting for the raw path: templates by name, files by path.
static int cmp_str_adapter(i32 a, i32 b);
static Str *g_sort_keys = 0;
static int cmp_by_key(i32 a, i32 b) { return str_cmp(g_sort_keys[a], g_sort_keys[b]); }

static void sort_pairs(Str *keys, Str *vals, u32 n) {
  if (n < 2) return;
  i32 *idx = (i32 *)arena_alloc(sizeof(i32) * n, 4);
  for (u32 i = 0; i < n; i++) idx[i] = (i32)i;
  g_sort_keys = keys;
  sort_indices(idx, n, cmp_by_key);
  Str *nk = (Str *)arena_alloc(sizeof(Str) * n, 8);
  Str *nv = (Str *)arena_alloc(sizeof(Str) * n, 8);
  for (u32 i = 0; i < n; i++) {
    nk[i] = keys[idx[i]];
    nv[i] = vals[idx[i]];
  }
  memcpy(keys, nk, sizeof(Str) * n);
  memcpy(vals, nv, sizeof(Str) * n);
}

// Raw path (the driver): cases.json bytes in, answers.json bytes out.
// The arena for a case starts after the answers assembled so far, and
// is reset for every case; each case's fragment is copied out before
// the next case starts.
__attribute__((export_name("build_all_entry"))) void build_all_entry(
    u32 raw_ptr, u32 raw_len) {
  u8 *answers_base = (u8 *)(((u32)raw_ptr + raw_len + 15) & ~15u);
  arena_init(answers_base);
  Buf answers;
  buf_init(&answers);
  buf_ch(&answers, '[');
  // Save the answers buffer's arena state: it lives in its own bump
  // region; case work uses a second region above it. We implement the
  // two regions by saving/restoring the bump cursor around each case.
  u8 *answers_region_end = arena_ptr;
  Json j = {(const u8 *)raw_ptr, raw_len, 0};
  json_ws(&j);
  int first = 1;
  if (j.pos < j.n && j.p[j.pos] == '[') {
    j.pos++;
    json_ws(&j);
    if (j.pos < j.n && j.p[j.pos] == ']') {
      j.pos++;
    } else {
      while (j.pos < j.n) {
        // case arena starts above the answers region
        arena_ptr = answers_region_end;
        arena_base = answers_region_end;
        CaseParse cp;
        memset(&cp, 0, sizeof(cp));
        json_object(&j, case_member, &cp);
        cp.in.tpl_names = cp.tpl_names;
        cp.in.tpl_srcs = cp.tpl_srcs;
        cp.in.file_paths = cp.file_paths;
        cp.in.file_contents = cp.file_contents;
        sort_pairs(cp.in.tpl_names, cp.in.tpl_srcs, cp.in.n_templates);
        sort_pairs(cp.in.file_paths, cp.in.file_contents, cp.in.n_files);
        Buf frag;
        buf_init(&frag);
        build_core(&cp.in, 1, &frag);
        // copy the fragment into the answers region
        u8 *save_base = arena_base;
        u32 save_ptr = (u32)arena_ptr;
        arena_base = answers_base;
        arena_ptr = answers_region_end;
        // answers Buf header lives on the stack; its storage may have
        // been inside the case region if it grew — it cannot: it only
        // grows here, in the answers region.
        if (!first) buf_ch(&answers, ',');
        first = 0;
        buf_str(&answers, buf_str_view(&frag));
        answers_region_end = arena_ptr;
        arena_base = save_base;
        arena_ptr = (u8 *)save_ptr;
        json_ws(&j);
        if (j.pos < j.n && j.p[j.pos] == ',') {
          j.pos++;
          continue;
        }
        if (j.pos < j.n && j.p[j.pos] == ']') j.pos++;
        break;
      }
    }
  }
  // finish in the answers region
  arena_base = answers_base;
  arena_ptr = answers_region_end;
  buf_ch(&answers, ']');
  volatile u32 *hdr = (volatile u32 *)0;
  hdr[0] = (u32)answers.p;
  hdr[1] = answers.n;
}
