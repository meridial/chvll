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

// 1 = decimal
// 2 = other radix
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
    return 2;
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

size_t IntegerParse(const uint8_t *s, size_t len, int it) {
  if (it == 1) {
    return IntegerDecimalOrSubDecimalParse(s, len, 10);
  }
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
    token_t *t = vectorAlloc(tv, sizeof(token_t));                             \
    t->token_type = (size_t)hashMapRetr(&tkhm, srcmap + current_token_start,   \
                                        ci - current_token_start);             \
    t->len = ci - current_token_start;                                         \
    t->character_offset = current_token_start;                                 \
    t->s = (size_t)srcmap + current_token_start;                               \
    current_token_start = ci;                                                  \
  }

void tokenize_normal(const uint8_t *srcmap, size_t len, arena *a, vector *tv) {
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
        token_t *t = vectorAlloc(tv, sizeof(token_t));
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
      uint8_t *sb = arenaAlloc(a, cci - ci);
      size_t nsl = StringEscape(sb, srcmap + ci, cci - ci);
      if (!nsl) {
        MALFORMSTRING_EXIT(srcmap, ci, cci - ci)
      }
      token_t *t = vectorAlloc(tv, sizeof(token_t));
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
      int it = is_string_valid_number(srcmap + ci, cci - ci);
      if (!it) {
        INVALIDNUMBER_EXIT(srcmap, ci, cci - ci)
      }
      token_t *t = vectorAlloc(tv, sizeof(token_t));
      t->s = is_negative ? -IntegerParse(srcmap + ci + 1, cci - ci - 1, it)
                         : IntegerParse(srcmap + ci, cci - ci, it);
      t->character_offset = ci;
      t->token_type = is_negative ? TokenNegativeInteger : TokenInteger;
      ci = cci;
      current_token_start = cci;
      continue;
    }
    ci++;
  }
  token_backwards_check();
  return;
}

// global context
struct parserEnv {
  const uint8_t *srcmap;
  allocator_t *allocator;
  hashMap *symbols; // str ,symbol
  vector *haystack; // packed alloc
  vector
      *astack; // abstractSyntaxTree*[] => haystack. this is actual parser stack
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
#define STRAYARGS_EXIT(srcmap, ci)                                             \
  struct lineAndOffset lo = find_char_line(srcmap, ci);                        \
  printf("\x1b[1;31mParser()\x1b[m: %lu:%lu => "                               \
         "\x1b[1;31mstray argument\x1b[m\n",                                   \
         lo.line, lo.offset_from_line);                                        \
  exit(1);
#define UNEXPECTEDEXPR_EXIT(srcmap, ci)                                        \
  struct lineAndOffset lo = find_char_line(srcmap, ci);                        \
  printf("\x1b[1;31mParser()\x1b[m: %lu:%lu => "                               \
         "\x1b[1;31mdid not expect an expression\x1b[m\n",                     \
         lo.line, lo.offset_from_line);                                        \
  exit(1);
#define EXPECTEDEXPR_EXIT(srcmap, ci)                                          \
  struct lineAndOffset lo = find_char_line(srcmap, ci);                        \
  printf("\x1b[1;31mParser()\x1b[m: %lu:%lu => "                               \
         "\x1b[1;31mexpected an expression\x1b[m\n",                           \
         lo.line, lo.offset_from_line);                                        \
  exit(1);
#define INVALIDINLINEARGS_EXIT(srcmap, ci)                                     \
  struct lineAndOffset lo = find_char_line(srcmap, ci);                        \
  printf("\x1b[1;31mParser()\x1b[m: %lu:%lu => "                               \
         "\x1b[1;31mnot invalid arguments for an inline operation\x1b[m\n",    \
         lo.line, lo.offset_from_line);                                        \
  exit(1);

// immediate level context
enum parseContextType { contextNormal, contextType };

#define PCTT_NEGATIVE_INT ((uint8_t)1 << 15)

struct parseContext {
  uint16_t pctt;
  uint16_t envdepth;
  uint64_t env_idx;
  size_t astack_base;
};

void ParseTuple(const token_t *ts, size_t ts_length, struct parserEnv *env,
                struct parseContext pctx) {}

size_t Parser(size_t ti, const token_t *ts, size_t ts_length,
              struct parserEnv *env, struct parseContext pctx) {
  if (ts_length - ti == 0) {
    abort();
  }
  while (1) {
    token_t ct = ts[ti];
    switch (ct.token_type) {
    case TokenParenClose:
      if (env->astack->len == pctx.astack_base) {
        EXPECTEDEXPR_EXIT(env->srcmap, ts[ti].character_offset)
      }
      return ti;
    case TokenParenOpen:
      if (ts_length < 2) {
        UNMATCHEDPAREN_EXIT(env->srcmap, ct.character_offset);
      }
      // stop parsing before entering malformed expression
      size_t d = 1;
      size_t i = ti + 1;
      for (;; i++) {
        if (i == ts_length) {
          UNMATCHEDPAREN_EXIT(env->srcmap, ct.character_offset);
        }
        switch (ts[i].token_type) {
        case TokenParenOpen:
          d++;
          break;
        case TokenParenClose:
          d--;
          if (d == 0) {
            goto p_ok;
          }
          continue;
        default:
          continue;
        }
      }
    p_ok:
      ti = Parser(ti + 1, ts, i + 1, env,
                  (struct parseContext){.pctt = pctx.pctt,
                                        .envdepth = pctx.envdepth + 1,
                                        .env_idx = pctx.env_idx + 1,
                                        .astack_base = env->astack->len});
      goto mcon;
    case TokenUnit:
      const abstractSyntaxTree **uasth =
          vectorAlloc(env->astack, sizeof(abstractSyntaxTree *));
      *uasth = &UnitAst;
      goto mcon;
      break;
    case TokenNegativeInteger:
      pctx.pctt |= PCTT_NEGATIVE_INT;
    case TokenInteger:
      abstractSyntaxTree *ca =
          vectorAlloc(env->haystack, sizeof(abstractSyntaxTree));
      *ca = (abstractSyntaxTree){
          .leaf_type = LeafScalar, .scalar_int = ct.s, .expr_class = &isize};
      abstractSyntaxTree **cah =
          vectorAlloc(env->astack, sizeof(abstractSyntaxTree *));
      *cah = ca;
      goto mcon;
      break;
    case TokenAdd:
    case TokenSub:
    case TokenAsterisk:
    case TokenForwardSlash:
      if (env->astack->len - pctx.astack_base <
          2 * sizeof(abstractSyntaxTree *)) {
        printf("closures not implemented yet so no currying. sorry\n");
        exit(4); // TODO: implement closures
      }
      abstractSyntaxTree **a0 = (abstractSyntaxTree **)vectorPop(
          env->astack, sizeof(abstractSyntaxTree *) * 2);
      abstractSyntaxTree *v =
          vectorAlloc(env->haystack, sizeof(abstractSyntaxTree));
      v->op.a0 = a0[0];
      v->op.a1 = a0[1];
      abstractSyntaxTree **vh =
          vectorAlloc(env->astack, sizeof(abstractSyntaxTree *));
      *vh = v;
      switch (ct.token_type) {
      case TokenAdd:
        v->leaf_type = LeafAdd;
        break;
      case TokenSub:
        v->leaf_type = LeafSub;
        break;
      case TokenAsterisk:
        v->leaf_type = LeafMul;
        break;
      case TokenForwardSlash:
        v->leaf_type = LeafDiv;
        break;
      }
      goto mcon;
      break;
    }
    goto con;
  mcon:
    abstractSyntaxTree *c = *(((abstractSyntaxTree **)env->astack->v) - 1);
    c->ci = ts[ti].character_offset;
  con:
    if (ts_length - ti == 1) {
      break;
    }
    ti++;
  }
  if (env->astack->len < pctx.astack_base) {
    abort(); // ????
  }
  if (env->astack->len - pctx.astack_base != 1 * sizeof(abstractSyntaxTree *)) {
    STRAYARGS_EXIT(env->srcmap, ts[ti].character_offset)
  }
  return ti;
}

void print_ast(abstractSyntaxTree *a, size_t depth) {
  switch (a->leaf_type) {
  case LeafUnit:
    printf("%*sunit\n", depth);
    return;
  case LeafScalar:
    printf("%*sscalar: %ld\n", depth, "", a->scalar_int);
    return;
  case LeafAdd:
    printf("%*s+\n", depth, "");
    goto print_op_args;
  case LeafSub:
    printf("%*s-\n", depth, "");
    goto print_op_args;
  case LeafMul:
    printf("%*s*\n", depth, "");
    goto print_op_args;
  case LeafDiv:
    printf("%*s/\n", depth, "");
    goto print_op_args;
  default:
    exit(10);
    break;
  }
print_op_args:
  print_ast(a->op.a0, depth + 1);
  print_ast(a->op.a1, depth + 1);
  return;
}

void hashmap_test(allocator_t *al) {

  hashMap m = {.ks = al->alloc(al->allocator, 2 * sizeof(str)),
               .v = al->alloc(al->allocator, 2 * sizeof(void *)),
               .cap = 2,
               .allocator = al};
  uint8_t k0[] = "dsgssdf";
  uint8_t v0[] = "hello!";
  uint8_t k1[] = {'x'};
  uint8_t v1[] = "hello!";
  uint8_t k2[] =
      "The desire of the sluggard kills him, for his hands refuse to labor.";
  uint8_t v2[] = "birb";
  hashMapInsert(&m, k0, sizeof(k0), v0);
  hashMapInsert(&m, k1, sizeof(k1), v1);
  hashMapInsert(&m, k2, sizeof(k2), v2);
  assert(hashMapRetr(&m, k0, sizeof(k0)) == v0);
  assert(hashMapRetr(&m, k1, sizeof(k1)) == v1);
  assert(hashMapRetr(&m, k2, sizeof(k2)) == v2);
  assert(hashMapContains(&m, k0, sizeof(k0)));
  assert(hashMapContains(&m, k1, sizeof(k1)));
  assert(hashMapContains(&m, k2, sizeof(k2)));
  hashMapPullOut(&m, k0, sizeof(k0));
  hashMapPullOut(&m, k1, sizeof(k1));
  hashMapPullOut(&m, k2, sizeof(k2));
  assert(!hashMapContains(&m, k0, sizeof(k0)));
  assert(!hashMapContains(&m, k1, sizeof(k1)));
  assert(!hashMapContains(&m, k2, sizeof(k2)));
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
  struct stat fst;
  fstat(fd, &fst);
  uint8_t *map = mmap(0, fst.st_size, PROT_READ, MAP_SHARED, fd, 0);
  if (map == 0) {
    printf("map failed\n");
    return 1;
  }
  if (!fst.st_size) {
    printf("empty file\n");
    return 1;
  }
  arena a = {.head = makeRegion(ARENA_REGION_DEFAULT_CAPACITY << 3)};
  allocator_t allocator_interface = {.alloc = arenaAlloc_AllocatorInterface,
                                     .realloc = arenaReAlloc_AllocatorInterface,
                                     .free = arenaFree_AllocatorInterface,
                                     .allocator = &a};
  hashmap_test(&allocator_interface);
  vector tv = {.allocator = &allocator_interface,
               .cap = ARENA_REGION_DEFAULT_CAPACITY,
               .len = 0,
               .v = arenaAlloc(&a, ARENA_REGION_DEFAULT_CAPACITY)};
  tokenize_normal(map, fst.st_size, &a, &tv);
  for (size_t i = 0; i < tv.len / sizeof(token_t); i++) {
    printf("%s\n", TokenTypeString[((token_t *)tv.v)[i].token_type]);
  }
  struct parserEnv env = {
      .allocator = &allocator_interface,
      .srcmap = map,
      .symbols =
          &(hashMap){
              .ks = arenaAlloc(&a, HASHMAP_DEFAULT_CAPACITY * sizeof(str)),
              .v = arenaAlloc(&a, HASHMAP_DEFAULT_CAPACITY * sizeof(void *)),
              .cap = HASHMAP_DEFAULT_CAPACITY,
              .allocator = &allocator_interface,
          },

      .haystack = &(vector){.allocator = &allocator_interface,
                            .cap = ARENA_REGION_DEFAULT_CAPACITY,
                            .len = 0,
                            .v = arenaAlloc(&a, ARENA_REGION_DEFAULT_CAPACITY)},
      .astack = &(vector){.allocator = &allocator_interface,
                          .cap = ARENA_REGION_DEFAULT_CAPACITY,
                          .len = 0,
                          .v = arenaAlloc(&a, ARENA_REGION_DEFAULT_CAPACITY)}};
  hashMapCopyInternal(builtinsymhm.ks, builtinsymhm.v, builtinsymhm.cap,
                      env.symbols->ks, env.symbols->v, env.symbols->cap);
  abstractSyntaxTree *ast = arenaAlloc(&a, sizeof(abstractSyntaxTree));
  Parser(0, tv.v, tv.len / sizeof(token_t), &env,
         (struct parseContext){
             .pctt = contextNormal,
             .envdepth = 0,
             .env_idx = 0,
         });
  print_ast(((abstractSyntaxTree **)env.astack->v)[0], 0);
  arenaFree(&a);
  return 0;
}