# fbs-attributes

Numeric game stats such as health or damage, with channel-ordered stacked modifiers
and explicit override precedence, in C99.

## What it does

An `fbs_attr` holds one base value and a list of modifiers. You add a modifier with `fbs_attr_add(attr, channel, op, value, &handle)`, take it off later with `fbs_attr_remove(attr, handle)`, and read the result with `fbs_attr_value(attr)`. The value is cached and recomputed on every change.

- Five operations: `FBS_ATTR_ADD`, `FBS_ATTR_MUL_ADD` (summed into one multiplier), `FBS_ATTR_MUL_COMPOUND` (each one multiplies), `FBS_ATTR_ADD_FINAL` (added after the multipliers) and `FBS_ATTR_OVERRIDE`.
- Channels are `uint16_t` numbers evaluated in ascending order, so you can layer "gear" before "buffs". Per channel the value becomes `(v + sum ADD) * (1 + sum MUL_ADD) * product(1 + MUL_COMPOUND) + sum ADD_FINAL`, or the channel's OVERRIDE value if it has one.
- An optional clamp is applied after all channels (`fbs_attr_set_clamp`). The type is `FBS_ATTR_F64`, `FBS_ATTR_F32` (result rounded through `float`) or `FBS_ATTR_I32` (rounded half away from zero; `fbs_attr_value_i32` returns it as `int32_t`).
- Handles are 64-bit, never reused, and saved with the attribute, so a handle stored by an item or buff still works after a save and load.
- `fbs_attr_clear_channel` and `fbs_attr_clear` drop groups of modifiers. `fbs_attr_get` and `fbs_attr_list` read them back.
- `fbs_attr_set_observer` installs a callback that runs once per effective change with the previous value and the kind of change. `fbs_attr_revision` is a counter for hosts that poll instead.
- `fbs_attr_serialize` and `fbs_attr_deserialize` write and read a versioned little-endian blob. Malformed blobs return `FBS_ATTR_E_SCHEMA`.

## When to use it

- Stats where buffs, gear and auras come and go in any order and you need the same number every time, for example lockstep multiplayer or saves that must reload to the identical value.
- You need to undo one specific modifier exactly. `tests/test_attributes.c` (A-T6) checks that adding then removing a modifier restores the previous value bit for bit.

## When not to use it

- One `fbs_attr` is one number. There are no attribute sets, no attributes computed from other attributes, no durations or timers, and no per-source tags: you track which handles belong to which item or effect.
- Capacity is fixed at creation (`max_modifiers`, from 1 to 1<<20). `fbs_attr_add` returns `FBS_ATTR_E_FULL` when it is reached.
- Every change re-evaluates all modifiers (sorting each bucket by value), and `fbs_attr_remove` and `fbs_attr_get` find a handle by linear scan. No benchmark ships with this repository.
- If one channel holds several OVERRIDEs, the lowest handle (the earliest added) wins, so that case does depend on insertion order.
- For I32 attributes the rounding happens after the clamp, so use integer clamp bounds: a clamp of [0.5, 0.7] gives 1.
- Bit-identical results require `FLT_EVAL_METHOD == 0` (x86-64 with SSE2, AArch64, wasm32). The source emits a `#warning` otherwise; 32-bit x87 builds need `-mfpmath=sse`.
- There is no internal locking (see Design notes).

## Example

```c
#include <fbs/attributes.h>
#include <stdio.h>

int main(void) {
  fbs_attr_config cfg = fbs_attr_config_default(); /* F64, base 0, 32 modifiers */
  fbs_attr *health = NULL, *loaded = NULL;
  fbs_attr_handle sword = FBS_ATTR_HANDLE_NONE, rage = FBS_ATTR_HANDLE_NONE;
  unsigned char blob[256];
  size_t len = 0;

  cfg.base = 100.0;
  if (fbs_attr_create(&cfg, NULL, &health) != FBS_ATTR_OK) return 1;
  /* Channel 0: flat bonuses. Channel 1: multipliers applied to the channel 0 result. */
  if (fbs_attr_add(health, 0, FBS_ATTR_ADD, 20.0, &sword) != FBS_ATTR_OK ||
      fbs_attr_add(health, 1, FBS_ATTR_MUL_ADD, 0.5, &rage) != FBS_ATTR_OK ||
      fbs_attr_set_clamp(health, 0.0, 999.0) != FBS_ATTR_OK) {
    fbs_attr_destroy(health);
    return 1;
  }
  printf("sword and rage: %g\n", fbs_attr_value(health)); /* (100 + 20) * 1.5 = 180 */

  if (fbs_attr_remove(health, rage) != FBS_ATTR_OK) { fbs_attr_destroy(health); return 1; }
  printf("rage expired: %g\n", fbs_attr_value(health)); /* 120 */

  /* Save and load: the clamp and the sword's handle survive. */
  if (fbs_attr_serialize(health, blob, sizeof blob, &len) != FBS_ATTR_OK ||
      fbs_attr_deserialize(blob, len, NULL, NULL, &loaded) != FBS_ATTR_OK) {
    fbs_attr_destroy(health);
    return 1;
  }
  fbs_attr_destroy(health);
  if (fbs_attr_remove(loaded, sword) != FBS_ATTR_OK) { fbs_attr_destroy(loaded); return 1; }
  printf("after load, sword unequipped: %g\n", fbs_attr_value(loaded)); /* 100 */
  fbs_attr_destroy(loaded);
  return 0;
}
```

Build it with `add_executable(demo main.c)` and `target_link_libraries(demo PRIVATE fbs::attributes)`.

## Build and test

Run from this repository's root. In addition to CMake and the compiler named
below, install the build tool selected by your generator (for example Make or
Ninja).

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --parallel 1
(cd build && ctest --output-on-failure)
```

This builds the library (`fbs::attributes`), the test program `fbs_test_attributes` (ctest name `attributes`) and `fbs_attributes_example` from `examples/basic.c` (ctest name `attributes_example`), which creates an attribute and prints the API version. `-DFBS_BUILD_TESTS=OFF` and `-DFBS_BUILD_EXAMPLES=OFF` skip them.

`tests/test_attributes.c` needs no test framework. It checks the evaluation formula and channel order, insertion-order independence, exact removal, override tie-breaks, I32 rounding and range, F32 rounding, clamping, observer notifications and the reentrancy guard, destroying from inside an observer, handle stability across save and load, rejection of corrupted blobs, allocator failure, capacity limits, every `FBS_ATTR_E_TRUNCATED` path and invalid arguments on every entry point. It also compares serialization against the committed golden file `tests/fixtures/attributes/attr.bin`.

Requirements: CMake 3.16 or newer, a C99 compiler and libm (linked automatically except with MSVC). No third-party code is vendored; `third_party/piperift/` holds license and notice files only.

With FetchContent (or `add_subdirectory` on a checkout):

```cmake
include(FetchContent)
set(FBS_BUILD_TESTS OFF)
set(FBS_BUILD_EXAMPLES OFF)
FetchContent_Declare(fbs_attributes
  GIT_REPOSITORY https://github.com/finalbuildgames-com/fbs-attributes.git
  GIT_TAG <full-commit-hash>) # pin a reviewed commit
FetchContent_MakeAvailable(fbs_attributes)
target_link_libraries(your_target PRIVATE fbs::attributes)
```

This repository ships the C library only. No engine bindings or adapters are included.

## Build modes and installation

`BUILD_SHARED_LIBS=ON` builds a shared library; the default is static.
`FBS_BUILD_TESTS` and `BUILD_TESTING` together enable the core test.
`FBS_BUILD_EXAMPLES` controls `fbs_attributes_example`; its CTest entry also requires
`BUILD_TESTING`. For a library-only build, set `FBS_BUILD_TESTS=OFF` and
`FBS_BUILD_EXAMPLES=OFF`.

```sh
cmake --install build --prefix "$PWD/install"
```

Installation supplies [the public header](include/fbs/attributes.h), the library,
license notices and `FinalBuildAttributesTargets.cmake` under
`${CMAKE_INSTALL_LIBDIR}/cmake/FinalBuildAttributes`. It supplies no package config or
version config, so `find_package(FinalBuildAttributes)` is unavailable. A consumer may
include the installed targets file explicitly and link `fbs::attributes`, or use
the source integration above. The [minimal program](examples/basic.c) and
[core tests](tests/test_attributes.c) show the implemented entry points.

## Design notes

- **Determinism.** Inside each (channel, op) bucket, sums and products are taken in ascending (value, handle) order, so ordinary modifier buckets depend only on the set of (channel, op, value) triples. Multiple OVERRIDEs remain the documented exception: the earliest handle wins. `tests/test_attributes.c` checks this on all 720 insertion orders of four 6-modifier sets, including sets where float addition order changes the result (A-T2), on 400 random sets built in 8 random orders each, and in 36 randomized rounds of 200 operations compared bit for bit against an independent reference evaluator for F64, F32 and I32 (A-T15).
- **Serialization.** Modifiers are written sorted by (channel, op, handle), and the next handle is stored. The blob is 28 bytes, plus 16 if a clamp is set, plus 24 per modifier. Re-serializing a loaded blob gives the same bytes (A-T13). The observer and the revision counter are not saved.
- **Memory.** `fbs_attr_create` and `fbs_attr_deserialize` make one allocation sized by `max_modifiers`; nothing else is allocated afterwards (`test_allocator`). Pass an `fbs_attr_allocator` (`alloc`, `free`, `user`) or `NULL` for `malloc`/`free`. `fbs_attr_memory` returns the block size. `fbs_attr_serialize` writes into your buffer; `fbs_attr_serialized_size` tells you how big it must be.
- **Threading.** No globals or static mutable state, so separate attributes are independent. There is no locking, so guard a shared attribute yourself.
- **Observers.** Called synchronously after each effective change; a no-op (same base, empty clear) does not notify. Mutating the attribute from inside its observer returns `FBS_ATTR_E_REENTRANT`. Calling `fbs_attr_destroy` from inside it is allowed; the free happens after the observer returns.
- **Errors.** Functions return `fbs_attr_status`; `fbs_attr_status_name` turns a code into a string. NULL pointers, NaN or infinite values and unknown enums give `FBS_ATTR_E_INVALID`. On error, outputs are left untouched, except `FBS_ATTR_E_TRUNCATED`, which writes the required count or length.
- **ABI.** `fbs_attr` is opaque and the header uses `extern "C"` under C++. `FBS_ATTR_VERSION` and `fbs_attr_version()` report 100. Blobs carry schema version 1, checked on load.

## License

MIT for Final Build Games' original code; see [LICENSE](LICENSE).

`src/attributes.c` is derived from Piperift's [AttributesExtension](https://github.com/PipeRift/AttributesExtension) at commit `d479b32106d403ac1429287d981df123d15d8669` (Apache-2.0, Copyright 2015-2026 Piperift), rewritten in C with changed semantics. Those derived portions remain under Apache-2.0; the MIT license does not replace it. See [THIRD_PARTY_NOTICES](THIRD_PARTY_NOTICES) and [third_party/piperift/](third_party/piperift/).
