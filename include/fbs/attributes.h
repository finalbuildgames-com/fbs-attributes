/*
 * fbs/attributes.h — FinalBuildSystems numeric attribute with modifiers.
 * C99, engine independent, libm only (round).
 *
 * Model (decision: docs/decisions/attributes.md): one attribute holds a base
 * value and modifiers. Each modifier has a channel (uint16, evaluated in
 * ascending numeric order), an operation and a value. Within a channel the
 * buckets combine commutatively, and sums and products are accumulated in
 * ascending (value, handle) order, so the result is bit-identical for the same
 * multiset of (channel, op, value) regardless of insertion order or handles,
 * and bit-stable across save/load. The one exception is OVERRIDE: when a
 * channel holds several overrides the lowest handle wins, so that result does
 * depend on insertion order by design. Bit-exactness assumes FLT_EVAL_METHOD 0
 * (every 64-bit target and wasm32; x87 builds need -mfpmath=sse). Contract:
 *
 *   v = base                                    (double for every type)
 *   for each channel with modifiers, ascending:
 *     if the channel has an OVERRIDE: v = value of the OVERRIDE with the lowest handle
 *     else v = (v + sum ADD) * (1 + sum MUL_ADD) * product (1 + MUL_COMPOUND) + sum ADD_FINAL
 *          (each sum/product taken in ascending value order, ties by handle)
 *   if a clamp is set: v = min(max(v, lo), hi)
 *   FBS_ATTR_F32: v = (double)(float)v
 *   FBS_ATTR_I32: v = round half away from zero (C round); fbs_attr_value_i32
 *                 reports FBS_ATTR_E_RANGE outside [INT32_MIN, INT32_MAX].
 *                 Rounding happens after the clamp, so a non-integer clamp
 *                 window can be left by the rounded value; use integer bounds.
 * fbs_attr_destroy may be called from inside an observer; the block is freed
 * after the observer returns.
 *
 * Handles are per-attribute, monotonic, never reused, persisted with the
 * attribute; 0 is never a valid handle. Zero-valued modifiers are stored.
 * Memory: one allocation in fbs_attr_create sized by max_modifiers. Observers
 * are called synchronously after each effective change; mutating the attribute
 * from inside the observer fails with FBS_ATTR_E_REENTRANT. Errors leave
 * outputs untouched except FBS_ATTR_E_TRUNCATED (required size reported).
 */
#ifndef FBS_ATTRIBUTES_H
#define FBS_ATTRIBUTES_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FBS_ATTR_VERSION 100

typedef enum fbs_attr_type { FBS_ATTR_F32 = 0, FBS_ATTR_F64 = 1, FBS_ATTR_I32 = 2 } fbs_attr_type;

typedef enum fbs_attr_op {
  FBS_ATTR_ADD = 0,          /* summed, added before the multipliers */
  FBS_ATTR_MUL_ADD = 1,      /* summed, multiplier is (1 + sum); sum below -1 flips the sign */
  FBS_ATTR_MUL_COMPOUND = 2, /* product of (1 + value) */
  FBS_ATTR_ADD_FINAL = 3,    /* summed, added after the multipliers */
  FBS_ATTR_OVERRIDE = 4      /* replaces the channel result; lowest handle wins */
} fbs_attr_op;

typedef enum fbs_attr_status {
  FBS_ATTR_OK = 0,
  FBS_ATTR_E_INVALID = -1,   /* NULL pointer, non-finite value, bad enum, lo > hi */
  FBS_ATTR_E_NOT_FOUND = -2, /* unknown handle */
  FBS_ATTR_E_FULL = -3,      /* max_modifiers reached */
  FBS_ATTR_E_RANGE = -4,     /* value outside int32 */
  FBS_ATTR_E_SCHEMA = -5,    /* blob magic/version/length/consistency mismatch */
  FBS_ATTR_E_TRUNCATED = -6, /* output too small; required count/length written */
  FBS_ATTR_E_REENTRANT = -7, /* mutation attempted from inside an observer */
  FBS_ATTR_E_MEMORY = -8     /* allocator returned NULL */
} fbs_attr_status;

const char *fbs_attr_status_name(int status);
unsigned fbs_attr_version(void);

typedef struct fbs_attr_allocator {
  void *(*alloc)(void *user, size_t bytes);
  void (*free)(void *user, void *ptr);
  void *user;
} fbs_attr_allocator;

typedef uint64_t fbs_attr_handle;
#define FBS_ATTR_HANDLE_NONE ((fbs_attr_handle)0)

typedef struct fbs_attr_config {
  int type;                /* fbs_attr_type */
  double base;             /* finite */
  unsigned max_modifiers;  /* >= 1, <= 1<<20 */
} fbs_attr_config;

/* FBS_ATTR_F64, base 0, 32 modifiers. */
fbs_attr_config fbs_attr_config_default(void);

typedef struct fbs_attr fbs_attr;

fbs_attr_status fbs_attr_create(const fbs_attr_config *cfg, const fbs_attr_allocator *alloc, fbs_attr **out);
void fbs_attr_destroy(fbs_attr *a);
size_t fbs_attr_memory(const fbs_attr *a);
int fbs_attr_type_of(const fbs_attr *a);

fbs_attr_status fbs_attr_set_base(fbs_attr *a, double base);
double fbs_attr_base(const fbs_attr *a);
/* Cached final value per the contract above (already clamped and rounded for I32). */
double fbs_attr_value(const fbs_attr *a);
fbs_attr_status fbs_attr_value_i32(const fbs_attr *a, int32_t *out);

/* Clamp applied after all channels; lo <= hi, both finite or infinite (never NaN). */
fbs_attr_status fbs_attr_set_clamp(fbs_attr *a, double lo, double hi);
fbs_attr_status fbs_attr_clear_clamp(fbs_attr *a);
int fbs_attr_has_clamp(const fbs_attr *a, double *out_lo, double *out_hi); /* 1 when set */

fbs_attr_status fbs_attr_add(fbs_attr *a, uint16_t channel, int op, double value, fbs_attr_handle *out_handle);
fbs_attr_status fbs_attr_remove(fbs_attr *a, fbs_attr_handle handle);
fbs_attr_status fbs_attr_clear_channel(fbs_attr *a, uint16_t channel); /* OK even when empty */
fbs_attr_status fbs_attr_clear(fbs_attr *a);
unsigned fbs_attr_count(const fbs_attr *a);

typedef struct fbs_attr_modifier {
  fbs_attr_handle handle;
  uint16_t channel;
  int op;
  double value;
} fbs_attr_modifier;

fbs_attr_status fbs_attr_get(const fbs_attr *a, fbs_attr_handle handle, fbs_attr_modifier *out);
/* Sorted by (channel, op, handle). E_TRUNCATED with *count = total when cap is too small. */
fbs_attr_status fbs_attr_list(const fbs_attr *a, fbs_attr_modifier *out, size_t cap, size_t *count);

typedef enum fbs_attr_change {
  FBS_ATTR_CHANGE_BASE = 0,
  FBS_ATTR_CHANGE_ADDED = 1,
  FBS_ATTR_CHANGE_REMOVED = 2,
  FBS_ATTR_CHANGE_CHANNEL_CLEARED = 3,
  FBS_ATTR_CHANGE_CLEARED = 4,
  FBS_ATTR_CHANGE_CLAMP = 5
} fbs_attr_change;

/* Called once per mutating call that changed state (a no-op such as setting the
 * same base does not notify). `previous_value` is the cached value before the
 * change; the new value is fbs_attr_value(a). `handle` is the affected modifier
 * for ADDED/REMOVED, otherwise FBS_ATTR_HANDLE_NONE. */
typedef void (*fbs_attr_observer)(void *user, const fbs_attr *a, double previous_value, int change,
                                  fbs_attr_handle handle);
fbs_attr_status fbs_attr_set_observer(fbs_attr *a, fbs_attr_observer observer, void *user);
/* Increments on every effective change; lets polling hosts (WASM) skip callbacks. */
uint32_t fbs_attr_revision(const fbs_attr *a);

/* Deterministic little-endian schema (docs/decisions/attributes.md section 8):
 * modifiers emitted sorted by (channel, op, handle); next handle persisted. */
size_t fbs_attr_serialized_size(const fbs_attr *a);
fbs_attr_status fbs_attr_serialize(const fbs_attr *a, void *buf, size_t cap, size_t *out_len);
/* cfg NULL: type and base from the blob, capacity = max(default, blob count). With cfg,
 * its type must match the blob (else E_SCHEMA) and its capacity must fit (else E_FULL). */
fbs_attr_status fbs_attr_deserialize(const void *buf, size_t len, const fbs_attr_config *cfg,
                                     const fbs_attr_allocator *alloc, fbs_attr **out);

#ifdef __cplusplus
}
#endif
#endif /* FBS_ATTRIBUTES_H */
