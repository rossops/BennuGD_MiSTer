/* What compiler-rt's InstrProfilingRuntime.cpp does, in C: define the
 * variable the instrumented code references and register the profile
 * writer at start-up (it runs at exit). zig's own compiler-rt has no
 * profile runtime, so the LLVM 19 one is vendored in profile/ and linked
 * into PGO=gen builds only. */
void __llvm_profile_initialize(void);
int __llvm_profile_runtime;
__attribute__((constructor)) static void pgo_register(void) { __llvm_profile_initialize(); }
