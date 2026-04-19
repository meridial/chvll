#include "assert.h"
#include "baked.h"
#include "commonhvll.h"
#include "fcntl.h"
#include "stdint.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "sys/mman.h"
#include "sys/stat.h"
#include <stddef.h>
#include <stdint.h>

struct token_t {
  size_t s;
  size_t len;
  size_t character_offset;
  size_t token_type;
};

typedef struct token_t token_t;

struct lineAndOffset {
  size_t line;
  size_t offset_from_line;
} find_char_line(const uint8_t *s, size_t ci) {
  size_t n = 1;
  size_t ln = 0;
  size_t i = 0;
  for (; i < ci; i++) {
    if (*(s + i) == '\n') {
      ln = i;
      n++;
    }
  }
  struct lineAndOffset l = {.line = n, .offset_from_line = i - ln};
  return l;
}

int special_whitespace[0xff] = {
    [' '] = 1,
    ['\t'] = 1,
    ['\n'] = 1,
};
int is_valid_decimal_char(uint8_t c) { return c >= '0' && c <= '9'; }

int is_valid_hexadecimal_char(uint8_t c) {
  return '0' <= c && c <= '9' || 'A' <= c && c <= 'F' || 'a' <= c && c <= 'f';
}

int is_valid_possible_number_char(uint8_t c) {
  return '0' <= c && c <= '9' || 'A' <= c && c <= 'F' || 'a' <= c && c <= 'f';
}

int (*valid_nondecmial_base_character_checker_function[0xff])(uint8_t c) = {
    ['h'] = is_valid_hexadecimal_char,
    ['H'] = is_valid_hexadecimal_char,
    ['x'] = is_valid_hexadecimal_char,
    ['X'] = is_valid_hexadecimal_char,
};

int is_string_valid_number(const uint8_t *c, size_t len) {
  if (*c == '-' && len >= 2) {
    return is_string_valid_number(c + 1, len - 1);
  }
  if (len == 1 && is_valid_decimal_char(*c)) {
    return 1;
  }
  if (len == 2 && is_valid_decimal_char(*c) &&
      is_valid_decimal_char(*(c + 1))) {
    return 1;
  }
  if (len >= 3) {
    size_t check_floor = 0;
    int (*ckfn)(uint8_t) =
        valid_nondecmial_base_character_checker_function[*(c + 1)];
    if (ckfn) {
      check_floor = 2;
    } else {
      ckfn = is_valid_decimal_char;
    }
    for (; check_floor < len; check_floor++) {
      if (!ckfn(*(c + check_floor))) {
        return 0;
      }
    }
    return 1;
  }
  return 0;
}

size_t IntegerDecimalOrSubDecimalParse(const uint8_t *c, size_t len,
                                       size_t base) {
  size_t a = 0;
  size_t t = 1;
  for (size_t i = 0; i < len; i++) {
    size_t x = *(c + (len - i - 1));
    a += (x - '0') * t;
    t = t * base;
  }
  return a;
}

size_t IntegerHexadecimalParse(const uint8_t *c, size_t len) {
  size_t a = 0;
  size_t t = 1;
  for (size_t i = 0; i < len; i++) {
    size_t x = *(c + (len - i - 1)) & 0xff;
    if (x <= '9') {
      a += (x - '0') * t;
    } else if (x >= 'a') {
      a += ((x - 'a') + 10) * t;
    } else {
      a += ((x - 'A') + 10) * t;
    }
    t = t * 0x10;
  }
  return a;
}

size_t IntegerParse(const uint8_t *s, size_t len) {
  switch (*(s + 1)) {
  case 'x':
  case 'X':
    return IntegerHexadecimalParse(s + 2, len - 2);
    break;
  default:
    return IntegerDecimalOrSubDecimalParse(s, len, 10);
  }
}

const uint8_t single_char_string_escapes[0xff] = {
    ['n'] = '\n', ['t'] = '\t', ['a'] = '\a',  ['b'] = '\b',  ['r'] = '\r',
    ['v'] = '\v', ['f'] = '\f', ['\\'] = '\\', ['\''] = '\'', ['"'] = '"'};

// 0 on invalid
size_t StringEscape(uint8_t *d, const uint8_t *s, size_t len) {
  size_t dst_i = 0;
  for (size_t src_i = 0; src_i < len; src_i++) {
    if (s[src_i] == '\\') {
      if (len - src_i <= 1) {
        return 0;
      }
      src_i++;
      if (single_char_string_escapes[s[src_i]]) {
        d[dst_i] = single_char_string_escapes[s[src_i]];
        dst_i++;
        continue;
      }
      switch (s[src_i]) {
      case 'x':
      case 'X':
        if (len - src_i <= 2) {
          return 0;
        }
        src_i++;
        if (!is_valid_hexadecimal_char(s[src_i]) ||
            !is_valid_hexadecimal_char(s[src_i + 1])) {
          return 0;
        }
        size_t n = IntegerHexadecimalParse(s + src_i, 2);
        d[dst_i] = (uint8_t)n;
        dst_i++;
        src_i++;
        continue;
      default:
        return 0;
      }
    }
    d[dst_i] = s[src_i];
    dst_i++;
  }
  return dst_i;
}

#define UNTERMSTRING_EXIT(srcmap, ci, len)                                     \
  struct lineAndOffset lo = find_char_line(srcmap, ci);                        \
  printf("\x1b[1;31mtokenize_normal()\x1b[m: %lu:%lu => "                      \
         "\x1b[1;31munterminated string\x1b[m: \"%.*s\"\n",                    \
         lo.line, lo.offset_from_line, len, srcmap + ci);                      \
  exit(1);
#define MALFORMSTRING_EXIT(srcmap, ci, len)                                    \
  struct lineAndOffset lo = find_char_line(srcmap, ci);                        \
  printf("\x1b[1;31mtokenize_normal()\x1b[m: %lu:%lu => "                      \
         "\x1b[1;31mmalformed string\x1b[m: \"%.*s\"\n",                       \
         lo.line, lo.offset_from_line, len, srcmap + ci);                      \
  exit(1);

#define INVALIDNUMBER_EXIT(srcmap, ci, len)                                    \
  struct lineAndOffset lo = find_char_line(srcmap, ci);                        \
  printf("\x1b[1;31mtokenize_normal()\x1b[m: %lu:%lu => "                      \
         "\x1b[1;31minvalid number\x1b[m: \"%.*s\"\n",                         \
         lo.line, lo.offset_from_line, len, srcmap + ci);                      \
  exit(1);

#define token_backwards_check()                                                \
  if (ci != current_token_start) {                                             \
    token_t *t = arenaAlloc(ta, sizeof(token_t));                              \
    t->token_type = (size_t)hashMapRetr(                               \
        &tkhm, srcmap + current_token_start, ci - current_token_start);        \
    t->len = ci - current_token_start;                                         \
    t->character_offset = current_token_start;                                 \
    t->s = (size_t)srcmap + current_token_start;                               \
    current_token_start = ci;                                                  \
  }

void tokenize_normal(const uint8_t *srcmap, size_t len, arena *ta,
                     arena *stra) {
  size_t current_token_start = 0;
  size_t ci = 0;
  while (ci < len) {
    const uint8_t cc = srcmap[ci];
    if (special_whitespace[cc]) {
      token_backwards_check();
      int contains_newline_layout = 0;
      while (ci < len && special_whitespace[*(srcmap + ci)]) {
        if (*(srcmap + ci) == '\n') {
          contains_newline_layout = 1;
        }
        ci++;
      }
      if (contains_newline_layout) {
        token_t *t = arenaAlloc(ta, sizeof(token_t));
        t->token_type = TokenLayout;
        t->character_offset = current_token_start;
      }
      current_token_start = ci;
      continue;
    }
    if (cc == '\"') {
      token_backwards_check();
      size_t cci = ci + 1;
      for (;; cci++) {
        if (cci >= len) {
          UNTERMSTRING_EXIT(srcmap, ci, cci - ci);
        }
        if (srcmap[cci] == '\\') {
          cci++;
          continue;
        }
        if (srcmap[cci] == '"') {
          ci++;
          break;
        }
      }
      uint8_t *sb = arenaAlloc(stra, cci - ci);
      size_t nsl = StringEscape(sb, srcmap + ci, cci - ci);
      if (!nsl) {
        MALFORMSTRING_EXIT(srcmap, ci, cci - ci)
      }
      token_t *t = arenaAlloc(ta, sizeof(token_t));
      t->token_type = TokenString;
      t->s = (size_t)sb;
      t->len = nsl;
      t->character_offset = ci;
      ci = cci + 1;
      current_token_start = ci;
      continue;
    }

    int is_negative = 0;
    if (ci == current_token_start && len - ci > 1 && cc == '-' &&
        is_valid_decimal_char(srcmap[ci + 1])) {
      is_negative = 1;
    }
    if (is_negative || ci == current_token_start && is_valid_decimal_char(cc)) {
      token_backwards_check();
      size_t cci = ci;
      for (; cci < len && !special_whitespace[*(srcmap + cci)]; cci++)
        ;
      enum TokenType tt = is_string_valid_number(srcmap + ci, cci - ci);
      if (!tt) {
        INVALIDNUMBER_EXIT(srcmap, ci, cci - ci)
      }
      token_t *t = arenaAlloc(ta, sizeof(token_t));
      t->token_type = tt;
      t->s = is_negative ? -IntegerParse(srcmap + ci + 1, cci - ci - 1)
                         : IntegerParse(srcmap + ci, cci - ci);
      t->character_offset = ci;
      t->token_type = TokenInteger;
      ci = cci;
      current_token_start = cci;
      continue;
    }
    ci++;
  }
  token_backwards_check();
  return;
}
struct envSymbol {
  size_t depth;
  // used when the depth is the same. index of environment (unique in whole src)
  uint64_t env_idx;
  hvllClass *type;
  abstractSyntaxTree *symbol_src_ast;
};

typedef struct envSymbol envSymbol;

struct parserEnv {
  const uint8_t *srcmap;
  arena *arena;     //  general purpose
  hashMap *symbols; // K = str (*envSymbol), envSymbol
};

#define SYMBOLREDECLARATION_EXIT(srcmap, ci, len)                              \
  struct lineAndOffset lo = find_char_line(srcmap, ci);                        \
  printf("\x1b[1;31mParser()\x1b[m: %lu:%lu => \x1b[1;31msymbol "              \
         "redeclaration within "                                               \
         "current env "                                                        \
         "depth or above\x1b[m \"%.*s\"\n",                                    \
         lo.line, lo.offset_from_line, len, srcmap + ci);                      \
  exit(1);
#define UNMATCHEDPAREN_EXIT(srcmap, ci)                                        \
  struct lineAndOffset lo = find_char_line(srcmap, ci);                        \
  printf("\x1b[1;31mParser()\x1b[m: %lu:%lu => "                               \
         "\x1b[1;31munmatched "                                                \
         "paren\x1b[m\n",                                                      \
         lo.line, lo.offset_from_line);                                        \
  exit(1);

enum parseContextType { contextExpr, contextType };

struct parseContext {
  enum parseContextType pctt;
};

// single expr step parser
size_t Parser(const token_t *ts, size_t ts_length, struct parserEnv *env,
              struct parseContext pctx, size_t depth, size_t env_idx,
              abstractSyntaxTree *ast) {
}
typedef struct parserEnv parserEnv;

void hashmap_test() {
  hashMap m = {.ks = calloc(HASHMAP_DEFAULT_CAPACITY, sizeof(hashMapEntry)),
               .v = calloc(HASHMAP_DEFAULT_CAPACITY, sizeof(void *)),
               .cap = HASHMAP_DEFAULT_CAPACITY};
  uint8_t k0[] = {'x'};
  uint8_t v0[] = "hello!";
  uint8_t k1[] =
      "The desire of the sluggard kills him, for his hands refuse to labor.";
  uint8_t v1[] = "birb";
  hashMapInsert(&m, k0, sizeof(k0), v0);
  hashMapInsert(&m, k1, sizeof(k1), v1);
  assert(hashMapRetr(&m, k0, sizeof(k0)) == v0);
  assert(hashMapRetr(&m, k1, sizeof(k1)) == v1);
  assert(hashMapContains(&m, k0, sizeof(k0)));
  assert(hashMapContains(&m, k1, sizeof(k1)));
  hashMapPullOut(&m, k0, sizeof(k0));
  hashMapPullOut(&m, k1, sizeof(k1));
  assert(!hashMapContains(&m, k0, sizeof(k0)));
  assert(!hashMapContains(&m, k1, sizeof(k1)));
  free(m.ks);
  free(m.v);
}

// meh realloc should work
void arena_test() {
  arena a = {.v = malloc(314), .len = 0, .cap = 314};
  uint64_t v0 = 3958395;
  *(uint64_t *)arenaAlloc(&a, 8) = v0;
  assert(*(uint64_t *)(a.v) == v0);
  free(a.v);
}

int main(int argc, char **argv) {
  if (argc <= 1) {
    printf("please pass filename\n");
    return 1;
  }
  int fd = open(argv[1], O_RDONLY);
  if (fd < 0) {
    printf(
        "cant open file. please check if it is readable and actually exists\n");
    return 1;
  }
  struct stat file_stat;
  fstat(fd, &file_stat);
  uint8_t *map = mmap(0, file_stat.st_size, PROT_READ, MAP_SHARED, fd, 0);
  if (map == 0) {
    printf("map failed\n");
    return 1;
  }
  // default token array size
  size_t ts_size = file_stat.st_size / 2 * sizeof(token_t);
  size_t misc = 0;
  arena ts = {.v = calloc(ts_size, 1), .cap = ts_size, .len = 0, .count = 0};
  arena stra = {.v = calloc(1024, 1), .cap = 1024, .len = 0, .count = 0};
  tokenize_normal(map, file_stat.st_size, &ts, &stra);
  for (size_t i = 0; i < ts.count; i++) {
    printf("%s\n", TokenTypeString[((token_t *)ts.v)[i].token_type]);
  }
  arena parser_arena = {.v = calloc(0xFFFF, 1), .cap = 0xFFFF, .len = 0};
  hashMap symbols = {.ks =
                         calloc(HASHMAP_DEFAULT_CAPACITY, sizeof(hashMapEntry)),
                     .v = calloc(HASHMAP_DEFAULT_CAPACITY, sizeof(envSymbol *)),
                     .cap = HASHMAP_DEFAULT_CAPACITY};
  struct parserEnv env = {
      .srcmap = map, .arena = &parser_arena, .symbols = &symbols};
  free(ts.v);
  free(stra.v);
  hashmap_test();
  arena_test();
  return 0;
}