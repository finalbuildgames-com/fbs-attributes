/*
 * tests/test_attributes.c — witnesses for include/fbs/attributes.h.
 *
 * Self-contained: no test framework. Exit code = number of failures (clamped
 * to 100 so it survives the 8-bit exit status; the true count is printed).
 *
 * Covers the test plan witnesses A-T1..A-T11 and A-T13..A-T15
 * (A-T12 is void because no fold/stack helper exists), plus allocator failure,
 * capacity exhaustion, every E_TRUNCATED path, NULL/bad-enum validation on
 * every entry point, the status-name/version functions and a committed golden
 * serialization fixture.
 *
 *   ./fbs_test_attributes                    compare against tests/fixtures/attributes/attr.bin
 *   ./fbs_test_attributes --write-fixtures   rewrite that file
 *   ./fbs_test_attributes --fixture-dir DIR  look for fixtures under DIR
 */

#include "fbs/attributes.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* Harness                                                                   */
/* ------------------------------------------------------------------------- */

static int g_checks = 0;
static int g_fails = 0;

static void check_impl(int cond, const char *expr, const char *file, int line) {
  ++g_checks;
  if (!cond) {
    ++g_fails;
    printf("FAIL %s:%d: %s\n", file, line, expr);
  }
}

#define CHECK(expr) check_impl((expr) ? 1 : 0, #expr, __FILE__, __LINE__)

static const char *g_fixture_dir = "fixtures/attributes";
static int g_write_fixtures = 0;

/* ------------------------------------------------------------------------- */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------- */

static uint64_t bits_of(double v) {
  uint64_t b;
  memcpy(&b, &v, sizeof b);
  return b;
}

static double make_nan(void) {
  uint64_t b = 0x7ff8000000000000ULL;
  double d;
  memcpy(&d, &b, sizeof d);
  return d;
}

static double make_inf(void) {
  uint64_t b = 0x7ff0000000000000ULL;
  double d;
  memcpy(&d, &b, sizeof d);
  return d;
}

static fbs_attr *make_attr(int type, double base, unsigned cap) {
  fbs_attr_config cfg = fbs_attr_config_default();
  fbs_attr *a = NULL;
  cfg.type = type;
  cfg.base = base;
  cfg.max_modifiers = cap;
  CHECK(fbs_attr_create(&cfg, NULL, &a) == FBS_ATTR_OK);
  return a;
}

static fbs_attr_handle add_mod(fbs_attr *a, unsigned channel, int op, double value) {
  fbs_attr_handle h = FBS_ATTR_HANDLE_NONE;
  CHECK(fbs_attr_add(a, (uint16_t)channel, op, value, &h) == FBS_ATTR_OK);
  CHECK(h != FBS_ATTR_HANDLE_NONE);
  return h;
}

static unsigned char *serialize_alloc(const fbs_attr *a, size_t *out_len) {
  size_t need = fbs_attr_serialized_size(a);
  size_t written = 0;
  unsigned char *buf = (unsigned char *)malloc(need ? need : 1u);
  CHECK(buf != NULL);
  CHECK(fbs_attr_serialize(a, buf, need, &written) == FBS_ATTR_OK);
  CHECK(written == need);
  *out_len = need;
  return buf;
}

static void expect_schema_reject(const unsigned char *blob, size_t len, const char *what) {
  fbs_attr *a = (fbs_attr *)0x1;
  fbs_attr_status st = fbs_attr_deserialize(blob, len, NULL, NULL, &a);
  ++g_checks;
  if (st != FBS_ATTR_E_SCHEMA || a != (fbs_attr *)0x1) {
    ++g_fails;
    printf("FAIL schema rejection (%s): status %s, out %s\n", what, fbs_attr_status_name(st),
           a == (fbs_attr *)0x1 ? "untouched" : "WRITTEN");
    if (st == FBS_ATTR_OK) fbs_attr_destroy(a);
  }
}

/* Deterministic 32-bit xorshift. */
static uint32_t g_rng = 0x9e3779b9u;

static void rng_seed(uint32_t seed) { g_rng = seed ? seed : 1u; }

static uint32_t rng_next(void) {
  uint32_t x = g_rng;
  x = (uint32_t)(x ^ (x << 13));
  x = (uint32_t)(x ^ (x >> 17));
  x = (uint32_t)(x ^ (x << 5));
  g_rng = x;
  return x;
}

static unsigned rng_below(unsigned n) { return n ? (unsigned)(rng_next() % n) : 0u; }

typedef struct {
  int allocs;
  int frees;
  size_t bytes;
  int budget;
} counting_alloc;

static void *ca_alloc(void *user, size_t bytes) {
  counting_alloc *c = (counting_alloc *)user;
  if (c->budget == 0) return NULL;
  if (c->budget > 0) --c->budget;
  ++c->allocs;
  c->bytes += bytes;
  return malloc(bytes);
}

static void ca_free(void *user, void *ptr) {
  counting_alloc *c = (counting_alloc *)user;
  if (!ptr) return;
  ++c->frees;
  free(ptr);
}

/* ------------------------------------------------------------------------- */
/* A-T1 — identity: no modifiers means value == base                         */
/* ------------------------------------------------------------------------- */

static void check_identity(int type, double base) {
  fbs_attr *a = make_attr(type, base, 4u);
  CHECK(fbs_attr_type_of(a) == type);
  CHECK(bits_of(fbs_attr_base(a)) == bits_of(base));
  CHECK(bits_of(fbs_attr_value(a)) == bits_of(base));
  CHECK(fbs_attr_count(a) == 0u);
  CHECK(fbs_attr_revision(a) == 0u);
  CHECK(fbs_attr_has_clamp(a, NULL, NULL) == 0);
  fbs_attr_destroy(a);
}

static void test_at1_identity(void) {
  check_identity(FBS_ATTR_F64, 0.0);
  check_identity(FBS_ATTR_F64, 7.25);
  check_identity(FBS_ATTR_F64, -12.5);
  check_identity(FBS_ATTR_F32, 0.0);
  check_identity(FBS_ATTR_F32, 0.5);
  check_identity(FBS_ATTR_F32, -3.25);
  check_identity(FBS_ATTR_I32, 0.0);
  check_identity(FBS_ATTR_I32, 42.0);
  check_identity(FBS_ATTR_I32, -42.0);

  /* the type conversion still applies with no modifiers */
  {
    fbs_attr *a = make_attr(FBS_ATTR_F32, 0.1, 4u);
    CHECK(bits_of(fbs_attr_value(a)) == bits_of((double)(float)0.1));
    CHECK(bits_of(fbs_attr_value(a)) != bits_of(0.1));
    fbs_attr_destroy(a);
  }
  {
    fbs_attr *a = make_attr(FBS_ATTR_I32, 2.5, 4u);
    CHECK(fbs_attr_value(a) == 3.0);
    CHECK(fbs_attr_base(a) == 2.5);
    fbs_attr_destroy(a);
  }
}

/* ------------------------------------------------------------------------- */
/* A-T2 — order independence over every permutation (the flagship test)      */
/* ------------------------------------------------------------------------- */

typedef struct {
  unsigned channel;
  int op;
  double value;
} mod_spec;

static int next_permutation(unsigned *a, unsigned n) {
  unsigned i, j, k, t;
  if (n < 2u) return 0;
  i = n - 1u;
  while (i > 0u && a[i - 1u] >= a[i]) --i;
  if (i == 0u) return 0;
  j = n - 1u;
  while (a[j] <= a[i - 1u]) --j;
  t = a[i - 1u];
  a[i - 1u] = a[j];
  a[j] = t;
  for (j = i, k = n - 1u; j < k; ++j, --k) {
    t = a[j];
    a[j] = a[k];
    a[k] = t;
  }
  return 1;
}

/* Accumulation inside a (channel, op) bucket is ordered by ascending
 * (value, handle), so the summation order depends only on the multiset of
 * values, never on insertion order or on which handles were issued. Bit
 * equality across all 720 permutations therefore has to hold for arbitrary
 * values, not just for dyadic ones whose partial sums happen to be exact —
 * sets C and D below are the non-dyadic, genuinely order-sensitive cases. */
static void run_permutation_test(const mod_spec *specs, unsigned n, double base,
                                 double expected, const char *label) {
  unsigned perm[8];
  unsigned i;
  unsigned permutations = 0u;
  uint64_t first_bits = 0u;
  int mismatch = 0;

  for (i = 0u; i < n; ++i) perm[i] = i;
  do {
    fbs_attr *a = make_attr(FBS_ATTR_F64, base, 8u);
    uint64_t got;
    for (i = 0u; i < n; ++i)
      (void)add_mod(a, specs[perm[i]].channel, specs[perm[i]].op, specs[perm[i]].value);
    got = bits_of(fbs_attr_value(a));
    if (permutations == 0u) first_bits = got;
    if (got != first_bits || got != bits_of(expected)) mismatch = 1;
    CHECK(fbs_attr_count(a) == n);
    fbs_attr_destroy(a);
    ++permutations;
  } while (next_permutation(perm, n));

  CHECK(permutations == 720u);
  ++g_checks;
  if (mismatch) {
    ++g_fails;
    printf("FAIL A-T2 (%s): permutations do not agree bit for bit\n", label);
  }
}

static void test_at2_order_independence(void) {
  /* 6 modifiers, 4 ops, 2 channels.
   * ch0: (8 + 10 + 2.5) * (1 + 0.5) * (1 + 0.25) = 20.5 * 1.5 * 1.25 = 38.4375
   * ch1: 38.4375 * (1 + 0.125) + 3 = 43.2421875 + 3 = 46.2421875 */
  static const mod_spec set_a[6] = {{0u, FBS_ATTR_ADD, 10.0},
                                    {0u, FBS_ATTR_ADD, 2.5},
                                    {0u, FBS_ATTR_MUL_ADD, 0.5},
                                    {0u, FBS_ATTR_MUL_COMPOUND, 0.25},
                                    {1u, FBS_ATTR_MUL_ADD, 0.125},
                                    {1u, FBS_ATTR_ADD_FINAL, 3.0}};
  /* 6 modifiers, 5 ops, 3 channels; the single OVERRIDE in ch1 discards the
   * ADD next to it no matter when either was inserted.
   * ch0: (2 + 4) * 1.5 = 9; ch1: override -> 7.5; ch2: 7.5 * 1.5 - 1.25 = 10 */
  static const mod_spec set_b[6] = {{0u, FBS_ATTR_ADD, 4.0},
                                    {0u, FBS_ATTR_MUL_ADD, 0.5},
                                    {1u, FBS_ATTR_OVERRIDE, 7.5},
                                    {1u, FBS_ATTR_ADD, 100.0},
                                    {2u, FBS_ATTR_MUL_COMPOUND, 0.5},
                                    {2u, FBS_ATTR_ADD_FINAL, -1.25}};

  /* Non-dyadic, 4 ops, 2 channels. The ch0 ADD bucket {0.1, 0.2, 0.3} is
   * genuinely order-sensitive in IEEE-754, and base 0 was chosen so the
   * resulting 1-ulp difference is not absorbed by a larger leading term but
   * reaches the final value: under the old ascending-handle rule these 720
   * permutations produce two distinct results, under ascending (value, handle)
   * exactly one. */
  static const mod_spec set_c[6] = {{0u, FBS_ATTR_ADD, 0.1},
                                    {0u, FBS_ATTR_ADD, 0.2},
                                    {0u, FBS_ATTR_ADD, 0.3},
                                    {0u, FBS_ATTR_MUL_ADD, 1.0 / 3.0},
                                    {1u, FBS_ATTR_MUL_COMPOUND, 0.7},
                                    {1u, FBS_ATTR_ADD_FINAL, 1.0e-3}};
  /* Two equal values in one bucket (the tie-break by handle must not leak into
   * the result), and the bucket is still order-sensitive as a whole; likewise
   * two distinct results under the old rule, one under this one. */
  static const mod_spec set_d[6] = {{0u, FBS_ATTR_ADD, 0.1},
                                    {0u, FBS_ATTR_ADD, 0.1},
                                    {0u, FBS_ATTR_ADD, 0.2},
                                    {0u, FBS_ATTR_ADD, 0.3},
                                    {0u, FBS_ATTR_MUL_ADD, 1.0e-3},
                                    {1u, FBS_ATTR_ADD_FINAL, 0.7}};
  double expected_c, expected_d;

  run_permutation_test(set_a, 6u, 8.0, 46.2421875, "4 ops / 2 channels");
  run_permutation_test(set_b, 6u, 2.0, 10.0, "5 ops / 3 channels with an override");

  /* Witnesses that both non-dyadic buckets really are order-sensitive, so the
   * sweeps below have teeth rather than passing by luck. */
  {
    double p[4];
    p[0] = 0.1;
    p[1] = 0.2;
    p[2] = 0.3;
    CHECK(((p[0] + p[1]) + p[2]) != ((p[1] + p[2]) + p[0]));
    p[0] = 0.1;
    p[1] = 0.1;
    p[2] = 0.2;
    p[3] = 0.3;
    /* ascending gives 0.7, this other order gives 0.7000000000000001 */
    CHECK((((p[0] + p[1]) + p[2]) + p[3]) != (((p[0] + p[2]) + p[3]) + p[1]));
  }

  /* Expected values, written in exactly the order the contract prescribes,
   * from a base of 0. */
  expected_c = (0.0 + ((0.1 + 0.2) + 0.3)) * (1.0 + (1.0 / 3.0)); /* ch0 */
  expected_c = expected_c * 1.0;
  expected_c = expected_c + 0.0;
  expected_c = (expected_c + 0.0) * (1.0 + 0.0); /* ch1 */
  expected_c = expected_c * (1.0 + 0.7);
  expected_c = expected_c + 1.0e-3;

  expected_d = (0.0 + (((0.1 + 0.1) + 0.2) + 0.3)) * (1.0 + 1.0e-3); /* ch0 */
  expected_d = expected_d * 1.0;
  expected_d = expected_d + 0.0;
  expected_d = (expected_d + 0.0) * (1.0 + 0.0); /* ch1 */
  expected_d = expected_d * 1.0;
  expected_d = expected_d + 0.7;

  run_permutation_test(set_c, 6u, 0.0, expected_c, "non-dyadic, 4 ops / 2 channels");
  run_permutation_test(set_d, 6u, 0.0, expected_d, "non-dyadic with two equal values in a bucket");
}

/* A-T2, extended past hand-picked sets: the same property over randomly
 * generated multisets of arbitrary (non-dyadic) values. Each multiset is built
 * into an attribute eight times in eight different random insertion orders,
 * and every build must produce a bit-identical cached value. A base of 0 keeps
 * a one-ulp difference in a bucket sum from being absorbed by a larger leading
 * term, so a regression in the accumulation order surfaces instead of being
 * rounded away.
 *
 * OVERRIDE is excluded on purpose: two overrides in one channel are resolved by
 * lowest handle, which is insertion order by definition, so that operation is
 * deliberately not permutation invariant and is covered by A-T5 instead. */
static void test_at2_random_multisets(void) {
  static const double values[12] = {0.1,  0.2,  0.3, 0.7,       1.0 / 3.0, 1.0e-3,
                                    -0.1, -0.3, 0.9, 1.0 / 7.0, 0.25,      -1.0 / 9.0};
  unsigned trial;
  int mismatch = 0;
  int add_failed = 0;

  rng_seed(0xa7c0ffeeu);
  for (trial = 0u; trial < 400u; ++trial) {
    mod_spec specs[8];
    unsigned n = 5u + rng_below(4u); /* 5..8 modifiers */
    unsigned i, round;
    uint64_t first = 0u;

    for (i = 0u; i < n; ++i) {
      specs[i].channel = rng_below(2u);
      specs[i].op = (int)rng_below(4u); /* ADD, MUL_ADD, MUL_COMPOUND, ADD_FINAL */
      specs[i].value = values[rng_below(12u)];
    }

    for (round = 0u; round < 8u; ++round) {
      unsigned order[8];
      fbs_attr *a = make_attr(FBS_ATTR_F64, 0.0, 8u);
      uint64_t got;
      for (i = 0u; i < n; ++i) order[i] = i;
      for (i = n; i-- > 1u;) { /* Fisher-Yates */
        unsigned j = rng_below(i + 1u);
        unsigned t = order[i];
        order[i] = order[j];
        order[j] = t;
      }
      for (i = 0u; i < n; ++i) {
        fbs_attr_handle h = FBS_ATTR_HANDLE_NONE;
        if (fbs_attr_add(a, (uint16_t)specs[order[i]].channel, specs[order[i]].op,
                         specs[order[i]].value, &h) != FBS_ATTR_OK)
          add_failed = 1;
      }
      got = bits_of(fbs_attr_value(a));
      if (round == 0u)
        first = got;
      else if (got != first)
        mismatch = 1;
      fbs_attr_destroy(a);
    }
  }

  ++g_checks;
  if (add_failed) {
    ++g_fails;
    printf("FAIL A-T2 (random multisets): a modifier could not be added\n");
  }
  ++g_checks;
  if (mismatch) {
    ++g_fails;
    printf("FAIL A-T2 (random multisets): insertion order changed the value\n");
  }
}

/* ------------------------------------------------------------------------- */
/* A-T3 — bucket algebra, exact (restated without DIV_ADD per decision s8)    */
/* ------------------------------------------------------------------------- */

static void test_at3_bucket_algebra(void) {
  fbs_attr *a = make_attr(FBS_ATTR_F64, 100.0, 8u);
  CHECK(fbs_attr_value(a) == 100.0);

  (void)add_mod(a, 0u, FBS_ATTR_ADD, 10.0);
  CHECK(fbs_attr_value(a) == 110.0);

  (void)add_mod(a, 0u, FBS_ATTR_MUL_ADD, 0.5);
  CHECK(fbs_attr_value(a) == 165.0); /* 110 * 1.5 */
  (void)add_mod(a, 0u, FBS_ATTR_MUL_ADD, 0.5);
  CHECK(fbs_attr_value(a) == 220.0); /* 110 * (1 + 1.0) */

  (void)add_mod(a, 0u, FBS_ATTR_MUL_COMPOUND, 0.5);
  CHECK(fbs_attr_value(a) == 330.0); /* 220 * 1.5 */

  (void)add_mod(a, 0u, FBS_ATTR_ADD_FINAL, 5.0);
  CHECK(fbs_attr_value(a) == 335.0);

  /* MUL_COMPOUND compounds, MUL_ADD does not */
  (void)add_mod(a, 0u, FBS_ATTR_MUL_COMPOUND, 0.5);
  CHECK(fbs_attr_value(a) == 500.0); /* 110 * 2 * 1.5 * 1.5 + 5 */

  /* a MUL_ADD sum below -1 flips the sign, as documented */
  {
    fbs_attr *b = make_attr(FBS_ATTR_F64, 10.0, 4u);
    (void)add_mod(b, 0u, FBS_ATTR_MUL_ADD, -1.5);
    CHECK(fbs_attr_value(b) == -5.0);
    fbs_attr_destroy(b);
  }
  fbs_attr_destroy(a);
}

/* ------------------------------------------------------------------------- */
/* A-T4 — channel ordering                                                   */
/* ------------------------------------------------------------------------- */

static void test_at4_channel_ordering(void) {
  fbs_attr *a = make_attr(FBS_ATTR_F64, 10.0, 4u);
  fbs_attr *b = make_attr(FBS_ATTR_F64, 10.0, 4u);

  /* (10 + 10) * 2 = 40 */
  (void)add_mod(a, 0u, FBS_ATTR_ADD, 10.0);
  (void)add_mod(a, 1u, FBS_ATTR_MUL_ADD, 1.0);
  CHECK(fbs_attr_value(a) == 40.0);

  /* swapping the channel numbers: 10 * 2 + 10 = 30 */
  (void)add_mod(b, 1u, FBS_ATTR_ADD, 10.0);
  (void)add_mod(b, 0u, FBS_ATTR_MUL_ADD, 1.0);
  CHECK(fbs_attr_value(b) == 30.0);
  CHECK(fbs_attr_value(a) != fbs_attr_value(b));

  /* channels are ascending numeric, with no requirement to be contiguous */
  {
    fbs_attr *c = make_attr(FBS_ATTR_F64, 1.0, 8u);
    (void)add_mod(c, 65535u, FBS_ATTR_ADD, 1.0);
    (void)add_mod(c, 7u, FBS_ATTR_MUL_ADD, 1.0);
    (void)add_mod(c, 0u, FBS_ATTR_ADD, 3.0);
    /* ch0: 1 + 3 = 4; ch7: 4 * 2 = 8; ch65535: 8 + 1 = 9 */
    CHECK(fbs_attr_value(c) == 9.0);
    fbs_attr_destroy(c);
  }
  fbs_attr_destroy(b);
  fbs_attr_destroy(a);
}

/* ------------------------------------------------------------------------- */
/* A-T5 — override, lowest handle wins, and it survives a round trip         */
/* ------------------------------------------------------------------------- */

static void test_at5_override(void) {
  fbs_attr *a = make_attr(FBS_ATTR_F64, 5.0, 8u);
  fbs_attr *loaded = NULL;
  fbs_attr_handle first, second;
  unsigned char *blob;
  size_t len = 0;

  (void)add_mod(a, 0u, FBS_ATTR_ADD, 100.0);
  (void)add_mod(a, 0u, FBS_ATTR_MUL_ADD, 3.0);
  CHECK(fbs_attr_value(a) == 420.0);

  first = add_mod(a, 0u, FBS_ATTR_OVERRIDE, 7.0);
  CHECK(fbs_attr_value(a) == 7.0); /* the whole channel is discarded */

  (void)add_mod(a, 1u, FBS_ATTR_ADD, 1.0);
  CHECK(fbs_attr_value(a) == 8.0); /* later channels still apply on top */

  second = add_mod(a, 0u, FBS_ATTR_OVERRIDE, 9.0);
  CHECK(second > first);
  CHECK(fbs_attr_value(a) == 8.0); /* the LOWEST handle wins */

  blob = serialize_alloc(a, &len);
  CHECK(fbs_attr_deserialize(blob, len, NULL, NULL, &loaded) == FBS_ATTR_OK);
  CHECK(fbs_attr_value(loaded) == 8.0); /* the tie-break survives save/load */
  CHECK(fbs_attr_count(loaded) == 5u);

  CHECK(fbs_attr_remove(loaded, first) == FBS_ATTR_OK);
  CHECK(fbs_attr_value(loaded) == 10.0); /* now the other override wins */
  CHECK(fbs_attr_remove(loaded, second) == FBS_ATTR_OK);
  CHECK(fbs_attr_value(loaded) == 421.0); /* buckets are back: (5+100)*4 + 1 */

  free(blob);
  fbs_attr_destroy(loaded);
  fbs_attr_destroy(a);
}

/* ------------------------------------------------------------------------- */
/* A-T6 — removal is exact                                                   */
/* ------------------------------------------------------------------------- */

static void test_at6_removal_is_exact(void) {
  static const mod_spec probes[8] = {{0u, FBS_ATTR_ADD, 3.5},
                                     {0u, FBS_ATTR_MUL_ADD, 0.25},
                                     {0u, FBS_ATTR_MUL_COMPOUND, 0.5},
                                     {0u, FBS_ATTR_ADD_FINAL, -1.25},
                                     {1u, FBS_ATTR_ADD, 8.0},
                                     {1u, FBS_ATTR_OVERRIDE, 2.5},
                                     {2u, FBS_ATTR_MUL_ADD, -0.5},
                                     {5u, FBS_ATTR_ADD_FINAL, 0.125}};
  fbs_attr *a = make_attr(FBS_ATTR_F64, 6.25, 16u);
  unsigned i;
  fbs_attr_handle stale;

  /* a non-trivial starting state */
  (void)add_mod(a, 0u, FBS_ATTR_ADD, 1.5);
  (void)add_mod(a, 1u, FBS_ATTR_MUL_ADD, 0.5);
  (void)add_mod(a, 3u, FBS_ATTR_MUL_COMPOUND, 0.25);

  for (i = 0u; i < 8u; ++i) {
    uint64_t before = bits_of(fbs_attr_value(a));
    unsigned count_before = fbs_attr_count(a);
    fbs_attr_handle h = add_mod(a, probes[i].channel, probes[i].op, probes[i].value);
    CHECK(fbs_attr_count(a) == count_before + 1u);
    CHECK(fbs_attr_remove(a, h) == FBS_ATTR_OK);
    CHECK(fbs_attr_count(a) == count_before);
    CHECK(bits_of(fbs_attr_value(a)) == before);
    CHECK(fbs_attr_remove(a, h) == FBS_ATTR_E_NOT_FOUND); /* removing twice */
    stale = h;
  }
  CHECK(fbs_attr_get(a, stale, NULL) == FBS_ATTR_E_INVALID);
  {
    fbs_attr_modifier m;
    CHECK(fbs_attr_get(a, stale, &m) == FBS_ATTR_E_NOT_FOUND);
    CHECK(fbs_attr_remove(a, FBS_ATTR_HANDLE_NONE) == FBS_ATTR_E_NOT_FOUND);
    CHECK(fbs_attr_get(a, FBS_ATTR_HANDLE_NONE, &m) == FBS_ATTR_E_NOT_FOUND);
  }
  fbs_attr_destroy(a);
}

/* ------------------------------------------------------------------------- */
/* A-T7 — handle stability across save/load                                  */
/* ------------------------------------------------------------------------- */

static void test_at7_handle_stability(void) {
  fbs_attr *a = make_attr(FBS_ATTR_F64, 1.0, 16u);
  fbs_attr *b = NULL;
  fbs_attr_handle h1, h2, h3, fresh = FBS_ATTR_HANDLE_NONE;
  fbs_attr_modifier m;
  unsigned char *blob;
  size_t len = 0;
  unsigned i;

  h1 = add_mod(a, 0u, FBS_ATTR_ADD, 2.0);
  h2 = add_mod(a, 1u, FBS_ATTR_MUL_ADD, 0.5);
  h3 = add_mod(a, 0u, FBS_ATTR_ADD_FINAL, 4.0);
  CHECK(h1 == 1u && h2 == 2u && h3 == 3u);

  blob = serialize_alloc(a, &len);
  CHECK(fbs_attr_deserialize(blob, len, NULL, NULL, &b) == FBS_ATTR_OK);

  /* the handles issued before the save still address the same modifiers */
  CHECK(fbs_attr_get(b, h2, &m) == FBS_ATTR_OK);
  CHECK(m.channel == 1u && m.op == FBS_ATTR_MUL_ADD && m.value == 0.5);
  CHECK(fbs_attr_remove(b, h2) == FBS_ATTR_OK);
  CHECK(fbs_attr_count(b) == 2u);

  /* a new handle collides with nothing, including the removed one */
  CHECK(fbs_attr_add(b, 0u, FBS_ATTR_ADD, 1.0, &fresh) == FBS_ATTR_OK);
  CHECK(fresh != h1 && fresh != h2 && fresh != h3);
  CHECK(fresh == 4u); /* next_handle was persisted */
  for (i = 0u; i < 5u; ++i) {
    fbs_attr_handle next = add_mod(b, 2u, FBS_ATTR_ADD, 0.0);
    CHECK(next > fresh);
    CHECK(next != h1 && next != h2 && next != h3);
  }
  free(blob);
  fbs_attr_destroy(b);
  fbs_attr_destroy(a);
}

/* ------------------------------------------------------------------------- */
/* A-T8 — int32 rounding, wide accumulation and range                        */
/* ------------------------------------------------------------------------- */

static void check_round(double base, double expected) {
  fbs_attr *a = make_attr(FBS_ATTR_I32, base, 2u);
  int32_t out = 0;
  CHECK(fbs_attr_value(a) == expected);
  CHECK(fbs_attr_value_i32(a, &out) == FBS_ATTR_OK);
  CHECK((double)out == expected);
  fbs_attr_destroy(a);
}

static void test_at8_int32(void) {
  /* half away from zero (C round), not banker's rounding */
  check_round(0.5, 1.0);
  check_round(-0.5, -1.0);
  check_round(1.5, 2.0);
  check_round(-1.5, -2.0);
  check_round(2.5, 3.0); /* banker's rounding would say 2 */
  check_round(-2.5, -3.0);
  check_round(3.5, 4.0);
  check_round(-3.5, -4.0);
  check_round(2.4999999999, 2.0);
  check_round(-2.4999999999, -2.0);

  /* accumulation happens in double: ten MUL_ADD of 0.1 on base 1 gives 2 */
  {
    fbs_attr *a = make_attr(FBS_ATTR_I32, 1.0, 16u);
    int32_t out = 0;
    unsigned i;
    for (i = 0u; i < 10u; ++i) (void)add_mod(a, 0u, FBS_ATTR_MUL_ADD, 0.1);
    CHECK(fbs_attr_value(a) == 2.0);
    CHECK(fbs_attr_value_i32(a, &out) == FBS_ATTR_OK);
    CHECK(out == 2);
    fbs_attr_destroy(a);
  }

  /* range */
  {
    int32_t out = 12345;
    fbs_attr *a = make_attr(FBS_ATTR_I32, 2147483647.0, 4u);
    CHECK(fbs_attr_value_i32(a, &out) == FBS_ATTR_OK);
    CHECK(out == 2147483647);
    fbs_attr_destroy(a);

    a = make_attr(FBS_ATTR_I32, -2147483648.0, 4u);
    CHECK(fbs_attr_value_i32(a, &out) == FBS_ATTR_OK);
    CHECK(out == (int32_t)(-2147483647 - 1));
    fbs_attr_destroy(a);

    a = make_attr(FBS_ATTR_I32, 2147483648.0, 4u);
    out = 12345;
    CHECK(fbs_attr_value_i32(a, &out) == FBS_ATTR_E_RANGE);
    CHECK(out == 12345); /* output untouched */
    fbs_attr_destroy(a);

    a = make_attr(FBS_ATTR_I32, 3.0e9, 4u);
    CHECK(fbs_attr_value_i32(a, &out) == FBS_ATTR_E_RANGE);
    fbs_attr_destroy(a);

    a = make_attr(FBS_ATTR_I32, -3.0e9, 4u);
    CHECK(fbs_attr_value_i32(a, &out) == FBS_ATTR_E_RANGE);
    fbs_attr_destroy(a);
  }

  /* value_i32 works for the float types too, with the same rounding rule */
  {
    fbs_attr *a = make_attr(FBS_ATTR_F64, 2.7, 4u);
    int32_t out = 0;
    CHECK(fbs_attr_value_i32(a, &out) == FBS_ATTR_OK);
    CHECK(out == 3);
    CHECK(fbs_attr_set_base(a, -2.5) == FBS_ATTR_OK);
    CHECK(fbs_attr_value_i32(a, &out) == FBS_ATTR_OK);
    CHECK(out == -3);
    fbs_attr_destroy(a);
  }

  /* an overflow to infinity is E_RANGE, not a wrapped integer */
  {
    fbs_attr *a = make_attr(FBS_ATTR_F64, 1.0e308, 8u);
    int32_t out = 0;
    (void)add_mod(a, 0u, FBS_ATTR_MUL_ADD, 1.0e10);
    CHECK(fbs_attr_value_i32(a, &out) == FBS_ATTR_E_RANGE);
    fbs_attr_destroy(a);
  }

  /* F32 rounds through float exactly once, at the end */
  {
    fbs_attr *a = make_attr(FBS_ATTR_F32, 1.0, 8u);
    (void)add_mod(a, 0u, FBS_ATTR_ADD, 0.1);
    CHECK(bits_of(fbs_attr_value(a)) == bits_of((double)(float)(1.0 + 0.1)));
    fbs_attr_destroy(a);
  }
}

/* ------------------------------------------------------------------------- */
/* A-T9 — clamping                                                           */
/* ------------------------------------------------------------------------- */

static void test_at9_clamping(void) {
  fbs_attr *a = make_attr(FBS_ATTR_F64, 50.0, 8u);
  fbs_attr_handle big;
  double lo = 0.0, hi = 0.0;

  CHECK(fbs_attr_set_clamp(a, 0.0, 100.0) == FBS_ATTR_OK);
  CHECK(fbs_attr_has_clamp(a, &lo, &hi) == 1);
  CHECK(lo == 0.0 && hi == 100.0);
  CHECK(fbs_attr_value(a) == 50.0);

  big = add_mod(a, 0u, FBS_ATTR_ADD, 100.0);
  CHECK(fbs_attr_value(a) == 100.0); /* clamped after all channels */
  CHECK(fbs_attr_remove(a, big) == FBS_ATTR_OK);
  CHECK(fbs_attr_value(a) == 50.0); /* removal brings it back inside */

  CHECK(fbs_attr_set_base(a, 500.0) == FBS_ATTR_OK);
  CHECK(fbs_attr_value(a) == 100.0);
  CHECK(fbs_attr_base(a) == 500.0); /* the base itself is not clamped */
  CHECK(fbs_attr_set_base(a, -500.0) == FBS_ATTR_OK);
  CHECK(fbs_attr_value(a) == 0.0);

  CHECK(fbs_attr_clear_clamp(a) == FBS_ATTR_OK);
  CHECK(fbs_attr_has_clamp(a, &lo, &hi) == 0);
  CHECK(fbs_attr_value(a) == -500.0);

  /* the clamp applies after the channels, never between them */
  CHECK(fbs_attr_set_base(a, 10.0) == FBS_ATTR_OK);
  CHECK(fbs_attr_set_clamp(a, 0.0, 20.0) == FBS_ATTR_OK);
  (void)add_mod(a, 0u, FBS_ATTR_MUL_ADD, 9.0); /* 100 before the clamp */
  (void)add_mod(a, 1u, FBS_ATTR_MUL_ADD, -0.5); /* 50 before the clamp */
  CHECK(fbs_attr_value(a) == 20.0);

  /* infinite bounds are legal; NaN and lo > hi are not */
  CHECK(fbs_attr_set_clamp(a, -make_inf(), 30.0) == FBS_ATTR_OK);
  CHECK(fbs_attr_value(a) == 30.0);
  CHECK(fbs_attr_set_clamp(a, -make_inf(), make_inf()) == FBS_ATTR_OK);
  CHECK(fbs_attr_value(a) == 50.0);
  CHECK(fbs_attr_set_clamp(a, 5.0, 4.0) == FBS_ATTR_E_INVALID);
  CHECK(fbs_attr_set_clamp(a, make_nan(), 4.0) == FBS_ATTR_E_INVALID);
  CHECK(fbs_attr_set_clamp(a, 1.0, make_nan()) == FBS_ATTR_E_INVALID);
  CHECK(fbs_attr_value(a) == 50.0); /* rejected calls change nothing */
  CHECK(fbs_attr_set_clamp(a, 7.0, 7.0) == FBS_ATTR_OK); /* lo == hi is legal */
  CHECK(fbs_attr_value(a) == 7.0);

  /* Documented interaction for I32: the clamp is applied BEFORE the rounding,
   * so a clamp window with non-integer bounds can produce a final value outside
   * it. Callers of an I32 attribute are expected to use integer bounds; this
   * pins the behaviour so it cannot drift silently. */
  {
    fbs_attr *i = make_attr(FBS_ATTR_I32, 10.0, 4u);
    int32_t out = 0;
    CHECK(fbs_attr_set_clamp(i, 0.5, 0.7) == FBS_ATTR_OK);
    CHECK(fbs_attr_value(i) == 1.0); /* clamped to 0.7, then rounded up to 1 */
    CHECK(fbs_attr_value_i32(i, &out) == FBS_ATTR_OK);
    CHECK(out == 1);
    CHECK(fbs_attr_set_clamp(i, 0.0, 1.0) == FBS_ATTR_OK); /* integer bounds behave */
    CHECK(fbs_attr_value(i) == 1.0);
    CHECK(fbs_attr_set_base(i, -10.0) == FBS_ATTR_OK);
    CHECK(fbs_attr_value(i) == 0.0);
    fbs_attr_destroy(i);
  }

  /* the clamp survives a round trip */
  {
    fbs_attr *b = NULL;
    size_t len = 0;
    unsigned char *blob = serialize_alloc(a, &len);
    CHECK(fbs_attr_deserialize(blob, len, NULL, NULL, &b) == FBS_ATTR_OK);
    CHECK(fbs_attr_has_clamp(b, &lo, &hi) == 1);
    CHECK(lo == 7.0 && hi == 7.0);
    CHECK(fbs_attr_value(b) == 7.0);
    free(blob);
    fbs_attr_destroy(b);
  }
  fbs_attr_destroy(a);
}

/* ------------------------------------------------------------------------- */
/* A-T10 — zero-valued modifiers are stored, never silently dropped          */
/* ------------------------------------------------------------------------- */

static void test_at10_no_silent_drops(void) {
  fbs_attr *a = make_attr(FBS_ATTR_F64, 4.0, 8u);
  fbs_attr_handle h;
  fbs_attr_modifier m;
  uint64_t before = bits_of(fbs_attr_value(a));
  uint32_t rev = fbs_attr_revision(a);

  h = add_mod(a, 0u, FBS_ATTR_ADD, 0.0);
  CHECK(h != FBS_ATTR_HANDLE_NONE);
  CHECK(fbs_attr_count(a) == 1u);
  CHECK(bits_of(fbs_attr_value(a)) == before); /* the value did not move */
  CHECK(fbs_attr_revision(a) == rev + 1u);     /* but the state did */
  CHECK(fbs_attr_get(a, h, &m) == FBS_ATTR_OK);
  CHECK(m.value == 0.0 && m.channel == 0u && m.op == FBS_ATTR_ADD && m.handle == h);
  CHECK(fbs_attr_remove(a, h) == FBS_ATTR_OK);
  CHECK(fbs_attr_count(a) == 0u);

  /* the same for a tiny but non-zero value, and for every op */
  h = add_mod(a, 0u, FBS_ATTR_ADD, 1.0e-9);
  CHECK(fbs_attr_count(a) == 1u);
  CHECK(fbs_attr_get(a, h, &m) == FBS_ATTR_OK);
  CHECK(m.value == 1.0e-9);
  (void)add_mod(a, 0u, FBS_ATTR_MUL_ADD, 0.0);
  (void)add_mod(a, 0u, FBS_ATTR_MUL_COMPOUND, 0.0);
  (void)add_mod(a, 0u, FBS_ATTR_ADD_FINAL, 0.0);
  CHECK(fbs_attr_count(a) == 4u);
  fbs_attr_destroy(a);
}

/* ------------------------------------------------------------------------- */
/* A-T11 — notification contract and the reentrancy guard                    */
/* ------------------------------------------------------------------------- */

typedef struct {
  fbs_attr *self;
  int calls;
  double prev;
  double at_call;
  int change;
  fbs_attr_handle handle;
  uint32_t revision_at_call;
  int try_reenter;
  int reenter_calls;
  int reenter_all_reentrant;
} observer_ctx;

static void observer_fn(void *user, const fbs_attr *a, double previous_value, int change,
                        fbs_attr_handle handle) {
  observer_ctx *ctx = (observer_ctx *)user;
  ctx->calls += 1;
  ctx->prev = previous_value;
  ctx->at_call = fbs_attr_value(a);
  ctx->change = change;
  ctx->handle = handle;
  ctx->revision_at_call = fbs_attr_revision(a);

  if (ctx->try_reenter) {
    fbs_attr *self = ctx->self;
    fbs_attr_handle h = FBS_ATTR_HANDLE_NONE;
    int all = 1;
    ctx->try_reenter = 0; /* one attempt per outer call */
    all &= (fbs_attr_set_base(self, 999.0) == FBS_ATTR_E_REENTRANT);
    all &= (fbs_attr_add(self, 3u, FBS_ATTR_ADD, 1.0, &h) == FBS_ATTR_E_REENTRANT);
    all &= (fbs_attr_remove(self, 1u) == FBS_ATTR_E_REENTRANT);
    all &= (fbs_attr_clear_channel(self, 0u) == FBS_ATTR_E_REENTRANT);
    all &= (fbs_attr_clear(self) == FBS_ATTR_E_REENTRANT);
    all &= (fbs_attr_set_clamp(self, 0.0, 1.0) == FBS_ATTR_E_REENTRANT);
    all &= (fbs_attr_clear_clamp(self) == FBS_ATTR_E_REENTRANT);
    all &= (fbs_attr_set_observer(self, NULL, NULL) == FBS_ATTR_E_REENTRANT);
    all &= (h == FBS_ATTR_HANDLE_NONE);
    /* read-only calls stay legal inside the observer */
    all &= (fbs_attr_count(self) == fbs_attr_count(a));
    ctx->reenter_calls = 8;
    ctx->reenter_all_reentrant = all;
    ctx->try_reenter = 0;
  }
}

static void test_at11_notifications(void) {
  fbs_attr *a = make_attr(FBS_ATTR_F64, 10.0, 8u);
  observer_ctx ctx;
  fbs_attr_handle h;
  uint32_t rev;

  memset(&ctx, 0, sizeof ctx);
  ctx.self = a;
  CHECK(fbs_attr_set_observer(a, observer_fn, &ctx) == FBS_ATTR_OK);
  CHECK(ctx.calls == 0); /* installing an observer is not a change */
  CHECK(fbs_attr_revision(a) == 0u);

  /* one notification per effective mutating call */
  CHECK(fbs_attr_set_base(a, 20.0) == FBS_ATTR_OK);
  CHECK(ctx.calls == 1);
  CHECK(ctx.change == FBS_ATTR_CHANGE_BASE);
  CHECK(ctx.prev == 10.0);
  CHECK(ctx.at_call == 20.0);
  CHECK(ctx.handle == FBS_ATTR_HANDLE_NONE);
  CHECK(ctx.revision_at_call == 1u);

  /* none when nothing changed */
  CHECK(fbs_attr_set_base(a, 20.0) == FBS_ATTR_OK);
  CHECK(ctx.calls == 1);
  CHECK(fbs_attr_revision(a) == 1u);
  CHECK(fbs_attr_clear_channel(a, 4u) == FBS_ATTR_OK); /* empty channel */
  CHECK(ctx.calls == 1);
  CHECK(fbs_attr_clear(a) == FBS_ATTR_OK); /* empty attribute */
  CHECK(ctx.calls == 1);
  CHECK(fbs_attr_clear_clamp(a) == FBS_ATTR_OK); /* no clamp set */
  CHECK(ctx.calls == 1);
  CHECK(fbs_attr_revision(a) == 1u);

  h = add_mod(a, 0u, FBS_ATTR_ADD, 5.0);
  CHECK(ctx.calls == 2);
  CHECK(ctx.change == FBS_ATTR_CHANGE_ADDED);
  CHECK(ctx.handle == h);
  CHECK(ctx.prev == 20.0 && ctx.at_call == 25.0);

  CHECK(fbs_attr_remove(a, h) == FBS_ATTR_OK);
  CHECK(ctx.calls == 3);
  CHECK(ctx.change == FBS_ATTR_CHANGE_REMOVED);
  CHECK(ctx.handle == h);
  CHECK(ctx.prev == 25.0 && ctx.at_call == 20.0);

  CHECK(fbs_attr_remove(a, h) == FBS_ATTR_E_NOT_FOUND);
  CHECK(ctx.calls == 3); /* a failed call never notifies */

  (void)add_mod(a, 6u, FBS_ATTR_ADD, 1.0);
  CHECK(ctx.calls == 4);
  CHECK(fbs_attr_clear_channel(a, 6u) == FBS_ATTR_OK);
  CHECK(ctx.calls == 5);
  CHECK(ctx.change == FBS_ATTR_CHANGE_CHANNEL_CLEARED);
  CHECK(ctx.handle == FBS_ATTR_HANDLE_NONE);

  (void)add_mod(a, 0u, FBS_ATTR_ADD, 1.0);
  (void)add_mod(a, 1u, FBS_ATTR_ADD, 1.0);
  CHECK(fbs_attr_clear(a) == FBS_ATTR_OK);
  CHECK(ctx.change == FBS_ATTR_CHANGE_CLEARED);
  CHECK(fbs_attr_count(a) == 0u);

  CHECK(fbs_attr_set_clamp(a, 0.0, 5.0) == FBS_ATTR_OK);
  CHECK(ctx.change == FBS_ATTR_CHANGE_CLAMP);
  rev = fbs_attr_revision(a);
  CHECK(fbs_attr_set_clamp(a, 0.0, 5.0) == FBS_ATTR_OK); /* same bounds: no-op */
  CHECK(fbs_attr_revision(a) == rev);
  CHECK(fbs_attr_clear_clamp(a) == FBS_ATTR_OK);
  CHECK(fbs_attr_revision(a) == rev + 1u);

  /* reentrancy: every mutating call from inside the observer is refused and
   * the attribute is left exactly as it was */
  {
    double value_before;
    unsigned count_before;
    uint32_t rev_before;
    CHECK(fbs_attr_set_base(a, 3.0) == FBS_ATTR_OK);
    (void)add_mod(a, 0u, FBS_ATTR_ADD, 2.0);
    value_before = fbs_attr_value(a);
    count_before = fbs_attr_count(a);
    ctx.try_reenter = 1;
    ctx.calls = 0;
    CHECK(fbs_attr_set_base(a, 4.0) == FBS_ATTR_OK);
    rev_before = fbs_attr_revision(a);
    CHECK(ctx.calls == 1);
    CHECK(ctx.reenter_calls == 8);
    CHECK(ctx.reenter_all_reentrant == 1);
    CHECK(fbs_attr_count(a) == count_before);
    CHECK(fbs_attr_value(a) == value_before + 1.0); /* only the outer set_base landed */
    CHECK(fbs_attr_has_clamp(a, NULL, NULL) == 0);
    CHECK(fbs_attr_base(a) == 4.0);
    /* the guard is released afterwards */
    CHECK(fbs_attr_set_base(a, 5.0) == FBS_ATTR_OK);
    CHECK(fbs_attr_revision(a) == rev_before + 1u);
  }

  /* the observer can be removed */
  CHECK(fbs_attr_set_observer(a, NULL, NULL) == FBS_ATTR_OK);
  ctx.calls = 0;
  CHECK(fbs_attr_set_base(a, 77.0) == FBS_ATTR_OK);
  CHECK(ctx.calls == 0);
  fbs_attr_destroy(a);
}

/* Destroying the attribute from inside its own observer. The free is deferred
 * until the observer returns, and nothing touches the block afterwards — under
 * ASan this test is the witness that there is no heap-use-after-free, and the
 * counting allocator is the witness that the block is released exactly once. */
typedef struct {
  fbs_attr *self;
  int calls;
} suicide_ctx;

static void suicide_observer(void *user, const fbs_attr *a, double previous_value, int change,
                             fbs_attr_handle handle) {
  suicide_ctx *ctx = (suicide_ctx *)user;
  (void)previous_value;
  (void)change;
  (void)handle;
  ctx->calls += 1;
  /* the attribute is still fully readable at this point */
  (void)fbs_attr_value(a);
  (void)fbs_attr_count(a);
  fbs_attr_destroy(ctx->self);
  /* still readable: the block is only released once this returns */
  (void)fbs_attr_value(a);
  fbs_attr_destroy(ctx->self); /* idempotent while deferred: still one free */
}

static void test_destroy_from_observer(void) {
  counting_alloc counter;
  fbs_attr_allocator alloc;
  fbs_attr_config cfg = fbs_attr_config_default();
  fbs_attr *a = NULL;
  suicide_ctx ctx;
  fbs_attr_handle h = FBS_ATTR_HANDLE_NONE;

  alloc.alloc = ca_alloc;
  alloc.free = ca_free;
  alloc.user = &counter;

  /* via set_base */
  memset(&counter, 0, sizeof counter);
  counter.budget = -1;
  memset(&ctx, 0, sizeof ctx);
  CHECK(fbs_attr_create(&cfg, &alloc, &a) == FBS_ATTR_OK);
  ctx.self = a;
  CHECK(fbs_attr_set_observer(a, suicide_observer, &ctx) == FBS_ATTR_OK);
  CHECK(fbs_attr_set_base(a, 5.0) == FBS_ATTR_OK);
  CHECK(ctx.calls == 1);
  CHECK(counter.allocs == 1 && counter.frees == 1);
  a = NULL; /* deliberately never dereferenced again */

  /* via add, whose caller writes *out_handle after the commit returns */
  memset(&counter, 0, sizeof counter);
  counter.budget = -1;
  memset(&ctx, 0, sizeof ctx);
  CHECK(fbs_attr_create(&cfg, &alloc, &a) == FBS_ATTR_OK);
  ctx.self = a;
  CHECK(fbs_attr_set_observer(a, suicide_observer, &ctx) == FBS_ATTR_OK);
  CHECK(fbs_attr_add(a, 0u, FBS_ATTR_ADD, 1.0, &h) == FBS_ATTR_OK);
  CHECK(h != FBS_ATTR_HANDLE_NONE); /* the out-param survived the deferred free */
  CHECK(ctx.calls == 1);
  CHECK(counter.allocs == 1 && counter.frees == 1);
  a = NULL;

  /* an observer that does not destroy leaves the attribute alive as before */
  memset(&counter, 0, sizeof counter);
  counter.budget = -1;
  CHECK(fbs_attr_create(&cfg, &alloc, &a) == FBS_ATTR_OK);
  CHECK(fbs_attr_set_base(a, 3.0) == FBS_ATTR_OK);
  CHECK(counter.frees == 0);
  fbs_attr_destroy(a);
  CHECK(counter.frees == 1);
}

/* ------------------------------------------------------------------------- */
/* A-T13 — serialization determinism                                         */
/* ------------------------------------------------------------------------- */

/* Zero the persisted handles so two insertion orders can be compared: the
 * modifier set is identical, only the handle numbering differs. */
static void mask_handles(unsigned char *b, size_t len) {
  size_t head = (b[7] & 1u) ? (size_t)44u : (size_t)28u;
  size_t i, n = (len - head) / 24u;
  memset(b + head - 12u, 0, 8u); /* next_handle */
  for (i = 0u; i < n; ++i) memset(b + head + i * 24u, 0, 8u);
}

static fbs_attr *build_at13_attr(int order) {
  fbs_attr *a = make_attr(FBS_ATTR_F64, 3.5, 16u);
  CHECK(fbs_attr_set_clamp(a, -1000.0, 1000.0) == FBS_ATTR_OK);
  if (order == 0) {
    (void)add_mod(a, 0u, FBS_ATTR_ADD, 1.5);
    (void)add_mod(a, 2u, FBS_ATTR_MUL_ADD, 0.25);
    (void)add_mod(a, 0u, FBS_ATTR_ADD_FINAL, -0.5);
    (void)add_mod(a, 1u, FBS_ATTR_OVERRIDE, 12.0);
  } else {
    (void)add_mod(a, 1u, FBS_ATTR_OVERRIDE, 12.0);
    (void)add_mod(a, 0u, FBS_ATTR_ADD_FINAL, -0.5);
    (void)add_mod(a, 2u, FBS_ATTR_MUL_ADD, 0.25);
    (void)add_mod(a, 0u, FBS_ATTR_ADD, 1.5);
  }
  return a;
}

/* An attribute with no modifiers and no clamp is a 28-byte header, and it
 * round trips for every type. */
static void test_empty_round_trip(void) {
  static const int types[3] = {FBS_ATTR_F32, FBS_ATTR_F64, FBS_ATTR_I32};
  unsigned t;
  for (t = 0u; t < 3u; ++t) {
    fbs_attr *a = make_attr(types[t], -4.5, 4u);
    fbs_attr *b = NULL;
    unsigned char buf[64], again[64];
    size_t len = 0, l2 = 0;
    CHECK(fbs_attr_serialized_size(a) == 28u);
    CHECK(fbs_attr_serialize(a, buf, sizeof buf, &len) == FBS_ATTR_OK);
    CHECK(len == 28u);
    CHECK(fbs_attr_deserialize(buf, len, NULL, NULL, &b) == FBS_ATTR_OK);
    CHECK(fbs_attr_type_of(b) == types[t]);
    CHECK(fbs_attr_base(b) == -4.5);
    CHECK(bits_of(fbs_attr_value(b)) == bits_of(fbs_attr_value(a)));
    CHECK(fbs_attr_count(b) == 0u);
    CHECK(fbs_attr_has_clamp(b, NULL, NULL) == 0);
    CHECK(fbs_attr_revision(b) == 0u);
    CHECK(fbs_attr_serialize(b, again, sizeof again, &l2) == FBS_ATTR_OK);
    CHECK(l2 == len && memcmp(again, buf, len) == 0);
    fbs_attr_destroy(b);
    fbs_attr_destroy(a);
  }
}

static void test_at13_determinism(void) {
  fbs_attr *a0 = build_at13_attr(0);
  fbs_attr *a1 = build_at13_attr(1);
  fbs_attr *round = NULL;
  size_t l0 = 0, l1 = 0, l2 = 0, l3 = 0;
  unsigned char *b0, *b1, *b2, *b3;

  CHECK(fbs_attr_value(a0) == fbs_attr_value(a1));

  b0 = serialize_alloc(a0, &l0);
  b1 = serialize_alloc(a1, &l1);
  CHECK(l0 == l1);
  /* the emitted order is (channel, op, handle), so only the handles differ */
  mask_handles(b0, l0);
  mask_handles(b1, l1);
  CHECK(memcmp(b0, b1, l0) == 0);
  free(b0);
  free(b1);

  /* ser is byte-stable and deser(ser(x)) is a fixed point */
  b0 = serialize_alloc(a0, &l0);
  b2 = serialize_alloc(a0, &l2);
  CHECK(l0 == l2 && memcmp(b0, b2, l0) == 0);
  CHECK(fbs_attr_deserialize(b0, l0, NULL, NULL, &round) == FBS_ATTR_OK);
  b3 = serialize_alloc(round, &l3);
  CHECK(l3 == l0 && memcmp(b3, b0, l0) == 0);
  CHECK(fbs_attr_value(round) == fbs_attr_value(a0));

  /* the sorted listing matches the emitted order */
  {
    fbs_attr_modifier list[8];
    size_t n = 0;
    unsigned i;
    CHECK(fbs_attr_list(a0, list, 8u, &n) == FBS_ATTR_OK);
    CHECK(n == 4u);
    for (i = 1u; i < (unsigned)n; ++i) {
      int ordered = list[i].channel > list[i - 1u].channel ||
                    (list[i].channel == list[i - 1u].channel && list[i].op > list[i - 1u].op) ||
                    (list[i].channel == list[i - 1u].channel && list[i].op == list[i - 1u].op &&
                     list[i].handle > list[i - 1u].handle);
      CHECK(ordered);
    }
  }

  free(b0);
  free(b2);
  free(b3);
  fbs_attr_destroy(round);
  fbs_attr_destroy(a1);
  fbs_attr_destroy(a0);
}

/* ------------------------------------------------------------------------- */
/* A-T14 — schema rejection                                                  */
/* ------------------------------------------------------------------------- */

static void test_at14_schema_rejection(void) {
  fbs_attr *a = make_attr(FBS_ATTR_F64, 2.5, 8u);
  fbs_attr *loaded = NULL;
  unsigned char *blob, *copy;
  size_t len = 0, head;
  fbs_attr_config cfg;

  (void)add_mod(a, 0u, FBS_ATTR_ADD, 1.0);
  (void)add_mod(a, 1u, FBS_ATTR_MUL_ADD, 0.5);
  (void)add_mod(a, 1u, FBS_ATTR_OVERRIDE, 3.0);
  CHECK(fbs_attr_set_clamp(a, -10.0, 10.0) == FBS_ATTR_OK);
  blob = serialize_alloc(a, &len);
  head = 44u; /* 16 + 16 clamp + 8 next_handle + 4 count */
  CHECK(len == head + 3u * 24u);
  copy = (unsigned char *)malloc(len);
  CHECK(copy != NULL);

  CHECK(fbs_attr_deserialize(blob, len, NULL, NULL, &loaded) == FBS_ATTR_OK);
  fbs_attr_destroy(loaded);
  loaded = NULL;

  memcpy(copy, blob, len);
  copy[0] = 'Z';
  expect_schema_reject(copy, len, "magic");

  memcpy(copy, blob, len);
  copy[4] = 9u;
  expect_schema_reject(copy, len, "version");

  memcpy(copy, blob, len);
  copy[6] = 7u;
  expect_schema_reject(copy, len, "type");

  memcpy(copy, blob, len);
  copy[7] = 3u; /* undefined flag bit */
  expect_schema_reject(copy, len, "flags");

  memcpy(copy, blob, len);
  copy[8 + 7] = 0x7fu; /* base becomes NaN/inf */
  copy[8 + 6] = 0xf8u;
  expect_schema_reject(copy, len, "non-finite base");

  memcpy(copy, blob, len);
  copy[16 + 7] = 0x7fu; /* clamp_lo becomes NaN */
  copy[16 + 6] = 0xf8u;
  expect_schema_reject(copy, len, "NaN clamp");

  memcpy(copy, blob, len);
  memcpy(copy + 16, blob + 24, 8u); /* lo := hi */
  memcpy(copy + 24, blob + 16, 8u); /* hi := lo, so lo > hi */
  expect_schema_reject(copy, len, "clamp lo > hi");

  memcpy(copy, blob, len);
  memset(copy + 32, 0, 8u); /* next_handle = 0 */
  expect_schema_reject(copy, len, "next_handle zero");

  memcpy(copy, blob, len);
  copy[40] = 200u; /* mod_count overruns the buffer */
  expect_schema_reject(copy, len, "mod count overrun");

  memcpy(copy, blob, len);
  expect_schema_reject(copy, len - 1u, "truncated buffer");
  expect_schema_reject(copy, 8u, "short header");
  expect_schema_reject(copy, 40u, "header cut before mod_count");

  /* The length must match exactly: one trailing byte is not tolerated. */
  {
    unsigned char *longer = (unsigned char *)malloc(len + 1u);
    CHECK(longer != NULL);
    memcpy(longer, blob, len);
    longer[len] = 0u;
    expect_schema_reject(longer, len + 1u, "trailing byte (zero)");
    longer[len] = 0xffu;
    expect_schema_reject(longer, len + 1u, "trailing byte (non-zero)");
    free(longer);
  }

  memcpy(copy, blob, len);
  copy[head + 10] = 9u; /* bad op */
  expect_schema_reject(copy, len, "modifier op enum");

  memcpy(copy, blob, len);
  copy[head + 11] = 1u; /* pad must be zero */
  expect_schema_reject(copy, len, "modifier pad");

  memcpy(copy, blob, len);
  copy[head + 12] = 1u; /* reserved must be zero */
  expect_schema_reject(copy, len, "modifier reserved");

  memcpy(copy, blob, len);
  copy[head + 16 + 7] = 0x7fu; /* value becomes non-finite */
  copy[head + 16 + 6] = 0xf0u;
  expect_schema_reject(copy, len, "non-finite modifier value");

  memcpy(copy, blob, len);
  memset(copy + head, 0, 8u); /* handle 0 */
  expect_schema_reject(copy, len, "modifier handle zero");

  memcpy(copy, blob, len);
  copy[head] = 250u; /* handle >= next_handle */
  expect_schema_reject(copy, len, "handle beyond next_handle");

  /* swap two records so the file is no longer sorted */
  memcpy(copy, blob, len);
  {
    unsigned char tmp[24];
    memcpy(tmp, copy + head, 24u);
    memcpy(copy + head, copy + head + 24u, 24u);
    memcpy(copy + head + 24u, tmp, 24u);
  }
  expect_schema_reject(copy, len, "unsorted modifiers");

  /* duplicate (channel, op, handle) */
  memcpy(copy, blob, len);
  memcpy(copy + head + 24u, copy + head, 24u);
  expect_schema_reject(copy, len, "duplicate modifier record");

  /* the same handle under two different channels: sorted, but not unique */
  memcpy(copy, blob, len);
  memcpy(copy + head + 24u, copy + head, 8u); /* record 1 takes record 0's handle */
  expect_schema_reject(copy, len, "duplicate handle across channels");

  /* cfg interaction */
  cfg = fbs_attr_config_default();
  cfg.type = FBS_ATTR_I32;
  loaded = (fbs_attr *)0x1;
  CHECK(fbs_attr_deserialize(blob, len, &cfg, NULL, &loaded) == FBS_ATTR_E_SCHEMA);
  CHECK(loaded == (fbs_attr *)0x1);
  cfg = fbs_attr_config_default();
  cfg.max_modifiers = 2u;
  CHECK(fbs_attr_deserialize(blob, len, &cfg, NULL, &loaded) == FBS_ATTR_E_FULL);
  CHECK(loaded == (fbs_attr *)0x1);
  cfg = fbs_attr_config_default();
  cfg.max_modifiers = 0u;
  CHECK(fbs_attr_deserialize(blob, len, &cfg, NULL, &loaded) == FBS_ATTR_E_INVALID);
  cfg = fbs_attr_config_default();
  cfg.max_modifiers = 3u;
  cfg.base = 999.0; /* ignored: the blob owns the base */
  loaded = NULL;
  CHECK(fbs_attr_deserialize(blob, len, &cfg, NULL, &loaded) == FBS_ATTR_OK);
  CHECK(fbs_attr_base(loaded) == 2.5);
  CHECK(fbs_attr_count(loaded) == 3u);
  {
    fbs_attr_handle overflow = FBS_ATTR_HANDLE_NONE;
    CHECK(fbs_attr_add(loaded, 0u, FBS_ATTR_ADD, 1.0, &overflow) == FBS_ATTR_E_FULL);
    CHECK(overflow == FBS_ATTR_HANDLE_NONE);
  }
  fbs_attr_destroy(loaded);

  free(copy);
  free(blob);
  fbs_attr_destroy(a);
}

/* ------------------------------------------------------------------------- */
/* A-T15 — randomized property test against an independent reference          */
/* ------------------------------------------------------------------------- */

#define REF_CAP 64

typedef struct {
  fbs_attr_handle handle;
  unsigned channel;
  int op;
  double value;
} ref_mod;

typedef struct {
  int type;
  double base;
  int has_clamp;
  double lo, hi;
  ref_mod mods[REF_CAP]; /* insertion order, i.e. ascending handle */
  unsigned count;
} ref_attr;

/* Accumulate one (channel, op) bucket in ascending (value, handle) order.
 * Deliberately a different algorithm from the module's heapsort over a scratch
 * index array: this repeatedly selects the smallest element strictly after the
 * previous one in the (value, handle) order. */
static void ref_accumulate(const ref_attr *r, unsigned channel, int op, double *sum,
                           double *product) {
  double last_value = 0.0;
  fbs_attr_handle last_handle = 0u;
  int have_last = 0;

  for (;;) {
    unsigned best = 0u;
    int found = 0;
    unsigned i;
    for (i = 0u; i < r->count; ++i) {
      if (r->mods[i].channel != channel || r->mods[i].op != op) continue;
      if (have_last && (r->mods[i].value < last_value ||
                        (r->mods[i].value == last_value && r->mods[i].handle <= last_handle)))
        continue; /* already consumed */
      if (!found || r->mods[i].value < r->mods[best].value ||
          (r->mods[i].value == r->mods[best].value && r->mods[i].handle < r->mods[best].handle)) {
        best = i;
        found = 1;
      }
    }
    if (!found) return;
    if (sum) *sum += r->mods[best].value;
    if (product) *product *= (1.0 + r->mods[best].value);
    last_value = r->mods[best].value;
    last_handle = r->mods[best].handle;
    have_last = 1;
  }
}

/* Independent evaluator: channels picked out one at a time in ascending order,
 * then one selection pass per op bucket. Different data structure and traversal
 * from src/attributes.c, same documented contract. */
static double ref_eval(const ref_attr *r) {
  double v = r->base;
  unsigned last_channel = 0u;
  int first_channel = 1;

  for (;;) {
    unsigned ch = 0u;
    int found = 0;
    unsigned i;
    double sum_add = 0.0, sum_mul_add = 0.0, prod_compound = 1.0, sum_add_final = 0.0;
    int has_override = 0;
    double override_value = 0.0;
    fbs_attr_handle override_handle = 0u;

    for (i = 0u; i < r->count; ++i) {
      unsigned c = r->mods[i].channel;
      if (!first_channel && c <= last_channel) continue;
      if (!found || c < ch) {
        ch = c;
        found = 1;
      }
    }
    if (!found) break;

    for (i = 0u; i < r->count; ++i) {
      if (r->mods[i].channel != ch) continue;
      if (r->mods[i].op != FBS_ATTR_OVERRIDE) continue;
      if (!has_override || r->mods[i].handle < override_handle) {
        has_override = 1;
        override_value = r->mods[i].value;
        override_handle = r->mods[i].handle;
      }
    }
    if (has_override) {
      v = override_value;
    } else {
      ref_accumulate(r, ch, FBS_ATTR_ADD, &sum_add, NULL);
      ref_accumulate(r, ch, FBS_ATTR_MUL_ADD, &sum_mul_add, NULL);
      ref_accumulate(r, ch, FBS_ATTR_MUL_COMPOUND, NULL, &prod_compound);
      ref_accumulate(r, ch, FBS_ATTR_ADD_FINAL, &sum_add_final, NULL);
      v = (v + sum_add) * (1.0 + sum_mul_add);
      v = v * prod_compound;
      v = v + sum_add_final;
    }

    last_channel = ch;
    first_channel = 0;
  }

  if (r->has_clamp) {
    if (v < r->lo) v = r->lo;
    if (v > r->hi) v = r->hi;
  }
  if (r->type == FBS_ATTR_F32) {
    v = (double)(float)v;
  } else if (r->type == FBS_ATTR_I32) {
    /* round half away from zero, written without libm. Every double at or
     * above 2^52 is already an integer, and that guard also passes NaN and
     * infinities through untouched. */
    double magnitude = (v < 0.0) ? -v : v;
    if (magnitude < 4503599627370496.0) {
      double truncated = (double)(long long)magnitude;
      if (magnitude - truncated >= 0.5) truncated += 1.0;
      v = (v < 0.0) ? -truncated : truncated;
    }
  }
  return v;
}

static uint64_t ref_signature(const ref_attr *r) {
  const uint64_t fnv_prime = (uint64_t)1099511628211ULL;
  uint64_t h = (uint64_t)1469598103934665603ULL;
  unsigned i;
  const unsigned char *p;
  size_t k;
  p = (const unsigned char *)&r->base;
  for (k = 0; k < sizeof r->base; ++k) h = (h ^ p[k]) * fnv_prime;
  h = (h ^ (uint64_t)r->has_clamp) * fnv_prime;
  if (r->has_clamp) {
    p = (const unsigned char *)&r->lo;
    for (k = 0; k < sizeof r->lo; ++k) h = (h ^ p[k]) * fnv_prime;
    p = (const unsigned char *)&r->hi;
    for (k = 0; k < sizeof r->hi; ++k) h = (h ^ p[k]) * fnv_prime;
  }
  h = (h ^ (uint64_t)r->count) * fnv_prime;
  for (i = 0u; i < r->count; ++i) {
    h = (h ^ r->mods[i].handle) * fnv_prime;
    h = (h ^ (uint64_t)r->mods[i].channel) * fnv_prime;
    h = (h ^ (uint64_t)r->mods[i].op) * fnv_prime;
    p = (const unsigned char *)&r->mods[i].value;
    for (k = 0; k < sizeof r->mods[i].value; ++k) h = (h ^ p[k]) * fnv_prime;
  }
  return h;
}

static void ref_remove_at(ref_attr *r, unsigned idx) {
  unsigned i;
  for (i = idx; i + 1u < r->count; ++i) r->mods[i] = r->mods[i + 1u];
  r->count -= 1u;
}

/* Magnitudes are deliberately bounded so that no reachable state overflows the
 * float range: at most 64 modifiers, so the compound product is under 1.5^64
 * and the multiplier product under 65^5, which keeps every value well inside
 * FLT_MAX even for the F32 type. The tables mix dyadic and non-dyadic values,
 * and the small alphabet makes equal values inside one bucket common, so the
 * (value, handle) accumulation order and its tie-break are both exercised;
 * agreement with the reference evaluator is required bit for bit. */
static double fuzz_value(int op) {
  static const double add_values[14] = {0.0,  0.5,  -0.5, 0.25, -0.25, 1.0,      -1.0,
                                        2.0,  -2.0, 8.0,  -8.0, 0.1,   -0.3,     1.0 / 3.0};
  static const double mul_values[9] = {-0.5, -0.25, 0.0, 0.25, 0.5, 1.0, 0.1, -0.7, 1.0e-3};
  static const double compound_values[8] = {-0.5, -0.25, 0.0, 0.25, 0.5, 0.1, -0.1, 1.0 / 7.0};
  if (op == FBS_ATTR_MUL_COMPOUND) return compound_values[rng_below(8u)];
  if (op == FBS_ATTR_MUL_ADD) return mul_values[rng_below(9u)];
  return add_values[rng_below(14u)];
}

static void run_fuzz_round(uint32_t seed, int type) {
  fbs_attr_config cfg = fbs_attr_config_default();
  fbs_attr *a = NULL;
  fbs_attr *loaded = NULL;
  ref_attr ref;
  unsigned step;
  int value_mismatch = 0;
  int revision_mismatch = 0;
  unsigned char *blob;
  size_t len = 0;

  cfg.type = type;
  cfg.base = 4.0;
  cfg.max_modifiers = REF_CAP;
  CHECK(fbs_attr_create(&cfg, NULL, &a) == FBS_ATTR_OK);

  memset(&ref, 0, sizeof ref);
  ref.type = type;
  ref.base = 4.0;
  rng_seed(seed);

  for (step = 0u; step < 200u; ++step) {
    uint64_t sig_before = ref_signature(&ref);
    uint32_t rev_before = fbs_attr_revision(a);
    unsigned op = rng_below(7u);
    uint64_t sig_after;

    if (op == 0u || (op == 1u && ref.count == 0u)) { /* add */
      /* A deliberately small channel alphabet: with up to 64 modifiers in
       * 3 channels x 5 ops the buckets are large enough that order-sensitive
       * sums arise constantly, which is what makes this round a real check on
       * the (value, handle) accumulation order. */
      static const unsigned channels[3] = {0u, 1u, 65535u};
      unsigned channel = channels[rng_below(3u)];
      int mop = (int)rng_below(5u);
      double value = fuzz_value(mop);
      fbs_attr_handle h = FBS_ATTR_HANDLE_NONE;
      fbs_attr_status st = fbs_attr_add(a, (uint16_t)channel, mop, value, &h);
      if (ref.count < REF_CAP) {
        CHECK(st == FBS_ATTR_OK);
        ref.mods[ref.count].handle = h;
        ref.mods[ref.count].channel = channel;
        ref.mods[ref.count].op = mop;
        ref.mods[ref.count].value = value;
        ref.count += 1u;
      } else {
        CHECK(st == FBS_ATTR_E_FULL);
      }
    } else if (op == 1u) { /* remove an existing handle */
      unsigned idx = rng_below(ref.count);
      fbs_attr_handle h = ref.mods[idx].handle;
      CHECK(fbs_attr_remove(a, h) == FBS_ATTR_OK);
      ref_remove_at(&ref, idx);
    } else if (op == 2u) { /* remove an unknown handle */
      CHECK(fbs_attr_remove(a, (fbs_attr_handle)0xffffffffu) == FBS_ATTR_E_NOT_FOUND);
    } else if (op == 3u) { /* set_base */
      double base = fuzz_value(FBS_ATTR_ADD) * 3.0;
      CHECK(fbs_attr_set_base(a, base) == FBS_ATTR_OK);
      ref.base = base;
    } else if (op == 4u) { /* clear_channel */
      static const unsigned channels[3] = {0u, 1u, 65535u};
      unsigned channel = channels[rng_below(3u)];
      unsigned i = 0u;
      CHECK(fbs_attr_clear_channel(a, (uint16_t)channel) == FBS_ATTR_OK);
      while (i < ref.count) {
        if (ref.mods[i].channel == channel)
          ref_remove_at(&ref, i);
        else
          ++i;
      }
    } else if (op == 5u) { /* clamp on/off */
      if (rng_below(2u) == 0u) {
        double lo = fuzz_value(FBS_ATTR_ADD) * 4.0;
        double hi = lo + (double)rng_below(20u);
        CHECK(fbs_attr_set_clamp(a, lo, hi) == FBS_ATTR_OK);
        ref.has_clamp = 1;
        ref.lo = lo;
        ref.hi = hi;
      } else {
        CHECK(fbs_attr_clear_clamp(a) == FBS_ATTR_OK);
        ref.has_clamp = 0;
      }
    } else { /* clear */
      CHECK(fbs_attr_clear(a) == FBS_ATTR_OK);
      ref.count = 0u;
    }

    if (bits_of(fbs_attr_value(a)) != bits_of(ref_eval(&ref))) value_mismatch = 1;
    if (fbs_attr_count(a) != ref.count) value_mismatch = 1;

    sig_after = ref_signature(&ref);
    if (sig_after == sig_before) {
      if (fbs_attr_revision(a) != rev_before) revision_mismatch = 1;
    } else {
      if (fbs_attr_revision(a) != rev_before + 1u) revision_mismatch = 1;
    }
  }

  ++g_checks;
  if (value_mismatch) {
    ++g_fails;
    printf("FAIL A-T15 (seed %lu, type %d): value disagrees with the reference evaluator\n",
           (unsigned long)seed, type);
  }
  ++g_checks;
  if (revision_mismatch) {
    ++g_fails;
    printf("FAIL A-T15 (seed %lu, type %d): revision did not track effective changes\n",
           (unsigned long)seed, type);
  }

  /* ser/deser is a fixed point */
  blob = serialize_alloc(a, &len);
  CHECK(fbs_attr_deserialize(blob, len, NULL, NULL, &loaded) == FBS_ATTR_OK);
  CHECK(bits_of(fbs_attr_value(loaded)) == bits_of(fbs_attr_value(a)));
  CHECK(fbs_attr_count(loaded) == fbs_attr_count(a));
  {
    size_t l2 = 0;
    unsigned char *b2 = serialize_alloc(loaded, &l2);
    CHECK(l2 == len && memcmp(b2, blob, len) == 0);
    free(b2);
  }
  free(blob);
  fbs_attr_destroy(loaded);
  fbs_attr_destroy(a);
}

static void test_at15_fuzz(void) {
  unsigned i;
  for (i = 0u; i < 12u; ++i) {
    run_fuzz_round(0x1234567u + i * 2654435761u, FBS_ATTR_F64);
    run_fuzz_round(0x89abcdefu + i * 40503u, FBS_ATTR_F32);
    run_fuzz_round(0xfeedbeefu + i * 2246822519u, FBS_ATTR_I32);
  }
}

/* ------------------------------------------------------------------------- */
/* Capacity, allocator, truncation                                           */
/* ------------------------------------------------------------------------- */

static void test_capacity(void) {
  fbs_attr *a = make_attr(FBS_ATTR_F64, 0.0, 3u);
  fbs_attr_handle h = FBS_ATTR_HANDLE_NONE, sentinel = 0xdeadu;
  unsigned i;

  for (i = 0u; i < 3u; ++i) (void)add_mod(a, (unsigned)i, FBS_ATTR_ADD, 1.0);
  CHECK(fbs_attr_count(a) == 3u);
  CHECK(fbs_attr_add(a, 0u, FBS_ATTR_ADD, 1.0, &sentinel) == FBS_ATTR_E_FULL);
  CHECK(sentinel == 0xdeadu); /* untouched */
  CHECK(fbs_attr_count(a) == 3u);
  CHECK(fbs_attr_value(a) == 3.0);

  CHECK(fbs_attr_clear(a) == FBS_ATTR_OK);
  CHECK(fbs_attr_add(a, 0u, FBS_ATTR_ADD, 1.0, &h) == FBS_ATTR_OK);
  CHECK(h == 4u); /* handles are never reused after a clear */
  fbs_attr_destroy(a);
}

static void test_allocator(void) {
  counting_alloc ctx;
  fbs_attr_allocator alloc;
  fbs_attr *a = (fbs_attr *)0x1;
  fbs_attr_config cfg = fbs_attr_config_default();
  unsigned char *blob;
  size_t len = 0;

  alloc.alloc = ca_alloc;
  alloc.free = ca_free;
  alloc.user = &ctx;

  memset(&ctx, 0, sizeof ctx);
  ctx.budget = 0;
  CHECK(fbs_attr_create(&cfg, &alloc, &a) == FBS_ATTR_E_MEMORY);
  CHECK(a == (fbs_attr *)0x1);
  CHECK(ctx.allocs == 0 && ctx.frees == 0);

  memset(&ctx, 0, sizeof ctx);
  ctx.budget = -1;
  a = NULL;
  CHECK(fbs_attr_create(&cfg, &alloc, &a) == FBS_ATTR_OK);
  CHECK(ctx.allocs == 1 && ctx.frees == 0);
  CHECK(fbs_attr_memory(a) == ctx.bytes);
  CHECK(fbs_attr_memory(NULL) == 0u);
  (void)add_mod(a, 0u, FBS_ATTR_ADD, 1.0);
  (void)add_mod(a, 1u, FBS_ATTR_MUL_ADD, 1.0);
  CHECK(fbs_attr_set_clamp(a, -5.0, 5.0) == FBS_ATTR_OK);
  CHECK(ctx.allocs == 1); /* nothing is allocated after create */
  blob = serialize_alloc(a, &len);
  fbs_attr_destroy(a);
  CHECK(ctx.allocs == 1 && ctx.frees == 1);

  memset(&ctx, 0, sizeof ctx);
  ctx.budget = 0;
  a = (fbs_attr *)0x1;
  CHECK(fbs_attr_deserialize(blob, len, NULL, &alloc, &a) == FBS_ATTR_E_MEMORY);
  CHECK(a == (fbs_attr *)0x1);

  memset(&ctx, 0, sizeof ctx);
  ctx.budget = -1;
  a = NULL;
  CHECK(fbs_attr_deserialize(blob, len, NULL, &alloc, &a) == FBS_ATTR_OK);
  CHECK(ctx.allocs == 1);
  fbs_attr_destroy(a);
  CHECK(ctx.frees == 1);

  alloc.alloc = NULL;
  a = (fbs_attr *)0x1;
  CHECK(fbs_attr_create(&cfg, &alloc, &a) == FBS_ATTR_E_INVALID);
  CHECK(a == (fbs_attr *)0x1);
  alloc.alloc = ca_alloc;
  alloc.free = NULL;
  CHECK(fbs_attr_create(&cfg, &alloc, &a) == FBS_ATTR_E_INVALID);

  free(blob);
}

static void test_truncated(void) {
  fbs_attr *a = make_attr(FBS_ATTR_F64, 1.0, 8u);
  fbs_attr_modifier list[4];
  unsigned char buf[256];
  size_t n = 0, need = 0;

  (void)add_mod(a, 0u, FBS_ATTR_ADD, 1.0);
  (void)add_mod(a, 1u, FBS_ATTR_ADD, 2.0);
  (void)add_mod(a, 2u, FBS_ATTR_ADD, 3.0);

  memset(list, 0x5a, sizeof list);
  n = 999u;
  CHECK(fbs_attr_list(a, list, 2u, &n) == FBS_ATTR_E_TRUNCATED);
  CHECK(n == 3u);
  CHECK(list[0].handle == 0x5a5a5a5a5a5a5a5aULL); /* nothing written */
  n = 999u;
  CHECK(fbs_attr_list(a, NULL, 0u, &n) == FBS_ATTR_E_TRUNCATED);
  CHECK(n == 3u);
  CHECK(fbs_attr_list(a, list, 4u, &n) == FBS_ATTR_OK);
  CHECK(n == 3u);
  CHECK(list[0].value == 1.0 && list[2].value == 3.0);

  need = fbs_attr_serialized_size(a);
  CHECK(need == 28u + 3u * 24u);
  memset(buf, 0x5a, sizeof buf);
  n = 999u;
  CHECK(fbs_attr_serialize(a, buf, need - 1u, &n) == FBS_ATTR_E_TRUNCATED);
  CHECK(n == need);
  CHECK(buf[0] == 0x5a);
  n = 999u;
  CHECK(fbs_attr_serialize(a, NULL, 0u, &n) == FBS_ATTR_E_TRUNCATED);
  CHECK(n == need);
  CHECK(fbs_attr_serialize(a, buf, sizeof buf, &n) == FBS_ATTR_OK);
  CHECK(n == need);
  fbs_attr_destroy(a);
}

/* ------------------------------------------------------------------------- */
/* NULL pointers, bad enums, bad values on every entry point                 */
/* ------------------------------------------------------------------------- */

static void test_invalid_arguments(void) {
  fbs_attr *a = make_attr(FBS_ATTR_F64, 1.0, 4u);
  fbs_attr_config cfg;
  fbs_attr_handle h = FBS_ATTR_HANDLE_NONE;
  fbs_attr_modifier m;
  int32_t i32 = 0;
  size_t n = 0;
  double lo = 0.0, hi = 0.0;
  unsigned char buf[64];

  /* create */
  CHECK(fbs_attr_create(NULL, NULL, NULL) == FBS_ATTR_E_INVALID);
  cfg = fbs_attr_config_default();
  cfg.type = 5;
  {
    fbs_attr *tmp = (fbs_attr *)0x1;
    CHECK(fbs_attr_create(&cfg, NULL, &tmp) == FBS_ATTR_E_INVALID);
    CHECK(tmp == (fbs_attr *)0x1);
    cfg = fbs_attr_config_default();
    cfg.base = make_nan();
    CHECK(fbs_attr_create(&cfg, NULL, &tmp) == FBS_ATTR_E_INVALID);
    cfg = fbs_attr_config_default();
    cfg.base = make_inf();
    CHECK(fbs_attr_create(&cfg, NULL, &tmp) == FBS_ATTR_E_INVALID);
    cfg = fbs_attr_config_default();
    cfg.max_modifiers = 0u;
    CHECK(fbs_attr_create(&cfg, NULL, &tmp) == FBS_ATTR_E_INVALID);
    cfg = fbs_attr_config_default();
    cfg.max_modifiers = (1u << 20) + 1u;
    CHECK(fbs_attr_create(&cfg, NULL, &tmp) == FBS_ATTR_E_INVALID);
    CHECK(tmp == (fbs_attr *)0x1);
  }

  /* accessors on NULL */
  CHECK(fbs_attr_type_of(NULL) == -1);
  CHECK(fbs_attr_base(NULL) == 0.0);
  CHECK(fbs_attr_value(NULL) == 0.0);
  CHECK(fbs_attr_count(NULL) == 0u);
  CHECK(fbs_attr_revision(NULL) == 0u);
  CHECK(fbs_attr_memory(NULL) == 0u);
  CHECK(fbs_attr_has_clamp(NULL, &lo, &hi) == 0);
  CHECK(fbs_attr_serialized_size(NULL) == 0u);
  fbs_attr_destroy(NULL);

  /* base */
  CHECK(fbs_attr_set_base(NULL, 1.0) == FBS_ATTR_E_INVALID);
  CHECK(fbs_attr_set_base(a, make_nan()) == FBS_ATTR_E_INVALID);
  CHECK(fbs_attr_set_base(a, make_inf()) == FBS_ATTR_E_INVALID);
  CHECK(fbs_attr_set_base(a, -make_inf()) == FBS_ATTR_E_INVALID);
  CHECK(fbs_attr_base(a) == 1.0);

  /* value_i32 */
  CHECK(fbs_attr_value_i32(NULL, &i32) == FBS_ATTR_E_INVALID);
  CHECK(fbs_attr_value_i32(a, NULL) == FBS_ATTR_E_INVALID);

  /* clamp */
  CHECK(fbs_attr_set_clamp(NULL, 0.0, 1.0) == FBS_ATTR_E_INVALID);
  CHECK(fbs_attr_clear_clamp(NULL) == FBS_ATTR_E_INVALID);

  /* add */
  CHECK(fbs_attr_add(NULL, 0u, FBS_ATTR_ADD, 1.0, &h) == FBS_ATTR_E_INVALID);
  CHECK(fbs_attr_add(a, 0u, FBS_ATTR_ADD, 1.0, NULL) == FBS_ATTR_E_INVALID);
  CHECK(fbs_attr_add(a, 0u, 5, 1.0, &h) == FBS_ATTR_E_INVALID);
  CHECK(fbs_attr_add(a, 0u, -1, 1.0, &h) == FBS_ATTR_E_INVALID);
  CHECK(fbs_attr_add(a, 0u, FBS_ATTR_ADD, make_nan(), &h) == FBS_ATTR_E_INVALID);
  CHECK(fbs_attr_add(a, 0u, FBS_ATTR_ADD, make_inf(), &h) == FBS_ATTR_E_INVALID);
  CHECK(fbs_attr_count(a) == 0u);

  /* remove / get / list / clear */
  CHECK(fbs_attr_remove(NULL, 1u) == FBS_ATTR_E_INVALID);
  CHECK(fbs_attr_get(NULL, 1u, &m) == FBS_ATTR_E_INVALID);
  CHECK(fbs_attr_get(a, 1u, NULL) == FBS_ATTR_E_INVALID);
  CHECK(fbs_attr_list(NULL, &m, 1u, &n) == FBS_ATTR_E_INVALID);
  CHECK(fbs_attr_list(a, &m, 1u, NULL) == FBS_ATTR_E_INVALID);
  CHECK(fbs_attr_list(a, NULL, 1u, &n) == FBS_ATTR_E_INVALID);
  CHECK(fbs_attr_clear_channel(NULL, 0u) == FBS_ATTR_E_INVALID);
  CHECK(fbs_attr_clear(NULL) == FBS_ATTR_E_INVALID);
  CHECK(fbs_attr_set_observer(NULL, NULL, NULL) == FBS_ATTR_E_INVALID);

  /* serialize / deserialize */
  CHECK(fbs_attr_serialize(NULL, buf, sizeof buf, &n) == FBS_ATTR_E_INVALID);
  CHECK(fbs_attr_serialize(a, buf, sizeof buf, NULL) == FBS_ATTR_E_INVALID);
  CHECK(fbs_attr_serialize(a, NULL, 8u, &n) == FBS_ATTR_E_INVALID);
  {
    fbs_attr *tmp = (fbs_attr *)0x1;
    memset(buf, 0, sizeof buf);
    CHECK(fbs_attr_deserialize(NULL, 0u, NULL, NULL, &tmp) == FBS_ATTR_E_INVALID);
    CHECK(fbs_attr_deserialize(buf, sizeof buf, NULL, NULL, NULL) == FBS_ATTR_E_INVALID);
    CHECK(tmp == (fbs_attr *)0x1);
  }
  fbs_attr_destroy(a);
}

/* ------------------------------------------------------------------------- */
/* Status names and version                                                  */
/* ------------------------------------------------------------------------- */

static void test_status_names(void) {
  CHECK(strcmp(fbs_attr_status_name(FBS_ATTR_OK), "ok") == 0);
  CHECK(strcmp(fbs_attr_status_name(FBS_ATTR_E_INVALID), "invalid") == 0);
  CHECK(strcmp(fbs_attr_status_name(FBS_ATTR_E_NOT_FOUND), "not_found") == 0);
  CHECK(strcmp(fbs_attr_status_name(FBS_ATTR_E_FULL), "full") == 0);
  CHECK(strcmp(fbs_attr_status_name(FBS_ATTR_E_RANGE), "range") == 0);
  CHECK(strcmp(fbs_attr_status_name(FBS_ATTR_E_SCHEMA), "schema") == 0);
  CHECK(strcmp(fbs_attr_status_name(FBS_ATTR_E_TRUNCATED), "truncated") == 0);
  CHECK(strcmp(fbs_attr_status_name(FBS_ATTR_E_REENTRANT), "reentrant") == 0);
  CHECK(strcmp(fbs_attr_status_name(FBS_ATTR_E_MEMORY), "memory") == 0);
  CHECK(strcmp(fbs_attr_status_name(-999), "unknown") == 0);
  CHECK(strcmp(fbs_attr_status_name(999), "unknown") == 0);
  CHECK(fbs_attr_version() == FBS_ATTR_VERSION);
  CHECK(fbs_attr_version() == 100u);
  {
    fbs_attr_config cfg = fbs_attr_config_default();
    CHECK(cfg.type == FBS_ATTR_F64);
    CHECK(cfg.base == 0.0);
    CHECK(cfg.max_modifiers == 32u);
  }
}

/* ------------------------------------------------------------------------- */
/* Golden serialization fixture                                              */
/* ------------------------------------------------------------------------- */

static fbs_attr *build_fixture_attr(void) {
  fbs_attr *a = make_attr(FBS_ATTR_F64, 12.5, 32u);
  fbs_attr_handle h3;
  CHECK(fbs_attr_set_clamp(a, -100.0, 250.0) == FBS_ATTR_OK);
  (void)add_mod(a, 0u, FBS_ATTR_ADD, 10.0);       /* handle 1 */
  (void)add_mod(a, 2u, FBS_ATTR_MUL_ADD, 0.5);    /* handle 2 */
  h3 = add_mod(a, 0u, FBS_ATTR_MUL_COMPOUND, 0.25); /* handle 3 */
  (void)add_mod(a, 1u, FBS_ATTR_OVERRIDE, 42.0);  /* handle 4 */
  (void)add_mod(a, 0u, FBS_ATTR_ADD, -2.5);       /* handle 5 */
  CHECK(fbs_attr_remove(a, h3) == FBS_ATTR_OK);
  /* ch0: 12.5 + 10 - 2.5 = 20; ch1: override 42; ch2: 42 * 1.5 = 63 */
  CHECK(fbs_attr_value(a) == 63.0);
  CHECK(fbs_attr_count(a) == 4u);
  return a;
}

static void test_fixture(void) {
  fbs_attr *a = build_fixture_attr();
  fbs_attr *loaded = NULL;
  char path[512];
  size_t len = 0;
  unsigned char *blob = serialize_alloc(a, &len);
  FILE *fp;

  /* 16 header + 16 clamp + 8 next_handle + 4 count + 4 * 24 */
  CHECK(len == 44u + 96u);

  sprintf(path, "%s/attr.bin", g_fixture_dir);
  if (g_write_fixtures) {
    fp = fopen(path, "wb");
    ++g_checks;
    if (!fp) {
      ++g_fails;
      printf("FAIL cannot open %s for writing\n", path);
    } else {
      size_t wrote = fwrite(blob, 1u, len, fp);
      fclose(fp);
      CHECK(wrote == len);
      printf("wrote %s (%lu bytes)\n", path, (unsigned long)len);
    }
  } else {
    fp = fopen(path, "rb");
    ++g_checks;
    if (!fp) {
      ++g_fails;
      printf("FAIL cannot open fixture %s (run with --write-fixtures to create it)\n", path);
    } else {
      unsigned char golden[1024];
      size_t got = fread(golden, 1u, sizeof golden, fp);
      fclose(fp);
      CHECK(got == len);
      if (got == len) {
        size_t l2 = 0;
        unsigned char *b2;
        CHECK(memcmp(golden, blob, len) == 0);
        CHECK(fbs_attr_deserialize(golden, got, NULL, NULL, &loaded) == FBS_ATTR_OK);
        CHECK(fbs_attr_value(loaded) == 63.0);
        CHECK(fbs_attr_count(loaded) == 4u);
        b2 = serialize_alloc(loaded, &l2);
        CHECK(l2 == len && memcmp(b2, golden, len) == 0);
        free(b2);
        fbs_attr_destroy(loaded);
      }
    }
  }
  free(blob);
  fbs_attr_destroy(a);
}

/* ------------------------------------------------------------------------- */

int main(int argc, char **argv) {
  int i;
  for (i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--write-fixtures") == 0) {
      g_write_fixtures = 1;
    } else if (strcmp(argv[i], "--fixture-dir") == 0 && i + 1 < argc) {
      g_fixture_dir = argv[++i];
    } else {
      printf("usage: %s [--write-fixtures] [--fixture-dir DIR]\n", argv[0]);
      return 2;
    }
  }

  test_at1_identity();
  test_at2_order_independence();
  test_at2_random_multisets();
  test_at3_bucket_algebra();
  test_at4_channel_ordering();
  test_at5_override();
  test_at6_removal_is_exact();
  test_at7_handle_stability();
  test_at8_int32();
  test_at9_clamping();
  test_at10_no_silent_drops();
  test_at11_notifications();
  test_destroy_from_observer();
  test_empty_round_trip();
  test_at13_determinism();
  test_at14_schema_rejection();
  test_at15_fuzz();
  test_capacity();
  test_allocator();
  test_truncated();
  test_invalid_arguments();
  test_status_names();
  test_fixture();

  printf("attributes: %d checks passed, %d failed\n", g_checks - g_fails, g_fails);
  return g_fails > 100 ? 100 : g_fails;
}
