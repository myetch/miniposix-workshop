# MiniPOSIX: UniqueFd and Pipe contract, implementation plan, test plan

Sources read: `ASSIGNMENT.md`, `include/miniposix/unique_fd.hpp`, `include/miniposix/pipe.hpp`,
`src/unique_fd.cpp`, `src/pipe.cpp`, `tests/test_unique_fd.cpp`, `tests/test_pipe.cpp`.

## 1. The `UniqueFd` contract

`UniqueFd` owns at most one file descriptor. The only state is `int fd_ = -1`.

| Member | Required behavior | Detail that's easy to get wrong |
|---|---|---|
| `UniqueFd()` | Empty (`-1`) | Already done (`= default` plus the default member value). |
| `explicit UniqueFd(int fd)` | Takes ownership of `fd` | **Descriptor `0` counts as owned.** Only `-1` means empty. A check like `if (fd_ > 0) close(...)` is wrong, and `DescriptorZeroCanBeReleasedAndClosed` tests exactly this. |
| `~UniqueFd()` | Closes the descriptor if one is owned | It's `noexcept`, so it can't report a failed `close()`. |
| Copy ctor / copy assign | Deleted | Checked at compile time with `static_assert`. |
| Move ctor | Takes `other`'s descriptor and sets `other` to `-1` | Must be `noexcept` (checked by `static_assert`). |
| Move assign | **Closes the descriptor it already owns**, then takes `other`'s | This is the "looks right but leaks" bug the assignment warns about. If you write `fd_ = other.fd_; other.fd_ = -1;`, `get()` returns the right value but the old descriptor stays open. Moving an object into itself must also be safe. |
| `get()` | Returns the descriptor without changing ownership | Already done. |
| `release()` | Returns the descriptor **without closing it** and sets itself to `-1` | Marked `[[nodiscard]]`: ignoring the return value leaks the descriptor. |
| `reset(fd = -1)` | Closes the current descriptor and takes `fd` | **`reset(get())` must do nothing.** A simple version closes the descriptor and then stores the now-closed number, so the object holds a dead descriptor that could be reused by something else and closed later. `reset()` with no argument closes and leaves the object empty. |

The header also says "Do not close an owned descriptor through a raw POSIX call." Calling
`::close(owner.get())` yourself leads to a double close, because the destructor closes it again.

## 2. The `Pipe` contract

- `Pipe::create()` makes one anonymous pipe. It returns a `Pipe` that owns both ends, or throws
  `std::system_error` carrying the `errno` value. **No descriptor may leak on failure.**
- The default constructor is private, so `create()` is the only way to get a `Pipe`.
- Copying is deleted. Moving is `= default`, so it moves each `UniqueFd` member in turn. As a
  result, **`Pipe`'s move assignment is only correct if `UniqueFd`'s move assignment closes the
  old descriptor.** `Pipe` adds no lifetime logic of its own.
- `read_end()` and `write_end()` return references to the owning `UniqueFd` members. Callers use
  `.get()` for I/O and can use `.reset()` to close one end early. `ClosingWriterProducesEndOfFile`
  relies on that.

## 3. POSIX calls involved

| Call | Role | What to check in the documentation |
|---|---|---|
| `close(2)` | Destructor, `reset`, move assign | **Don't retry when it fails with `EINTR` on Linux.** The descriptor is already released, so a retry could close a descriptor another thread just opened (see `man 2 close`, "Dealing with error returns from close()"). In a `noexcept` function, the reasonable choice is to ignore the result. |
| `pipe(2)` / `pipe2(2)` | `Pipe::create` | On failure, POSIX says the array is not modified and no descriptors are created, so there's nothing to clean up. `pipe2(fds, O_CLOEXEC)` is Linux-specific (the header says "Linux pipe") and stops the descriptors leaking into child processes after `exec`. |
| `errno` | Error reporting | Read it **immediately** after the failing call, before anything else can overwrite it. |
| `std::system_error(errno, std::system_category(), "pipe2")` | The exception | The test checks `error.code().value() == EMFILE`. On POSIX, `system_category` values are `errno` values. |
| `fcntl(fd, F_GETFD)` | Used by tests only | Returns `-1`/`EBADF` if `fd` isn't open. This is how the tests observe the real descriptor state rather than the wrapper's stored value. |
| `read`/`write`, `sigaction(SIGPIPE)`, `setrlimit(RLIMIT_NOFILE)` | Used by tests only | Byte transfer, end-of-file, and forcing `EMFILE` by setting the soft limit to 0. |

## 4. Assumptions about ownership and error paths

1. **Negative values other than `-1`.** The contract says "-1 means empty; every other value is
   owned", so `UniqueFd{-5}` would count as owning `-5`. Assume callers only pass `-1` or a valid
   descriptor. The guard before `close` can be `fd_ != -1` (the literal contract) or `fd_ >= 0`
   (defensive). Either way, closing `-5` only produces a harmless `EBADF`. Worth asking the
   instructor which one is intended.
2. **`close()` failures are swallowed.** A `noexcept` destructor or `reset` has no way to report
   them, and that's the normal trade-off for RAII handles. If close errors matter (for example,
   delayed write errors on NFS), the caller has to `release()` and call `close` themselves.
3. **No thread safety.** A single `UniqueFd` can't be used from several threads at once, just like
   `std::unique_ptr`.
4. **Nothing can throw after `pipe2` succeeds.** `UniqueFd(int)` and the moves are `noexcept`, so
   once the call returns, storing both descriptors can't fail halfway. That's why "no leak on
   failure" only needs a check of the system call's result.
5. **`O_CLOEXEC` is a choice, not a requirement.** No test checks descriptor flags. Plain `pipe()`
   would also pass the tests.
6. **The `is_open` check in the tests has a limit.** If a descriptor number is closed and then
   reused by something else, `fcntl` reports it as open. In these single-threaded tests that would
   cause a false failure, not hide a leak, but it's worth knowing when investigating a failure.

## 5. Implementation plan

**`src/unique_fd.cpp`**
- Constructor: `fd_(fd)` in the initializer list.
- Destructor: `if (fd_ != -1) ::close(fd_);`
- Move ctor: `fd_(std::exchange(other.fd_, -1))`
- Move assign: `reset(other.release()); return *this;`. This is safe for self-move without a
  separate `this != &other` check: `release()` empties `*this` first, so `reset` finds nothing to
  close and simply takes the descriptor back. An explicit check is still fine if it reads more
  clearly.
- `release()`: `return std::exchange(fd_, -1);`
- `reset(int fd)`: `if (fd == fd_) return; const int old = std::exchange(fd_, fd); if (old != -1) ::close(old);`.
  Swapping first and closing second means the object never holds a closed descriptor.
- Add `#include <utility>`.

**`src/pipe.cpp`**
```cpp
int fds[2];
if (::pipe2(fds, O_CLOEXEC) == -1)
    throw std::system_error(errno, std::system_category(), "pipe2");
Pipe p;
p.read_end_.reset(fds[0]);
p.write_end_.reset(fds[1]);
return p;
```
This needs `<fcntl.h>`. `pipe2` is a GNU extension, and `CMAKE_CXX_EXTENSIONS OFF` gives
`-std=c++23`, but glibc's `<unistd.h>` still declares `pipe2` because g++ and clang++ define
`_GNU_SOURCE` for C++ by default. Check that on the first build. If it doesn't compile, use plain
`pipe()` (or add `_GNU_SOURCE`) rather than changing the language standard.

## 6. Test plan

**Order of runs** (commands per README): configure the `clang22-debug` preset, build, then run the
`smoke` label and the `functional` label separately. Initially smoke should pass and functional
should fail.

**What the existing tests cover:**
- `UniqueFd`: empty by default, not copyable, `noexcept` moves, descriptor 0, the destructor
  closes, `release` doesn't close, `reset` closes the old descriptor, `reset(get())` does nothing,
  move construction, move assignment closing the old descriptor, self-move.
- `Pipe`: real byte transfer, both ends closed at destruction, end-of-file once the writer is
  closed, move construction, and `EMFILE` reported through `system_error`.

**Tests to add** (each checks with `fcntl`, not only `get()`):
1. **Pipe move assignment** (the stretch task): create `dst` and `src`, record `dst`'s two
   descriptors, run `dst = std::move(src)`, then assert the old descriptors are closed, `dst`
   holds `src`'s old descriptors, and `src` holds `-1`/`-1`. If you only compare `get()` values, a
   move assignment that overwrites without closing still passes, and the leak goes unnoticed.
2. `release()` on an empty object returns `-1`, and `reset()` on an empty object doesn't crash.
3. Move construction and move assignment from an empty source leave the target empty, and the
   target's old descriptor is still closed.
4. Destroying an object after `release()` leaves the descriptor open (partly covered already).
5. Failure path doesn't leak: count entries in `/proc/self/fd` before the forced-`EMFILE`
   `create()` and again after the limit is restored, then compare.
6. Only if `O_CLOEXEC` is used: `fcntl(fd, F_GETFD) & FD_CLOEXEC` is set on both ends.

**Additional checks outside unit tests:** ASan/UBSan don't detect descriptor leaks. To check the
whole process, run a test binary under `valgrind --track-fds=yes`, or list `/proc/<pid>/fd` at exit.

## 7. Claims to verify for the attempt log

- "Don't retry `close` on `EINTR` on Linux": see `man 2 close`.
- "`pipe` leaves `fildes` unmodified and creates no descriptors on error": see the POSIX `pipe()`
  page, "Return value" section.
- "`system_category()` values on POSIX are `errno` values": see the cppreference page for
  `std::system_category`.
- "`pipe2` is declared under `-std=c++23` without extra macros": check on the first build.
  **Confirmed (2026-11-17):** `src/pipe.cpp` builds with clang++-22, `-std=c++23`, no extra macros,
  no warnings.

## 8. Progress log (2026-11-17, branch `implement-miniposix`)

### Done

1. **`src/unique_fd.cpp` implemented** as in section 5:
   - ctor `fd_(fd)`; dtor closes when `fd_ != -1` (descriptor 0 included), never retries `close`.
   - move ctor and `release()` use `std::exchange(fd_, -1)`.
   - move assign is `reset(other.release())`; safe for self-move without a separate check.
   - `reset(fd)` returns early when `fd == fd_`; otherwise stores the new fd first, then closes the old one.
   - Result: `unique_fd` tests 7/7 passed; `pipe` still 1/6 (stub `create()` returned empty `Pipe`,
     so reader/writer were `-1` and no `system_error` was thrown).
2. **`src/pipe.cpp` implemented**:
   - `pipe2(fds, O_CLOEXEC)`; on `-1` throws `std::system_error(errno, std::system_category(), "pipe2")`.
   - On success, `Pipe pipe; pipe.read_end_.reset(fds[0]); pipe.write_end_.reset(fds[1]); return pipe;`
     (everything after `pipe2` is `noexcept`, so no leak).
   - Result: smoke 1/1, `unique_fd` 7/7, `pipe` 6/6. No compiler warnings.

### Attempt log (for ASSIGNMENT.md table)

| Attempt | Failing test or observed behavior | Help needed? | AI mistake or uncertain claim | Evidence and reason for the correction |
| --- | --- | --- | --- | --- |
| 1 | After `UniqueFd` only: `pipe` 5/6 failed (`reader >= 0` was `-1`; no `system_error` on `EMFILE`) | No, expected: `Pipe::create()` was still a stub | Uncertain: does `pipe2` compile under `-std=c++23` with `CMAKE_CXX_EXTENSIONS OFF`? | Built cleanly with clang++-22; clang/g++ define `_GNU_SOURCE` for C++ |
| 2 | All functional tests pass after implementing `Pipe::create()` | No | Claims still to check in docs: `close` + `EINTR` on Linux; `pipe` creates no fds on error | See section 7 |

### How to build and test

One command, from `/work`:

```bash
cmake --preset clang22-debug && cmake --build --preset clang22-debug --parallel && ctest --preset clang22-debug-smoke --no-tests=error && ctest --preset clang22-debug-functional --no-tests=error
```

Other forms:

```bash
ctest --test-dir build/clang22-debug --output-on-failure        # every test
./build/clang22-debug/test_unique_fd                            # one test program
./build/clang22-debug/test_pipe --gtest_filter='Pipe.Moving*'   # a single test case
```

`ctest` doesn't rebuild; build first.

### Still to do (from ASSIGNMENT.md)

**Audit A**
- [ ] Optionally add tests from section 6 (Pipe move assignment closes old fds, empty-object cases,
      `/proc/self/fd` leak check on failure, `FD_CLOEXEC` check)
- [ ] Fill in the familiarity table (0–3) and the attempt log above in team notes
- [ ] Verify at least one claim against POSIX/C++ docs (section 7) and record it
- [ ] Commit, push, open a PR **to `main` in your own fork**, inspect the Actions result.
      Uncommitted right now: `README.md` (modified), `Dockerfile`, `.dockerignore`, `notes/`,
      `src/unique_fd.cpp`, `src/pipe.cpp`.

**Audit B** (library works now)

See section 9 for the benchmark command and the hot-path analysis.

- [ ] Answer the four questions (best median buffer size and stability; what the timing includes and
      why it's not `UniqueFd` alone; what changes on other machines / `perf stat` counters; what to
      change before claiming one variant is faster). The benchmark **appends** to the CSV, so use a
      fresh path per run.

**Afternoon exercise (35 min)**
- [ ] Same POSIX question in two separate AI conversations; save both answers
- [ ] Documentation URL, focused test command + result
- [ ] JSON evidence record: `claim`, `source_url`, `test_command`, `observed`, `verdict` (human sets
      accept / revise / reject)
- [ ] Workflow sketch or script that stops before the PR for the human decision

**Optional / stretch**
- [ ] I/O helper (EOF, `EINTR`, short transfers): no stub exists, would be new code
- [ ] Pipe move-assignment test checking old descriptors are closed with a POSIX call
- [ ] Benchmark rerun with ≥10 repeats to a fresh CSV; compare median and spread; list what the CSV
      doesn't record (CPU, load, OS, compiler)

**Final discussion**
- [ ] Show PR, local test results, attempt log, CSV/plot analysis, evidence record and workflow;
      sketch one extension (subprocess or event-driven) and its new ownership/error cases

## 9. Benchmark: generating CSV and plot, hot-path analysis (2026-11-17)

### Generate a new CSV and plot

Paste all lines at once (each ends with `&& \`, so it's one command). If you copy only part of
it, you get errors like `bash: clang22-perf: command not found` and `$RUN` stays unset.

```bash
RUN=bench-results/$(date +%Y%m%d-%H%M%S) && mkdir -p "$RUN" && \
cmake --preset clang22-perf && \
cmake --build --preset clang22-perf --parallel && \
./build/clang22-perf/bench_pipe --repeats 10 --output "$RUN/pipe.csv" && \
uv sync --project analysis --locked && \
uv run --project analysis --locked python analysis/analyze.py "$RUN/pipe.csv" --plot "$RUN/throughput.png"
```

- Output: `bench-results/<timestamp>/pipe.csv` (one row per repeat) and `throughput.png`;
  a summary grouped by variant and buffer size is printed. `echo $RUN` shows the folder.
- `bench_pipe` **appends** to its CSV, so the timestamped folder keeps runs separate.
- Options: `--repeats N` (default 3), `--bytes N` (default 8 MiB; 64 MiB = `67108864` gives less
  noise), `--buffer-sizes 512,4096,65536,262144,1048576`.
- `analysis/analyze.py` groups by the CSV's `variant` column, so optimized variants will appear
  as separate lines on the same plot.

### Baseline measured by Claude (container, 10 CPUs, clang22-perf, 64 MiB per run, 5 repeats)

| Buffer | Throughput | Spread |
|---|---|---|
| 512 B | ~600 MiB/s | ±5 |
| 4096 B | ~900 MiB/s | ±15 |
| 65536 B | ~1010 MiB/s | ±25 |

`perf` is not installed in the container; `strace` was used instead.

### Where the time goes

1. **Library code is not on the hot path.** `Pipe::create()` runs once, before the timer starts;
   `.get()` once per thread; `reset()` once when the writer finishes. Nothing in `UniqueFd`/`Pipe`
   runs per chunk, so micro-optimizing them gains nothing measurable. (Answers Audit B Q2.)
2. **System calls:** `strace -f -c` shows exactly `total / buffer` reads and writes, no short
   transfers (16,386 writes at 4 KiB, 1,026 at 64 KiB). Fixed per-call cost explains why 512 B is slowest.
3. **Per-byte fill/verify** (`byte_at(offset) = offset % 251`): timed alone, ~4.7 GB/s fill and
   ~4.5 GB/s verify, i.e. roughly 20% of each thread's time at ~1 GB/s throughput.
4. **The rest:** kernel copies into/out of the 64 KiB pipe buffer, and reader/writer threads
   blocking and waking each other.

### Optimization options (not implemented yet)

| # | Change | Where | Expected effect | Catch |
|---|---|---|---|---|
| 1 | Precompute the 251-byte pattern; fill with `memcpy`, verify with `memcmp` | benchmark | Removes most of the ~20% per-thread cost | Changes the benchmark; record as new variant |
| 2 | `fcntl(fd, F_SETPIPE_SZ, 1 MiB)` + buffers 256 KiB–1 MiB | benchmark (or new optional `Pipe` function) | Fewer syscalls and block/wake cycles | Linux-only; capped by `/proc/sys/fs/pipe-max-size` |
| 3 | Pin reader/writer threads to CPUs | benchmark | Often large, and less variance | Very machine-dependent |
| 4 | `vmsplice`/`splice` zero-copy | benchmark | Can be much faster | Different experiment, no longer plain `read`/`write` |
| ✗ | Micro-optimize `UniqueFd` | library | None | Explain why in notes instead |

### Recommendation

1. Confirm with the lecturer what "optimize the hot paths" refers to (likely the benchmark loop;
   the public headers fix the library signatures).
2. Keep the original benchmark as the baseline; add each optimization as a separate `variant`
   (e.g. behind a `--variant` option).
3. Start with #1 and #2; run every variant with ≥10 repeats into a fresh CSV; compare medians and
   spread. ±25 MiB/s is already the noise level at 64 KiB.
4. Record why `UniqueFd` isn't the bottleneck, using the counts above.

## 10. Hot-path optimization implemented: `table` variant (2026-11-17)

**Change** (`benchmarks/bench_pipe.cpp`): new option `--variants baseline,table` (default `baseline`,
so the original command behaves as before). `table` precomputes the repeating 251-byte pattern
once and fills with `memcpy` / verifies with `memcmp` instead of computing `offset % 251` per byte.
Same bytes, same `read`/`write` calls, same verification. Variants alternate within each repeat.
CSV variant names: `miniposix_pipe` (baseline, unchanged) and `miniposix_pipe_table`.

**Command:**

```bash
RUN=bench-results/$(date +%Y%m%d-%H%M%S)-variants && mkdir -p "$RUN" && \
cmake --build --preset clang22-perf --parallel && \
./build/clang22-perf/bench_pipe --bytes 67108864 --repeats 1 --variants baseline,table --output "$RUN/warmup.csv" && \
./build/clang22-perf/bench_pipe --bytes 268435456 --repeats 10 --variants baseline,table --output "$RUN/pipe.csv" && \
uv run --project analysis --locked python analysis/analyze.py "$RUN/pipe.csv" --plot "$RUN/throughput.png"
```

**Result** (`bench-results/20261117-094747-variants/`, 256 MiB × 10 repeats, after a discarded warm-up):

| Buffer | baseline median | table median | Speed-up | table std |
|---|---|---|---|---|
| 512 B | 607 MiB/s | 1580 MiB/s | ~2.6× | 117 |
| 4096 B | 908 MiB/s | 6840 MiB/s | ~7.5× | 1167 |
| 65536 B | 1019 MiB/s | 3821 MiB/s | ~3.8× | 1301 |

**Interpretation:** the per-byte fill/verify loops were the bottleneck, not the pipe and not
`UniqueFd`. With them removed, the pipe itself (kernel copies, scheduling) dominates, and the
spread became much larger (min 1170 / max 5707 MiB/s at 64 KiB), so the 4 KiB-vs-64 KiB ordering
for `table` is not yet reliable. Possible causes to test: cache size (two 64 KiB buffers + 64 KiB
pipe buffer), thread placement on CPUs. Single machine, single session; not a general claim.

### Stock benchmark vs hot-path optimized (2026-11-17)

- `pipe-long.csv` and `throughput-long.png` were deleted on request (no code referenced them).
- Stock = `benchmarks/bench_pipe.cpp` exactly as committed in `HEAD` (no edits), built separately
  to `build/stock/bench_pipe_stock` with the clang22-perf flags (`-O2 -g -DNDEBUG -fno-omit-frame-pointer -std=c++23`)
  and the same `libminiposix.a`. Optimized = edited benchmark with `--variants table`.
- Script: discarded warm-up of each, then 10 rounds alternating which program runs first,
  256 MiB per measurement. Variants renamed in the CSV to `stock_benchmark` / `hotpath_optimized`.
- Results in `bench-results/20261117-095039-stock-vs-hotpath/`:

| Buffer | stock median (std) | optimized median (std) | Speed-up |
|---|---|---|---|
| 512 B | 609 (8) | 1517 (179) | ~2.5× |
| 4096 B | 907 (2) | 6971 (695) | ~7.7× |
| 65536 B | 1016 (15) | 3685 (891) | ~3.6× |
