#pragma once
#include "assert.h"
#include "fcntl.h"
#include "stdint.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "sys/mman.h"
#include "sys/stat.h"
#include <sys/mman.h>

// non-null terminated string
typedef struct str {
  const uint8_t *s;
  size_t l;
} str;

#define LSTR(s) {(uint8_t *)s, sizeof(s) - 1} // jane signalis

#define mstr(x) #x
#define xstr(x) mstr(x)
#define xenumstr(x) xstr(x),

// allocator interface
typedef struct allocator_t {
  void *(*alloc)(void *allocator, size_t size_nbytes);
  void *(*realloc)(void *allocator, void *ptr, size_t new_size_nbytes);
  void (*free)(void *allocator, void *ptr);
  void *allocator;
} allocator_t;

#define ARENA_REGION_DEFAULT_CAPACITY ((size_t)0x1000)

typedef struct arenaRegion {
  size_t inuse;
  size_t len;
  size_t cap;
  struct arenaRegion *next;
} arenaRegion;

// dumb arena
typedef struct arena {
  arenaRegion *head;
} arena;

arenaRegion *makeRegion(size_t size) {
  arenaRegion *r =
      (arenaRegion *)mmap(0, size + sizeof(arenaRegion), PROT_READ | PROT_WRITE,
                          MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  r->inuse = 0;
  r->len = 0;
  r->cap = size;
  r->next = 0;
  return r;
}

int freeRegion(arenaRegion *r) {
  return munmap(r, r->cap + sizeof(arenaRegion));
}

void arenaMarkNotInUse(void *v) { (((arenaRegion *)v) - 1)->inuse = 0; }

arenaRegion *_arenaAlloc(arena *a, size_t nbytes) {
  arenaRegion *r = a->head;
  while (r->inuse || r->cap <= nbytes) {
    if (!r->next) {
      r->next = makeRegion(nbytes < ARENA_REGION_DEFAULT_CAPACITY
                               ? ARENA_REGION_DEFAULT_CAPACITY
                               : nbytes);
      r = r->next;
      goto ret;
    }
    r = r->next;
  }
ret:
  r->inuse = 1;
  r->len = nbytes;
  return r;
}

void *arenaAlloc(arena *a, size_t nbytes) { return _arenaAlloc(a, nbytes) + 1; }

void arenaFree(arena *a) {
  arenaRegion *r = a->head;
  while (r) {
    arenaRegion *nr = r->next;
    munmap(r, r->cap + sizeof(arenaRegion));
    r = nr;
  }
}

void *arenaReAlloc(arena *a, void *v, size_t newsize) {
  arenaRegion *r = (arenaRegion *)v - 1;
  if (r->cap >= newsize) {
    r->len = newsize;
    return v;
  }
  r->inuse = 0;
  arenaRegion *nr = _arenaAlloc(a, newsize) + 1;
  memcpy(nr, r + 1, r->len);
  return nr;
}

void *arenaAlloc_AllocatorInterface(void *a, size_t size) {
  return arenaAlloc((arena *)a, size);
}
void *arenaReAlloc_AllocatorInterface(void *a, void *v, size_t newsize) {
  return arenaReAlloc((arena *)a, v, newsize);
}
void arenaFree_AllocatorInterface(void *_, void *v) { arenaMarkNotInUse(v); }

typedef struct vector {
  void *v;
  size_t len;
  size_t cap;
  allocator_t *allocator;
} vector;

void *vectorAlloc(vector *v, size_t size) {
  if (v->len + size > v->cap) {
    v->v = v->allocator->realloc(v->allocator->allocator, v->v,
                                 (v->cap << 2) + size);
  }
  uint8_t *va = (uint8_t *)v->v + v->len;
  memset(va, 0, size);
  v->len += size;
  return va;
}

void *vectorPop(vector *v, size_t size) {
  v->len -= size;
  uint8_t *va = (uint8_t *)v->v + (v->len);
  return va;
}

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

#define HASHMAP_DEFAULT_CAPACITY                                               \
  ARENA_REGION_DEFAULT_CAPACITY // must be multiple of 2

struct hashMap {
  str *ks;
  void **v;
  size_t len;
  size_t cap;
  allocator_t *allocator;
};

typedef struct hashMap hashMap;

void *hashMapRetr(const hashMap *m, const uint8_t *k, size_t k_length) {
  uint64_t i = fnv_1a(k, k_length) & (m->cap - 1);
  while ((m->ks + i)->s) {
    if ((m->ks + i)->l == k_length && !memcmp(k, (m->ks + i)->s, k_length)) {
      return *(m->v + i);
    }
    i = (i + 1) & (m->cap - 1);
  }
  return 0;
}

void hashMapCopyInternal(str *sks, void **sv, size_t scap, str *dks, void **dv,
                         size_t dcap) {
  size_t i = 0;
  while (i < scap) {
    if ((sks + i)->s) {
      uint64_t di = fnv_1a((sks + i)->s, (sks + i)->l) & (dcap - 1);
      while ((dks + di)->s != 0) {
        di = (di + 1) & (dcap - 1);
      };
      *(dks + di) = *(sks + i);
      *(dv + di) = *(sv + i);
    }
    i++;
  }
}

void hashMapResize(hashMap *s, size_t newcap) {
  void **nv = (void **)s->allocator->alloc(s->allocator->allocator,
                                           newcap * sizeof(void *));
  str *nks =
      (str *)s->allocator->alloc(s->allocator->allocator, newcap * sizeof(str));
  hashMapCopyInternal(s->ks, s->v, s->cap, nks, nv, newcap);
  s->v = nv;
  s->ks = nks;
  s->cap = newcap;
}

void hashMapInsert(hashMap *m, const uint8_t *k, size_t k_length, void *v) {
  if (m->len == m->cap) {
    void *oks = m->ks;
    void *ov = m->v;
    hashMapResize(m, m->cap << 1);
    m->allocator->free(m->allocator->allocator, oks);
    m->allocator->free(m->allocator->allocator, ov);
  }
  uint64_t i = fnv_1a(k, k_length) & (m->cap - 1);
  while ((m->ks + i)->s != 0) {
    i = (i + 1) & (m->cap - 1);
  };
  (m->ks + i)->s = k;
  (m->ks + i)->l = k_length;
  *(m->v + i) = v;
  m->len = m->len + 1;
  return;
}

void hashMapPullOut(hashMap *m, const uint8_t *k, size_t k_length) {
  uint64_t i = fnv_1a(k, k_length) & (m->cap - 1);
  while ((m->ks + i)->s) {
    if ((m->ks + i)->l == k_length && !memcmp(k, (m->ks + i)->s, k_length)) {
      (m->ks + i)->s = 0;
    }
    i = (i + 1) & (m->cap - 1);
    m->len = m->len - 1;
  }
  return;
}

int hashMapContains(hashMap *m, const uint8_t *k, size_t k_length) {
  uint64_t i = fnv_1a(k, k_length) & (m->cap - 1);
  while ((m->ks + i)->s) {
    if ((m->ks + i)->l == k_length && !memcmp(k, (m->ks + i)->s, k_length)) {
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
  X(TokenNegativeInteger, "")                                                  \
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
  X(TokenFatL2RArrow, "=>")                                                    \
  X(TokenAdd, "+")                                                             \
  X(TokenSub, "-")                                                             \
  X(TokenAsterisk, "*")                                                        \
  X(TokenForwardSlash, "/")

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

#define TYPECLASS_UNIT 1
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
  struct Field {
    hvllClass *t;
    str name;
  } fields[];
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
const hvllClass UnitType = {.type_mod = 0,
                            .type_class = TYPECLASS_UNIT,
                            .size = 0,
                            .name = LSTR("UNIT")};

enum LeafType {
  LeafRuntimeReserve,
  LeafDecl,
  LeafAssign,
  LeafFnCall,
  LeafType,
  LeafUnit,
  LeafScalar,
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
        } table_array_syle_inst;
        struct {
          const void *v;
          size_t len;
        } array;
      };
    };
  };
};

const abstractSyntaxTree UnitAst = {.leaf_type = LeafUnit};

typedef struct Symbol {
  str sym_name;
  const abstractSyntaxTree ast;
  size_t depth;
  size_t env_id;
} Symbol;

#define DEFTYPESYMBOL(nm, t)                                                   \
  {.sym_name = LSTR(nm),                                                       \
   .ast = {.leaf_type = LeafType, .type_class = t},                            \
   .depth = 0,                                                                 \
   .env_id = 0}

const Symbol builtin_symbols[] =
    /* !!DO NOT CHANGE THIS COMMENT!! ~!*/ {DEFTYPESYMBOL("usize", &usize),
                                            DEFTYPESYMBOL("isize", &isize)};