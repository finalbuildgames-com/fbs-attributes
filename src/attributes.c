/*
 * src/attributes.c — FinalBuildSystems numeric attribute with bucketed
 * modifiers. Implements include/fbs/attributes.h.
 *
 * Derived from PipeRift/AttributesExtension (Apache License 2.0, Copyright
 * 2015-2026 Piperift) at commit d479b32106d403ac1429287d981df123d15d8669,
 * rewritten in C with changed semantics; see third_party/piperift/NOTICE.md.
 * These files have been changed from the original work: the sequential,
 * order-dependent application model of the original is deliberately replaced
 * by the commutative bucketed contract documented in include/fbs/attributes.h.
 * That fixes defects of the source, among them category order that depended
 * on insertion history, modifier ids that collided after a load, no clamping
 * or int32 range check, silently dropped near-zero modifiers and unguarded
 * reentrancy from change handlers.
 *
 * C99. Standard library plus round/isfinite from libm. No globals, no static
 * mutable state.
 */

#include "fbs/attributes.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Every guarantee about bit-identical results assumes each double operation
 * rounds to double precision. That is FLT_EVAL_METHOD == 0, which is what
 * x86-64 with SSE2, AArch64 and wasm32 give. A target that evaluates doubles in
 * a wider format (classic x87, FLT_EVAL_METHOD == 2) keeps excess precision in
 * intermediate results and will differ in the low-order bits; the arithmetic
 * below is deliberately not changed to compensate. Builds also pass
 * -ffp-contract=off so no FMA contraction happens either. */
#if defined(FLT_EVAL_METHOD) && FLT_EVAL_METHOD != 0
#warning "fbs/attributes: bit-exact results require FLT_EVAL_METHOD == 0 (SSE2, AArch64 or wasm32)"
#endif

/* The binary schema stores IEEE-754 binary64 doubles field by field. */
typedef char fbs_attr_static_assert_f64[(sizeof(double) == 8) ? 1 : -1];

#define FBS_ATTR_SCHEMA_VERSION 1u
#define FBS_ATTR_MAX_MODIFIERS_LIMIT 1048576u /* 1 << 20 */
#define FBS_ATTR_MOD_RECORD_BYTES 24u
#define FBS_ATTR_BLOCK_ALIGN 8u

#define FBS_ATTR_I32_MIN (-2147483648.0)
#define FBS_ATTR_I32_MAX (2147483647.0)

/* ------------------------------------------------------------------------- */
/* Internal representation                                                   */
/* ------------------------------------------------------------------------- */

typedef struct fbs_attr_mod {
  uint64_t handle;
  double value;
  uint16_t channel;
  uint8_t op;
  uint8_t pad[5];
} fbs_attr_mod;

struct fbs_attr {
  fbs_attr_allocator alloc;
  size_t block_size;
  fbs_attr_mod *mods; /* always sorted ascending by (channel, op, handle) */
  /* Scratch index array carved from the same block (never observable). The
   * storage order is the schema order, so evaluation orders each bucket by
   * (value, handle) through this array, and deserialize proves handle
   * uniqueness through it. */
  unsigned *order;
  unsigned max_modifiers;
  unsigned count;
  int type;
  double base;
  double value; /* cached, recomputed on every effective change */
  double clamp_lo;
  double clamp_hi;
  int has_clamp;
  uint64_t next_handle; /* 0 means exhausted */
  uint32_t revision;
  fbs_attr_observer observer;
  void *observer_user;
  int in_observer;
  int destroy_pending; /* fbs_attr_destroy was called from inside the observer */
};

static void *fbs_attr_default_alloc(void *user, size_t bytes) {
  (void)user;
  return malloc(bytes);
}

static void fbs_attr_default_free(void *user, void *ptr) {
  (void)user;
  free(ptr);
}

static size_t fbs_attr_align_up(size_t v) {
  return (v + (FBS_ATTR_BLOCK_ALIGN - 1u)) & ~(size_t)(FBS_ATTR_BLOCK_ALIGN - 1u);
}

static int fbs_attr_op_valid(int op) {
  return op == FBS_ATTR_ADD || op == FBS_ATTR_MUL_ADD || op == FBS_ATTR_MUL_COMPOUND ||
         op == FBS_ATTR_ADD_FINAL || op == FBS_ATTR_OVERRIDE;
}

static int fbs_attr_type_valid(int t) {
  return t == FBS_ATTR_F32 || t == FBS_ATTR_F64 || t == FBS_ATTR_I32;
}

/* ------------------------------------------------------------------------- */
/* Status names, version, config                                             */
/* ------------------------------------------------------------------------- */

const char *fbs_attr_status_name(int status) {
  switch (status) {
    case FBS_ATTR_OK: return "ok";
    case FBS_ATTR_E_INVALID: return "invalid";
    case FBS_ATTR_E_NOT_FOUND: return "not_found";
    case FBS_ATTR_E_FULL: return "full";
    case FBS_ATTR_E_RANGE: return "range";
    case FBS_ATTR_E_SCHEMA: return "schema";
    case FBS_ATTR_E_TRUNCATED: return "truncated";
    case FBS_ATTR_E_REENTRANT: return "reentrant";
    case FBS_ATTR_E_MEMORY: return "memory";
    default: return "unknown";
  }
}

unsigned fbs_attr_version(void) { return FBS_ATTR_VERSION; }

fbs_attr_config fbs_attr_config_default(void) {
  fbs_attr_config cfg;
  cfg.type = FBS_ATTR_F64;
  cfg.base = 0.0;
  cfg.max_modifiers = 32u;
  return cfg;
}

static int fbs_attr_config_valid(const fbs_attr_config *cfg) {
  if (!fbs_attr_type_valid(cfg->type)) return 0;
  if (!isfinite(cfg->base)) return 0;
  if (cfg->max_modifiers < 1u || cfg->max_modifiers > FBS_ATTR_MAX_MODIFIERS_LIMIT) return 0;
  return 1;
}

/* ------------------------------------------------------------------------- */
/* Evaluation (include/fbs/attributes.h file comment; decision section 4.2)   */
/* ------------------------------------------------------------------------- */

/* Ordering modes for the scratch index sort. */
#define FBS_ATTR_ORDER_VALUE 0
#define FBS_ATTR_ORDER_HANDLE 1

/* Strict weak ordering over indices into `m`. Values are validated finite on
 * the way in, so `<` is a total order on them apart from +0.0 / -0.0, which
 * compare equal and are separated by the handle; both orders give the same sum
 * and the same (1 + v) product, so the tie-break never affects the result. */
static int fbs_attr_index_less(const fbs_attr_mod *m, unsigned i, unsigned j, int mode) {
  if (mode == FBS_ATTR_ORDER_HANDLE) return m[i].handle < m[j].handle;
  if (m[i].value < m[j].value) return 1;
  if (m[i].value > m[j].value) return 0;
  return m[i].handle < m[j].handle;
}

static void fbs_attr_sift(const fbs_attr_mod *m, unsigned *order, unsigned root, unsigned n,
                          int mode) {
  while (root * 2u + 1u < n) {
    unsigned child = root * 2u + 1u;
    unsigned tmp;
    if (child + 1u < n && fbs_attr_index_less(m, order[child], order[child + 1u], mode))
      child += 1u;
    if (!fbs_attr_index_less(m, order[root], order[child], mode)) return;
    tmp = order[root];
    order[root] = order[child];
    order[child] = tmp;
    root = child;
  }
}

/* Heapsort of an index array: O(n log n), in place, no scratch allocation. */
static void fbs_attr_sort_indices(const fbs_attr_mod *m, unsigned *order, unsigned n, int mode) {
  unsigned i;
  if (n < 2u) return;
  for (i = n / 2u; i-- > 0u;) fbs_attr_sift(m, order, i, n, mode);
  for (i = n; i-- > 1u;) {
    unsigned tmp = order[0];
    order[0] = order[i];
    order[i] = tmp;
    fbs_attr_sift(m, order, 0u, i, mode);
  }
}

/* Writes into the scratch index array, hence the non-const attribute. */
static double fbs_attr_eval(fbs_attr *a) {
  double v = a->base;
  unsigned i = 0u;

  while (i < a->count) {
    uint16_t ch = a->mods[i].channel;
    unsigned channel_end = i;
    unsigned bucket = i;
    unsigned override_at = 0u;
    int has_override = 0;
    double sum_add = 0.0, sum_mul_add = 0.0, prod_compound = 1.0, sum_add_final = 0.0;

    while (channel_end < a->count && a->mods[channel_end].channel == ch) ++channel_end;

    /* The array is sorted by (channel, op, handle), so one channel is a
     * contiguous run and each (channel, op) bucket inside it is contiguous and
     * appears in enum order. Accumulation inside a bucket, however, follows
     * ascending (value, handle) — established here through the scratch index
     * array — so the result depends only on the multiset of
     * (channel, op, value) and never on insertion order or handle numbering. */
    while (bucket < channel_end) {
      unsigned bucket_end = bucket;
      unsigned n, k;
      uint8_t op = a->mods[bucket].op;
      while (bucket_end < channel_end && a->mods[bucket_end].op == op) ++bucket_end;
      n = bucket_end - bucket;

      if ((int)op == FBS_ATTR_OVERRIDE) {
        /* Lowest handle wins, not lowest value: the bucket is stored in
         * ascending handle order, so that is its first entry. */
        if (!has_override) {
          has_override = 1;
          override_at = bucket;
        }
      } else {
        for (k = 0u; k < n; ++k) a->order[k] = bucket + k;
        fbs_attr_sort_indices(a->mods, a->order, n, FBS_ATTR_ORDER_VALUE);
        for (k = 0u; k < n; ++k) {
          const fbs_attr_mod *m = &a->mods[a->order[k]];
          switch ((int)op) {
            case FBS_ATTR_ADD: sum_add += m->value; break;
            case FBS_ATTR_MUL_ADD: sum_mul_add += m->value; break;
            case FBS_ATTR_MUL_COMPOUND: prod_compound *= (1.0 + m->value); break;
            default: sum_add_final += m->value; break; /* FBS_ATTR_ADD_FINAL */
          }
        }
      }
      bucket = bucket_end;
    }

    if (has_override) {
      v = a->mods[override_at].value;
    } else {
      double t = (v + sum_add) * (1.0 + sum_mul_add);
      t = t * prod_compound;
      v = t + sum_add_final;
    }
    i = channel_end;
  }

  if (a->has_clamp) {
    /* Plain comparisons: a NaN produced by overflow propagates instead of
     * being silently clamped to lo. */
    if (v < a->clamp_lo) v = a->clamp_lo;
    if (v > a->clamp_hi) v = a->clamp_hi;
  }
  if (a->type == FBS_ATTR_F32) {
    v = (double)(float)v;
  } else if (a->type == FBS_ATTR_I32) {
    /* The clamp is applied before the rounding, per the header, so a clamp
     * window with non-integer bounds can produce a final value outside it:
     * clamping to [0.5, 0.7] then rounding half away from zero yields 1. This
     * is documented behaviour, not a bug — callers of an I32 attribute are
     * expected to use integer bounds. */
    v = round(v); /* half away from zero */
  }
  return v;
}

/* Frees the single block. Never called while the observer is running. */
static void fbs_attr_release(fbs_attr *a) {
  fbs_attr_allocator al = a->alloc; /* copy: the allocator lives in the block */
  al.free(al.user, a);
}

/* Recompute the cache, bump the revision and notify. `prev` is the cached
 * value from before the state change.
 *
 * If the observer called fbs_attr_destroy, the block is released here, so
 * `a` is dangling once this returns. Every caller of fbs_attr_commit must
 * therefore treat it as the last use of `a` — they all do: each one returns a
 * status immediately afterwards and touches only locals. */
static void fbs_attr_commit(fbs_attr *a, double prev, int change, fbs_attr_handle handle) {
  a->value = fbs_attr_eval(a);
  a->revision += 1u;
  if (a->observer) {
    fbs_attr_observer observer = a->observer;
    void *user = a->observer_user;
    a->in_observer = 1;
    observer(user, a, prev, change, handle);
    a->in_observer = 0;
    if (a->destroy_pending) {
      fbs_attr_release(a);
      return; /* `a` must not be touched again */
    }
  }
}

/* ------------------------------------------------------------------------- */
/* Sorted modifier store                                                     */
/* ------------------------------------------------------------------------- */

/* First index whose (channel, op) is greater than (ch, op): the insertion
 * point for a new modifier, whose handle is larger than every existing one. */
static unsigned fbs_attr_upper_bound(const fbs_attr *a, uint16_t ch, int op) {
  unsigned lo = 0u, hi = a->count;
  while (lo < hi) {
    unsigned mid = lo + (hi - lo) / 2u;
    const fbs_attr_mod *m = &a->mods[mid];
    int greater = (m->channel > ch) || (m->channel == ch && (int)m->op > op);
    if (greater)
      hi = mid;
    else
      lo = mid + 1u;
  }
  return lo;
}

/* First index with channel >= ch. */
static unsigned fbs_attr_channel_begin(const fbs_attr *a, uint16_t ch) {
  unsigned lo = 0u, hi = a->count;
  while (lo < hi) {
    unsigned mid = lo + (hi - lo) / 2u;
    if (a->mods[mid].channel < ch)
      lo = mid + 1u;
    else
      hi = mid;
  }
  return lo;
}

/* First index with channel > ch. */
static unsigned fbs_attr_channel_end(const fbs_attr *a, uint16_t ch) {
  unsigned lo = 0u, hi = a->count;
  while (lo < hi) {
    unsigned mid = lo + (hi - lo) / 2u;
    if (a->mods[mid].channel <= ch)
      lo = mid + 1u;
    else
      hi = mid;
  }
  return lo;
}

static unsigned fbs_attr_index_of(const fbs_attr *a, fbs_attr_handle handle) {
  unsigned i;
  for (i = 0u; i < a->count; ++i)
    if (a->mods[i].handle == handle) return i;
  return a->count; /* not found */
}

/* ------------------------------------------------------------------------- */
/* Lifetime                                                                  */
/* ------------------------------------------------------------------------- */

static fbs_attr_status fbs_attr_alloc_block(const fbs_attr_config *cfg,
                                            const fbs_attr_allocator *alloc, fbs_attr **out) {
  fbs_attr_allocator al;
  size_t off_mods, off_order, total;
  unsigned char *block;
  fbs_attr *a;

  if (alloc) {
    if (!alloc->alloc || !alloc->free) return FBS_ATTR_E_INVALID;
    al = *alloc;
  } else {
    al.alloc = fbs_attr_default_alloc;
    al.free = fbs_attr_default_free;
    al.user = NULL;
  }

  off_mods = fbs_attr_align_up(sizeof(struct fbs_attr));
  off_order = fbs_attr_align_up(off_mods + (size_t)cfg->max_modifiers * sizeof(fbs_attr_mod));
  total = fbs_attr_align_up(off_order + (size_t)cfg->max_modifiers * sizeof(unsigned));

  block = (unsigned char *)al.alloc(al.user, total);
  if (!block) return FBS_ATTR_E_MEMORY;
  memset(block, 0, total);

  a = (fbs_attr *)(void *)block;
  a->alloc = al;
  a->block_size = total;
  a->mods = (fbs_attr_mod *)(void *)(block + off_mods);
  a->order = (unsigned *)(void *)(block + off_order);
  a->max_modifiers = cfg->max_modifiers;
  a->count = 0u;
  a->type = cfg->type;
  a->base = cfg->base;
  a->clamp_lo = 0.0;
  a->clamp_hi = 0.0;
  a->has_clamp = 0;
  a->next_handle = 1u;
  a->revision = 0u;
  a->observer = NULL;
  a->observer_user = NULL;
  a->in_observer = 0;
  a->destroy_pending = 0;
  a->value = fbs_attr_eval(a);

  *out = a;
  return FBS_ATTR_OK;
}

fbs_attr_status fbs_attr_create(const fbs_attr_config *cfg, const fbs_attr_allocator *alloc,
                                fbs_attr **out) {
  fbs_attr_config c;
  fbs_attr *a = NULL;
  fbs_attr_status st;

  if (!out) return FBS_ATTR_E_INVALID;
  c = cfg ? *cfg : fbs_attr_config_default();
  if (!fbs_attr_config_valid(&c)) return FBS_ATTR_E_INVALID;

  st = fbs_attr_alloc_block(&c, alloc, &a);
  if (st != FBS_ATTR_OK) return st;
  *out = a;
  return FBS_ATTR_OK;
}

void fbs_attr_destroy(fbs_attr *a) {
  if (!a) return;
  if (a->in_observer) {
    /* Destroying from inside the observer is legal but deferred: the block
     * cannot be released here because fbs_attr_commit still has to unwind
     * through it. The flag is picked up the moment the observer returns and
     * the block is freed there, with no further access to `a`. */
    a->destroy_pending = 1;
    return;
  }
  fbs_attr_release(a);
}

size_t fbs_attr_memory(const fbs_attr *a) { return a ? a->block_size : (size_t)0; }

int fbs_attr_type_of(const fbs_attr *a) { return a ? a->type : -1; }

/* ------------------------------------------------------------------------- */
/* Values                                                                    */
/* ------------------------------------------------------------------------- */

fbs_attr_status fbs_attr_set_base(fbs_attr *a, double base) {
  double prev;
  if (!a) return FBS_ATTR_E_INVALID;
  if (a->in_observer) return FBS_ATTR_E_REENTRANT;
  if (!isfinite(base)) return FBS_ATTR_E_INVALID;
  if (base == a->base) return FBS_ATTR_OK; /* no-op: no notification, no revision bump */
  prev = a->value;
  a->base = base;
  fbs_attr_commit(a, prev, FBS_ATTR_CHANGE_BASE, FBS_ATTR_HANDLE_NONE);
  return FBS_ATTR_OK;
}

double fbs_attr_base(const fbs_attr *a) { return a ? a->base : 0.0; }

double fbs_attr_value(const fbs_attr *a) { return a ? a->value : 0.0; }

fbs_attr_status fbs_attr_value_i32(const fbs_attr *a, int32_t *out) {
  double r;
  if (!a || !out) return FBS_ATTR_E_INVALID;
  r = round(a->value); /* identity for FBS_ATTR_I32; also rejects NaN below */
  if (!(r >= FBS_ATTR_I32_MIN && r <= FBS_ATTR_I32_MAX)) return FBS_ATTR_E_RANGE;
  *out = (int32_t)r;
  return FBS_ATTR_OK;
}

fbs_attr_status fbs_attr_set_clamp(fbs_attr *a, double lo, double hi) {
  double prev;
  if (!a) return FBS_ATTR_E_INVALID;
  if (a->in_observer) return FBS_ATTR_E_REENTRANT;
  if (isnan(lo) || isnan(hi) || !(lo <= hi)) return FBS_ATTR_E_INVALID;
  if (a->has_clamp && a->clamp_lo == lo && a->clamp_hi == hi) return FBS_ATTR_OK;
  prev = a->value;
  a->clamp_lo = lo;
  a->clamp_hi = hi;
  a->has_clamp = 1;
  fbs_attr_commit(a, prev, FBS_ATTR_CHANGE_CLAMP, FBS_ATTR_HANDLE_NONE);
  return FBS_ATTR_OK;
}

fbs_attr_status fbs_attr_clear_clamp(fbs_attr *a) {
  double prev;
  if (!a) return FBS_ATTR_E_INVALID;
  if (a->in_observer) return FBS_ATTR_E_REENTRANT;
  if (!a->has_clamp) return FBS_ATTR_OK; /* no-op */
  prev = a->value;
  a->has_clamp = 0;
  a->clamp_lo = 0.0;
  a->clamp_hi = 0.0;
  fbs_attr_commit(a, prev, FBS_ATTR_CHANGE_CLAMP, FBS_ATTR_HANDLE_NONE);
  return FBS_ATTR_OK;
}

int fbs_attr_has_clamp(const fbs_attr *a, double *out_lo, double *out_hi) {
  if (!a || !a->has_clamp) return 0;
  if (out_lo) *out_lo = a->clamp_lo;
  if (out_hi) *out_hi = a->clamp_hi;
  return 1;
}

/* ------------------------------------------------------------------------- */
/* Modifiers                                                                 */
/* ------------------------------------------------------------------------- */

fbs_attr_status fbs_attr_add(fbs_attr *a, uint16_t channel, int op, double value,
                             fbs_attr_handle *out_handle) {
  unsigned pos;
  fbs_attr_handle handle;
  double prev;

  if (!a) return FBS_ATTR_E_INVALID;
  if (a->in_observer) return FBS_ATTR_E_REENTRANT;
  if (!out_handle || !fbs_attr_op_valid(op) || !isfinite(value)) return FBS_ATTR_E_INVALID;
  if (a->count >= a->max_modifiers || a->next_handle == 0u) return FBS_ATTR_E_FULL;

  handle = a->next_handle;
  pos = fbs_attr_upper_bound(a, channel, op);
  if (pos < a->count)
    memmove(&a->mods[pos + 1u], &a->mods[pos], (size_t)(a->count - pos) * sizeof(fbs_attr_mod));
  memset(&a->mods[pos], 0, sizeof(fbs_attr_mod));
  a->mods[pos].handle = handle;
  a->mods[pos].value = value;
  a->mods[pos].channel = channel;
  a->mods[pos].op = (uint8_t)op;
  a->count += 1u;
  a->next_handle = handle + 1u; /* wraps to 0 only after 2^64 - 1 modifiers */

  prev = a->value;
  fbs_attr_commit(a, prev, FBS_ATTR_CHANGE_ADDED, handle);
  *out_handle = handle;
  return FBS_ATTR_OK;
}

fbs_attr_status fbs_attr_remove(fbs_attr *a, fbs_attr_handle handle) {
  unsigned pos;
  double prev;

  if (!a) return FBS_ATTR_E_INVALID;
  if (a->in_observer) return FBS_ATTR_E_REENTRANT;
  if (handle == FBS_ATTR_HANDLE_NONE) return FBS_ATTR_E_NOT_FOUND;
  pos = fbs_attr_index_of(a, handle);
  if (pos == a->count) return FBS_ATTR_E_NOT_FOUND;

  if (pos + 1u < a->count)
    memmove(&a->mods[pos], &a->mods[pos + 1u],
            (size_t)(a->count - pos - 1u) * sizeof(fbs_attr_mod));
  a->count -= 1u;
  prev = a->value;
  fbs_attr_commit(a, prev, FBS_ATTR_CHANGE_REMOVED, handle);
  return FBS_ATTR_OK;
}

fbs_attr_status fbs_attr_clear_channel(fbs_attr *a, uint16_t channel) {
  unsigned begin, end;
  double prev;

  if (!a) return FBS_ATTR_E_INVALID;
  if (a->in_observer) return FBS_ATTR_E_REENTRANT;
  begin = fbs_attr_channel_begin(a, channel);
  end = fbs_attr_channel_end(a, channel);
  if (begin == end) return FBS_ATTR_OK; /* no-op */

  if (end < a->count)
    memmove(&a->mods[begin], &a->mods[end], (size_t)(a->count - end) * sizeof(fbs_attr_mod));
  a->count -= (end - begin);
  prev = a->value;
  fbs_attr_commit(a, prev, FBS_ATTR_CHANGE_CHANNEL_CLEARED, FBS_ATTR_HANDLE_NONE);
  return FBS_ATTR_OK;
}

fbs_attr_status fbs_attr_clear(fbs_attr *a) {
  double prev;
  if (!a) return FBS_ATTR_E_INVALID;
  if (a->in_observer) return FBS_ATTR_E_REENTRANT;
  if (a->count == 0u) return FBS_ATTR_OK; /* no-op */
  a->count = 0u;
  prev = a->value;
  fbs_attr_commit(a, prev, FBS_ATTR_CHANGE_CLEARED, FBS_ATTR_HANDLE_NONE);
  return FBS_ATTR_OK;
}

unsigned fbs_attr_count(const fbs_attr *a) { return a ? a->count : 0u; }

fbs_attr_status fbs_attr_get(const fbs_attr *a, fbs_attr_handle handle, fbs_attr_modifier *out) {
  unsigned pos;
  if (!a || !out) return FBS_ATTR_E_INVALID;
  if (handle == FBS_ATTR_HANDLE_NONE) return FBS_ATTR_E_NOT_FOUND;
  pos = fbs_attr_index_of(a, handle);
  if (pos == a->count) return FBS_ATTR_E_NOT_FOUND;
  out->handle = a->mods[pos].handle;
  out->channel = a->mods[pos].channel;
  out->op = (int)a->mods[pos].op;
  out->value = a->mods[pos].value;
  return FBS_ATTR_OK;
}

fbs_attr_status fbs_attr_list(const fbs_attr *a, fbs_attr_modifier *out, size_t cap, size_t *count) {
  unsigned i;
  if (!a || !count) return FBS_ATTR_E_INVALID;
  if (!out && cap > 0u) return FBS_ATTR_E_INVALID;
  if ((size_t)a->count > cap) {
    *count = (size_t)a->count;
    return FBS_ATTR_E_TRUNCATED;
  }
  for (i = 0u; i < a->count; ++i) {
    out[i].handle = a->mods[i].handle;
    out[i].channel = a->mods[i].channel;
    out[i].op = (int)a->mods[i].op;
    out[i].value = a->mods[i].value;
  }
  *count = (size_t)a->count;
  return FBS_ATTR_OK;
}

fbs_attr_status fbs_attr_set_observer(fbs_attr *a, fbs_attr_observer observer, void *user) {
  if (!a) return FBS_ATTR_E_INVALID;
  if (a->in_observer) return FBS_ATTR_E_REENTRANT;
  a->observer = observer;
  a->observer_user = user;
  return FBS_ATTR_OK;
}

uint32_t fbs_attr_revision(const fbs_attr *a) { return a ? a->revision : 0u; }

/* ------------------------------------------------------------------------- */
/* Serialization                                                             */
/* ------------------------------------------------------------------------- */

static void fbs_attr_put_u16(unsigned char *p, unsigned v) {
  p[0] = (unsigned char)(v & 0xffu);
  p[1] = (unsigned char)((v >> 8) & 0xffu);
}

static void fbs_attr_put_u32(unsigned char *p, uint32_t v) {
  p[0] = (unsigned char)(v & 0xffu);
  p[1] = (unsigned char)((v >> 8) & 0xffu);
  p[2] = (unsigned char)((v >> 16) & 0xffu);
  p[3] = (unsigned char)((v >> 24) & 0xffu);
}

static void fbs_attr_put_u64(unsigned char *p, uint64_t v) {
  int i;
  for (i = 0; i < 8; ++i) p[i] = (unsigned char)((v >> (i * 8)) & 0xffu);
}

static void fbs_attr_put_f64(unsigned char *p, double v) {
  uint64_t bits;
  memcpy(&bits, &v, sizeof bits);
  fbs_attr_put_u64(p, bits);
}

static unsigned fbs_attr_get_u16(const unsigned char *p) {
  return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

static uint32_t fbs_attr_get_u32(const unsigned char *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t fbs_attr_get_u64(const unsigned char *p) {
  uint64_t v = 0u;
  int i;
  for (i = 7; i >= 0; --i) v = (v << 8) | (uint64_t)p[i];
  return v;
}

static double fbs_attr_get_f64(const unsigned char *p) {
  uint64_t bits = fbs_attr_get_u64(p);
  double v;
  memcpy(&v, &bits, sizeof v);
  return v;
}

static size_t fbs_attr_head_bytes(int has_clamp) {
  return (size_t)16u + (has_clamp ? (size_t)16u : (size_t)0u) + (size_t)8u + (size_t)4u;
}

size_t fbs_attr_serialized_size(const fbs_attr *a) {
  if (!a) return 0u;
  return fbs_attr_head_bytes(a->has_clamp) + (size_t)a->count * FBS_ATTR_MOD_RECORD_BYTES;
}

fbs_attr_status fbs_attr_serialize(const fbs_attr *a, void *buf, size_t cap, size_t *out_len) {
  unsigned char *p;
  size_t need, off;
  unsigned i;

  if (!a || !out_len) return FBS_ATTR_E_INVALID;
  if (!buf && cap > 0u) return FBS_ATTR_E_INVALID;
  need = fbs_attr_serialized_size(a);
  if (cap < need) {
    *out_len = need;
    return FBS_ATTR_E_TRUNCATED;
  }

  p = (unsigned char *)buf;
  p[0] = 'F';
  p[1] = 'B';
  p[2] = 'S';
  p[3] = 'A';
  fbs_attr_put_u16(p + 4, FBS_ATTR_SCHEMA_VERSION);
  p[6] = (unsigned char)a->type;
  p[7] = a->has_clamp ? (unsigned char)1u : (unsigned char)0u;
  fbs_attr_put_f64(p + 8, a->base);
  off = 16u;
  if (a->has_clamp) {
    fbs_attr_put_f64(p + off, a->clamp_lo);
    fbs_attr_put_f64(p + off + 8u, a->clamp_hi);
    off += 16u;
  }
  fbs_attr_put_u64(p + off, a->next_handle);
  off += 8u;
  fbs_attr_put_u32(p + off, (uint32_t)a->count);
  off += 4u;

  for (i = 0u; i < a->count; ++i) {
    unsigned char *r = p + off;
    fbs_attr_put_u64(r, a->mods[i].handle);
    fbs_attr_put_u16(r + 8, a->mods[i].channel);
    r[10] = a->mods[i].op;
    r[11] = 0u;
    fbs_attr_put_u32(r + 12, 0u);
    fbs_attr_put_f64(r + 16, a->mods[i].value);
    off += FBS_ATTR_MOD_RECORD_BYTES;
  }

  *out_len = need;
  return FBS_ATTR_OK;
}

static void fbs_attr_read_record(fbs_attr_mod *m, const unsigned char *r) {
  memset(m, 0, sizeof *m);
  m->handle = fbs_attr_get_u64(r);
  m->channel = (uint16_t)fbs_attr_get_u16(r + 8);
  m->op = r[10];
  m->value = fbs_attr_get_f64(r + 16);
}

fbs_attr_status fbs_attr_deserialize(const void *buf, size_t len, const fbs_attr_config *cfg,
                                     const fbs_attr_allocator *alloc, fbs_attr **out) {
  const unsigned char *p = (const unsigned char *)buf;
  const unsigned char *recs;
  size_t head, need, off;
  uint64_t next_handle;
  uint32_t mod_count, i;
  double base, clamp_lo = 0.0, clamp_hi = 0.0;
  int type, has_clamp;
  unsigned prev_channel = 0u;
  int prev_op = 0;
  uint64_t prev_handle = 0u;
  int first = 1;
  fbs_attr_config c;
  fbs_attr *a = NULL;
  fbs_attr_status st;

  if (!buf || !out) return FBS_ATTR_E_INVALID;
  if (cfg && !fbs_attr_config_valid(cfg)) return FBS_ATTR_E_INVALID;

  if (len < 16u) return FBS_ATTR_E_SCHEMA;
  if (p[0] != 'F' || p[1] != 'B' || p[2] != 'S' || p[3] != 'A') return FBS_ATTR_E_SCHEMA;
  if (fbs_attr_get_u16(p + 4) != FBS_ATTR_SCHEMA_VERSION) return FBS_ATTR_E_SCHEMA;
  type = (int)p[6];
  if (!fbs_attr_type_valid(type)) return FBS_ATTR_E_SCHEMA;
  if ((p[7] & (unsigned char)0xfeu) != 0u) return FBS_ATTR_E_SCHEMA; /* only bit 0 is defined */
  has_clamp = (p[7] & 1u) != 0u;
  base = fbs_attr_get_f64(p + 8);
  if (!isfinite(base)) return FBS_ATTR_E_SCHEMA;

  head = fbs_attr_head_bytes(has_clamp);
  if (len < head) return FBS_ATTR_E_SCHEMA;
  off = 16u;
  if (has_clamp) {
    clamp_lo = fbs_attr_get_f64(p + off);
    clamp_hi = fbs_attr_get_f64(p + off + 8u);
    if (isnan(clamp_lo) || isnan(clamp_hi) || !(clamp_lo <= clamp_hi)) return FBS_ATTR_E_SCHEMA;
    off += 16u;
  }
  next_handle = fbs_attr_get_u64(p + off);
  off += 8u;
  mod_count = fbs_attr_get_u32(p + off);
  off += 4u;
  if (next_handle == 0u) return FBS_ATTR_E_SCHEMA;
  if (mod_count > FBS_ATTR_MAX_MODIFIERS_LIMIT) return FBS_ATTR_E_SCHEMA;
  need = head + (size_t)mod_count * FBS_ATTR_MOD_RECORD_BYTES;
  if (len != need) return FBS_ATTR_E_SCHEMA;

  recs = p + off;
  for (i = 0u; i < mod_count; ++i) {
    const unsigned char *r = recs + (size_t)i * FBS_ATTR_MOD_RECORD_BYTES;
    uint64_t handle = fbs_attr_get_u64(r);
    unsigned channel = fbs_attr_get_u16(r + 8);
    int op = (int)r[10];
    double value = fbs_attr_get_f64(r + 16);
    int ordered;
    if (r[11] != 0u || fbs_attr_get_u32(r + 12) != 0u) return FBS_ATTR_E_SCHEMA;
    if (!fbs_attr_op_valid(op)) return FBS_ATTR_E_SCHEMA;
    if (!isfinite(value)) return FBS_ATTR_E_SCHEMA;
    if (handle == 0u || handle >= next_handle) return FBS_ATTR_E_SCHEMA;
    ordered = first || channel > prev_channel || (channel == prev_channel && op > prev_op) ||
              (channel == prev_channel && op == prev_op && handle > prev_handle);
    if (!ordered) return FBS_ATTR_E_SCHEMA;
    prev_channel = channel;
    prev_op = op;
    prev_handle = handle;
    first = 0;
  }

  if (cfg) {
    c = *cfg;
    if (c.type != type) return FBS_ATTR_E_SCHEMA;
    if (c.max_modifiers < mod_count) return FBS_ATTR_E_FULL;
  } else {
    c = fbs_attr_config_default();
    c.type = type;
    if ((uint32_t)c.max_modifiers < mod_count) c.max_modifiers = (unsigned)mod_count;
  }
  c.base = base; /* the blob owns type and base */

  st = fbs_attr_alloc_block(&c, alloc, &a);
  if (st != FBS_ATTR_OK) return st;

  for (i = 0u; i < mod_count; ++i)
    fbs_attr_read_record(&a->mods[i], recs + (size_t)i * FBS_ATTR_MOD_RECORD_BYTES);

  /* Sortedness by (channel, op, handle) does not preclude the same handle
   * appearing under two different channels; prove uniqueness explicitly. The
   * scratch index array is sorted rather than the records, so the canonical
   * storage order is never disturbed. */
  for (i = 0u; i < mod_count; ++i) a->order[i] = (unsigned)i;
  fbs_attr_sort_indices(a->mods, a->order, (unsigned)mod_count, FBS_ATTR_ORDER_HANDLE);
  for (i = 1u; i < mod_count; ++i) {
    if (a->mods[a->order[i - 1u]].handle == a->mods[a->order[i]].handle) {
      fbs_attr_destroy(a);
      return FBS_ATTR_E_SCHEMA;
    }
  }

  a->count = (unsigned)mod_count;
  a->has_clamp = has_clamp;
  a->clamp_lo = clamp_lo;
  a->clamp_hi = clamp_hi;
  a->next_handle = next_handle;
  a->value = fbs_attr_eval(a);

  *out = a;
  return FBS_ATTR_OK;
}
