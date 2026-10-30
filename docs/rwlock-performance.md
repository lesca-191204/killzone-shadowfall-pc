# Experimental sharded rwlock registry

`GOW3_RWLOCK_FAST=1` selects an alternative rwlock metadata backend. Unset or `0`
keeps the existing backend. The selected backend is printed once during import
resolution, and the experimental counters have a `sharded-256` suffix.

The native pthread rwlock operations and guest API remain the same. This changes
the registry/ownership bookkeeping around them:

- Hash validated handle values across 256 independent mutex-protected registries.
  An invalid handle is hashed, then checked for membership before dereferencing.
- Pin objects while native lock acquisition runs outside the metadata mutex.
  Destruction rejects objects with owners or in-flight operations.
- Preserve recursive reads, writer ownership, timed/try errors, and cross-thread
  read unlocks. The latter is required by Killzone's asset streamer.
- Serialize only zero-handle initialization through a separate global mutex.
- Retain at most one spare holder record per object, released on destruction,
  to reduce allocation churn. Copied handle values continue to use the same shard.

The stock backend validates each handle by scanning one global linked list under
one mutex. Each acquisition enters that mutex twice and each release once.
A Chapter 5 CPU profile on Windows showed the main thread and five job workers
repeatedly at those registry-mutex and native-rwlock call sites. The session had
about 3,100 rwlock objects and millions of lock operations.

## Correctness tests

Build `rwlock-isolated-test`, or compile the three files directly with Clang/GCC:

```sh
cc -std=c11 -O2 -g -UNDEBUG -Wall -Wextra -Werror -pthread -Isrc \
  tests/test_rwlock_isolated.c src/runtime_rwlock.c src/runtime_rwlock_fast.c \
  -o out/rwlock-isolated-test
GOW3_RWLOCK_FAST=0 out/rwlock-isolated-test
GOW3_RWLOCK_FAST=1 out/rwlock-isolated-test
GOW3_RWLOCK_FAST=0 out/rwlock-isolated-test --report-first
GOW3_RWLOCK_FAST=1 out/rwlock-isolated-test --report-first
```

On Windows use the MSYS2 CLANG64 toolchain; the output has an `.exe` suffix.
The CMake target also works there:

```powershell
cmake --build out/gpu --target rwlock-isolated-test
$env:GOW3_RWLOCK_FAST='0'
./out/gpu/rwlock-isolated-test.exe
$env:GOW3_RWLOCK_FAST='1'
./out/gpu/rwlock-isolated-test.exe
```

The standalone test retains the existing runtime rwlock lifecycle, concurrency,
cross-thread-release and timeout cases. It adds copied/stale handles, concurrent
lazy initialization, shared reader/writer stress, acquisition versus destruction,
and report initialization before resolving any imports.

`--bench` runs six threads with independent locks and 3,105 registered objects.
It intentionally stresses registry lookup/contention. On an i7-12700K, the same
60,000 read/unlock pairs took 1.18-1.20 seconds with the stock backend and
0.020-0.024 seconds with sharding. This is not an FPS prediction.

## Initial gameplay result

One Windows A/B trial used a v0.6.0-based executable containing both backends:
i7-12700K, RTX 5070 Ti, CUSA00190 update 01.81, Chapter 5, FSR 3.1 reconstructing
1920x1080 to 3840x2160, frame generation off, Unlimited, identical saved settings.
The player ran the stock backend first, then the sharded backend.

The final six frame-statistics windows in each run (approximately 30 seconds)
give:

| Reported window metric | Stock | Sharded |
| --- | ---: | ---: |
| Mean FPS | 30.6 | 93.8 |
| FPS range | 27.5-33.1 | 83.6-111.9 |
| Mean command-processor idle time | 68.8% | 2.9% |
| Median of window median frame intervals | 31.42 ms | 11.05 ms |
| Mean draws per frame | 2,150.5 | 1,931.8 |

These are summaries of the printed windows, excluding earlier startup/menu/loading
samples by using the same final-six-window rule. They are not a whole-run FPS
average. Command idle is a CPU-thread submission wait, not hardware GPU usage.
Camera positions were not recorded and workload varied: the selected patched
samples had about 10% fewer draws per frame. Cache/startup behavior also differed
between the first and second runs. The approximately 3.1x FPS ratio is an initial
result for this section, not a guaranteed gain elsewhere.

Both runs exited normally and the player reported normal movement/animation speed
and no observed problems. CPU feature detection, Vulkan submission/readback, and
GPU initialization plus game-import resolution (zero missing imports) passed on
the local test build. The source-only PR also initializes metadata before an
early report; the standalone test covers that additional report-only guard.

Longer campaign testing, checkpoint reloads, chapter transitions, reverse-order
A/B runs and other hardware/OS testing remain open. The backend stays opt-in
while those are reviewed. No game assets or raw player logs are included.
