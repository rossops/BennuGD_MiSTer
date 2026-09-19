Profile-guided optimisation support. `profile/` is the instrumentation
runtime from LLVM 19.1.7's compiler-rt (`lib/profile`, Apache-2.0 with LLVM
exception, see LICENSE.TXT), needed because zig cc links its own
compiler-rt which has no profile runtime: an instrumented binary gets the
counters but nothing writes the .profraw. `hps/build.sh` with `PGO=gen`
links it and `pgo_runtime.c`; `PGO=use PGO_PROFILE=<file.profdata>` uses a
merged profile (`llvm-profdata merge`, Homebrew llvm@19 on the Mac).
