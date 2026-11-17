# MiniPOSIX workshop

A small Linux C++23 project for the BIP workshop **AI for Software Engineering and Data — Coding, Analysis and Intelligent Workflows** (17 November 2026). You will use an AI coding agent, check its work with tests and documentation, then collect and examine a simple I/O benchmark. The two workshop blocks are 09:15–11:30 and 12:00–13:40.

No previous POSIX experience is required. A **file descriptor** is an integer handle to an open operating-system resource. A **pipe** connects a write end to a read end so bytes can flow between them. The starter deliberately leaves the main library functions unfinished; see [ASSIGNMENT.md](ASSIGNMENT.md) for the contract and activities.

## What you need

- Linux. On university Windows computers, use the preconfigured Ubuntu virtual machine below. On your own computer, Ubuntu 24.04 LTS is recommended.
- LLVM/Clang 22.x as `clang-22` and `clang++-22`, with C++23 support. The project presets use these executable names explicitly. On Ubuntu, follow the [official LLVM APT instructions](https://apt.llvm.org/) for your release if they are missing. A generic `clang++` command may select a different version.
- CMake 3.28 or newer, Ninja, Git, and GoogleTest development files. On Ubuntu, install the test dependency with `sudo apt install libgtest-dev`.
- [uv](https://docs.astral.sh/uv/getting-started/installation/) and Python 3.12 or newer for CSV analysis. `uv` manages the analysis environment locally and can install a compatible Python; see [analysis/README.md](analysis/README.md).
- [OpenCode](https://opencode.ai/docs/) for the agent exercise. Use an available model; the workshop does not require a particular paid model or API key.
- On university computers, configure CLion in the Ubuntu virtual machine as described below. The project can also be completed from a terminal.

## On university Windows computers

1. In Windows, open **Oracle VirtualBox**. Select the virtual machine labeled **Ubuntu 26.06 LTS AI** and start it. This is the name shown in VirtualBox.
2. Open a terminal and CLion **inside the Ubuntu virtual machine**. Clone the repository and run all build, test, agent, and analysis commands there.
3. In the Ubuntu terminal, check `command -v clang-22 clang++-22`. Both commands should resolve to `/usr/bin`.
4. In CLion, open **Settings → Build, Execution, Deployment → Toolchains** and add or select a local toolchain. Set **C Compiler** to `/usr/bin/clang-22` and **C++ Compiler** to `/usr/bin/clang++-22`, then apply the settings.
5. In **Settings → Build, Execution, Deployment → CMake**, enable the `clang22-debug` preset if it is disabled. Select that preset before building.

## Without CLion: Docker (terminal only)

The [Dockerfile](Dockerfile) builds an Ubuntu 26.04 image with Clang 22, CMake, Ninja, GoogleTest, uv, and OpenCode. Docker Desktop on macOS or Windows, or Docker Engine on Linux, is enough; no GUI or IDE is needed. From the repository root:

```bash
docker build -t miniposix-workshop .
docker run -it --name miniposix -v "$PWD":/work miniposix-workshop
```

Run every build, test, agent, and analysis command inside that container shell; the repository is mounted at `/work`, so edits are visible on both sides. Re-enter later with `docker start -ai miniposix`. Keep building inside the container only, because `build/` caches paths from the container. The benchmark plot is saved as a PNG in `bench-results/`; open it on the host. Under Docker Desktop the benchmark measures a Linux VM, so record that in your notes, and `perf stat` is usually unavailable there.

Check your tools before starting:

```bash
command -v clang-22 clang++-22 cmake ninja git uv
clang-22 --version
clang++-22 --version
cmake --version
uv --version
```

If LLVM is installed outside `/usr/bin`, put the directory containing both compiler executables on `PATH` **before** configuring CMake. For example, run `export PATH="/path/to/llvm/bin:$PATH"`, then confirm both paths with `command -v clang-22 clang++-22`. In CLion, set the C and C++ compiler paths in **Settings → Build, Execution, Deployment → Toolchains** and make sure CLion also inherits that `PATH` before choosing the matching CMake preset. The preset fixes the compiler for terminal and agent runs; changing only the CLion toolchain is insufficient.

## Work in your team's fork

One fork per team of two or three is recommended. Replace the URL with your team's fork URL copied from GitHub.

1. Select **Fork** on the workshop repository. In your fork, open **Actions** and enable workflows if GitHub asks you to do so.
2. Clone your fork and create a branch:

   ```bash
   git clone https://github.com/YOUR-TEAM/miniposix-workshop.git
   cd miniposix-workshop
   git switch -c implement-miniposix
   ```

3. Start OpenCode in this directory with `opencode`. Ask it first to explain the API, POSIX operations, and a test plan. Review and approve every command it proposes. Read the changes it makes; use POSIX/C++ documentation and local tests to check its claims.
4. Build and test locally (commands below). It is expected that the smoke test passes while the functional tests fail on the untouched starter.
5. After you understand and verify your changes, commit and push your branch:

   ```bash
   git add include src tests
   git status --short
   git commit -m "Implement MiniPOSIX exercise"
   git push -u origin implement-miniposix
   ```

   Add any other public files you intentionally changed. Do not commit credentials, private notes, or generated benchmark results.
6. On GitHub, open a pull request **from your branch to `main` in your own fork**. Check the **base repository** selector: it must name your team's fork, not the workshop source repository. Open the PR's **Checks** tab and the fork's **Actions** tab to inspect the results. Fix failures, push again, and repeat.

CI reports build and test results. A green check is useful feedback, but it does not prove full correctness or count as automatic completion of the exercise.

## Build and test

Run these commands from the repository root. CMake finds the locally installed GoogleTest development package.

```bash
cmake --preset clang22-debug
cmake --build --preset clang22-debug --parallel
ctest --preset clang22-debug-smoke --no-tests=error
ctest --preset clang22-debug-functional --no-tests=error
```

To run every registered test, use `ctest --test-dir build/clang22-debug --output-on-failure`. The initial functional failures are the exercise's starting point. Run the smoke test separately to check the build and test infrastructure. Do not treat initial functional failures as a broken toolchain.

For runtime debugging, a Debug build, a debugger, and optionally ASan/UBSan or `strace` can help. The exact library behavior is stated in the public headers and [ASSIGNMENT.md](ASSIGNMENT.md); a successful compile alone is not enough.

## Benchmark and CSV analysis

After your pipe implementation works and its tests pass, configure the optimized preset and run the supplied benchmark:

```bash
cmake --preset clang22-perf
cmake --build --preset clang22-perf --parallel
mkdir -p bench-results
./build/clang22-perf/bench_pipe --output bench-results/pipe.csv
uv sync --project analysis --locked
uv run --project analysis --locked python analysis/analyze.py bench-results/pipe.csv --plot bench-results/throughput.png
```

By default, the benchmark transfers 8 MiB for each of three repeats at buffer sizes 512, 4096, and 65536 bytes. You can change these with `--bytes N --repeats N --buffer-sizes 512,4096,65536`. It verifies the transferred bytes and **appends** CSV rows, so use a fresh output path for a separate run. Inspect the CSV as well as the plot. This measures the **whole transfer scenario**, including I/O, scheduling, and buffer management; it does not isolate the cost of `UniqueFd`. Run performance measurements with `clang22-perf`, not the Debug preset.

For installation and an explanation of the analysis commands, see [analysis/README.md](analysis/README.md). The Python packages go into `analysis/.venv`, not a global Python environment.

On Linux, optional local counters are available if `perf stat` and its permissions allow them:

```bash
perf stat -e task-clock,cycles,instructions,context-switches \
  ./build/clang22-perf/bench_pipe --repeats 1 --output bench-results/perf-run.csv
```

If access to counters is denied, continue with the CSV timing results. CI does not use performance thresholds.

## If a service is unavailable

If OpenCode or a model is unavailable, continue with your editor and record which parts you completed manually. If GitHub Actions is unavailable, run the same build and CTest commands locally and save the output for discussion. The instructor may provide a reference implementation for the measurement activity if a team cannot finish the library in time. You can still analyze a provided CSV and discuss the limits of its conclusions.
