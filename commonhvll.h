#pragma once
#include "assert.h"
#include "fcntl.h"
#include "stdint.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "sys/mman.h"
#include "sys/stat.h"

typedef struct str {
  uint8_t *s;
  size_t l;
} str;

#define LSTR(s) {(uint8_t *)s, sizeof(s) - 1} // jane signalis

#define mstr(x) #x
#define xstr(x) mstr(x)
#define xenumstr(x) xstr(x),

// dumb flat arena

typedef struct arena arena;

struct arena {
  void *v;
  size_t len;   // total bytes used
  size_t count; // number of allocations
  size_t cap;
};

void *arenaAlloc(arena *a, size_t size) {
  if (a->len + size >= a->cap) {
    size_t newcap =
        (a->cap << 1) + (size + sizeof(size_t) - 1) & ~(sizeof(size_t) - 1);
    a->v = realloc(a->v, newcap); // null? scape
    a->cap = newcap;
  }
  void *r = ((uint8_t *)a->v + a->len);
  a->count++;
  a->len = (a->len + size);
  return r;
}

void *arenaRealloc(arena *a, void *src, size_t src_size, size_t new_size) {
  if (a->len + new_size >= a->cap) {
    size_t newcap = (a->cap << 1) + new_size;
    a->v = realloc(a->v, newcap); // null? let it happen
    a->cap = newcap;
  }
  void *dest = ((uint8_t *)a->v + a->len);
  memcpy(dest, src, src_size);
  a->len = (a->len + new_size + sizeof(size_t) - 1) & ~(sizeof(size_t) - 1);
  return dest;
}

void arenaDeallocLast(arena *a, size_t size) { a->len = a->len - size; }

#define FNV_OFFSET 0xcbf29ce484222325
#define FNV_PRIME 0x100000001b3

uint64_t fnv_1a(const uint8_t *v, size_t len) {
  uint64_t h = FNV_OFFSET;
  for (size_t i = 0; i < len; i++) {
    h = h ^ *(v + i);
    h = h * FNV_PRIME;
  }
  return h;
}

#define HASHMAP_DEFAULT_CAPACITY (size_t)2048 // must be multiple of 2

struct hashMapKeyEntry {
  const uint8_t *k;
  size_t k_length;
};

typedef struct hashMapKeyEntry hashMapKeyEntry;

struct hashMap {
  hashMapKeyEntry *ks;
  void **v;
  size_t len;
  size_t cap;
};

typedef struct hashMap hashMap;

void *hashMapRetr(const hashMap *m, const uint8_t *k, size_t k_length) {
  uint64_t i = fnv_1a(k, k_length) & (m->cap - 1);
  while ((m->ks + i)->k) {
    if ((m->ks + i)->k_length == k_length &&
        !memcmp(k, (m->ks + i)->k, k_length)) {
      return *(m->v + i);
    }
    i = (i + 1) & (m->cap - 1);
  }
  return 0;
}

// dest hashmap must be eq or larger than src
void hashMapCopyInternal(hashMapKeyEntry *sks, void **sv, size_t scap,
                         hashMapKeyEntry *dks, void **dv, size_t dcap) {
  size_t i = 0;
  while (i < scap) {
    if ((sks + i)->k) {
      uint64_t di = fnv_1a((sks + i)->k, (sks + i)->k_length) & (dcap - 1);
      while ((dks + di)->k != 0) {
        di = (di + 1) & (dcap - 1);
      };
      *(dks + di) = *(sks + i);
      *(dv + di) = *(sv + i);
    }
    i++;
  }
}

void hashMapResizeRelocate(hashMap *s, size_t newcap) {
  void **nv = (void **)calloc(newcap, sizeof(void *));
  hashMapKeyEntry *nks =
      (hashMapKeyEntry *)calloc(newcap, sizeof(hashMapKeyEntry));
  hashMapCopyInternal(s->ks, s->v, s->cap, nks, nv, newcap);
  s->v = nv;
  s->ks = nks;
  s->cap = newcap;
}

void hashMapInsert(hashMap *m, const uint8_t *k, size_t k_length, void *v) {
  if (m->len == m->cap) {
    void *oks = m->ks;
    void *ov = m->v;
    hashMapResizeRelocate(m, m->cap << 1);
    free(oks);
    free(ov);
  }
  uint64_t i = fnv_1a(k, k_length) & (m->cap - 1);
  while ((m->ks + i)->k != 0) {
    i = (i + 1) & (m->cap - 1);
  };
  (m->ks + i)->k = k;
  (m->ks + i)->k_length = k_length;
  *(m->v + i) = v;
  m->len = m->len + 1;
  return;
}

void hashMapPullOut(hashMap *m, const uint8_t *k, size_t k_length) {
  uint64_t i = fnv_1a(k, k_length) & (m->cap - 1);
  while ((m->ks + i)->k) {
    if ((m->ks + i)->k_length == k_length &&
        !memcmp(k, (m->ks + i)->k, k_length)) {
      (m->ks + i)->k = 0;
    }
    i = (i + 1) & (m->cap - 1);
    m->len = m->len - 1;
  }
  return;
}

int hashMapContains(hashMap *m, const uint8_t *k, size_t k_length) {
  uint64_t i = fnv_1a(k, k_length) & (m->cap - 1);
  while ((m->ks + i)->k) {
    if ((m->ks + i)->k_length == k_length &&
        !memcmp(k, (m->ks + i)->k, k_length)) {
      return 1;
    }
    i = (i + 1) & (m->cap - 1);
  }
  return 0;
}
// do linear search for now. too lazy to sort

#define DEFTOKENS                                                              \
  X(TokenAiden, "")                                                            \
  X(TokenString, "")                                                           \
  X(TokenInteger, "")                                                          \
  X(Token2Colon, "::")                                                         \
  X(TokenL2RArrow, "->")                                                       \
  X(TokenLayout, ";")                                                          \
  X(TokenQuestion, "?")                                                        \
  X(TokenColon, ":")                                                           \
  X(TokenAssign, "=")                                                          \
  X(TokenParenOpen, "(")                                                       \
  X(TokenParenClose, ")")                                                      \
  X(TokenBracketOpen, "{")                                                     \
  X(TokenBracketClose, "}")                                                    \
  X(TokenDol, "$")                                                             \
  X(TokenComma, ",")                                                           \
  X(TokenPoint, ".")                                                           \
  X(TokenMacro, "!")                                                           \
  X(TokenSquareBracketOpen, "[")                                               \
  X(TokenSquareBracketClose, "]")                                              \
  X(TokenMut, "mut")                                                           \
  X(TokenTable, "table")                                                       \
  X(TokenTrue, "true")                                                         \
  X(TokenFalse, "false")                                                       \
  X(TokenUnit, "()")                                                           \
  X(TokenSentinel, "sentinel")                                                 \
  X(TokenMatch, "match")                                                       \
  X(TokenFatL2RArrow, "=>")

#define X(a, b) a,
enum TokenType { DEFTOKENS };
#undef X
#define X(A, B) xstr(A),
const char *TokenTypeString[] = {DEFTOKENS};
#undef X
#define X(a, b) {LSTR(b), a},
#define TKNFLOOR (TokenInteger + 1)
struct tkent {
  str s;
  size_t t;
} tkns[] = {DEFTOKENS};
#undef X
typedef struct hvllClass hvllClass;

#define TYPEMODIFIER_COMPILETIME 1 << 0
#define TYPEMODIFIER_VOLATILE 1 << 1
#define TYPEMODIFIER_MUTABLE 1 << 2
#define TYPEMODIFIER_BORROW 1 << 3

#define TYPECLASS_SENTINEL 1
#define TYPECLASS_INTEGER 2
#define TYPECLASS_BOOLEAN 3
#define TYPECLASS_FUNCTION 4
#define TYPECLASS_FATPTR 5
#define TYPECLASS_PTR 6
#define TYPECLASS_TABLE 7

typedef struct hvllClass {
  int type_mod;
  int type_class;
  size_t size;
  str name;
  union {
    size_t is_integer_signed;
    struct {
      const hvllClass *arg;
      const hvllClass *ret;
    } fn;
    const hvllClass *pointee;
  };
  const hvllClass *fields[];
} hvllClass;

const hvllClass usize = {.type_mod = 0,
                         .type_class = TYPECLASS_INTEGER,
                         .size = sizeof(size_t),
                         .name = LSTR("usize"),
                         .is_integer_signed = 0};
const hvllClass isize = {.type_mod = 0,
                         .type_class = TYPECLASS_INTEGER,
                         .size = sizeof(size_t),
                         .name = LSTR("isize"),
                         .is_integer_signed = 1};

#define LEAF_EXPR 1 << ((sizeof(size_t) * 8) - 1)

enum LeafType {
  LeafNull,
  LeafDecl,
  LeafAssign,
  LeafFnCall,
  LeafTypeDef,
};

typedef struct abstractSyntaxTree abstractSyntaxTree;

struct abstractSyntaxTree {
  size_t leaf_type;
  union {
    const hvllClass *type_class;
    struct {
      const hvllClass *expr_class;
      union {
        size_t scalar_int;
        struct {
          const abstractSyntaxTree **v;
          size_t field_count;
        } table;
        struct {
          const void *v;
          size_t len;
        } array;
      };
    };
  };
};

typedef struct Symbol {
  str sym_name;
  const abstractSyntaxTree ast;
  size_t depth;
  size_t env_id;
} Symbol;

#define DEFTYPESYMBOL(nm, t)                                                   \
  {.sym_name = LSTR(nm),                                                       \
   .ast = {.leaf_type = LeafTypeDef, .type_class = t},                         \
   .depth = 0,                                                                 \
   .env_id = 0}

const Symbol builtin_symbols[] =
    /* !!DO NOT CHANGE THIS COMMENT!! ~!*/ {DEFTYPESYMBOL("usize", &usize),
                                            DEFTYPESYMBOL("isize", &isize)};