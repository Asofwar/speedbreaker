// SpeedBreaker. GPL-3.0-or-later (see COPYING).
//
// Built into the instrumented build only (-DSB_PGO_GENERATE=ON, see
// CMakeLists.txt and scripts/pgo_train.sh), with the linker's --wrap=_Exit:
// every call of _Exit in the game comes here instead. The profile runtime
// writes its counters from an atexit handler, but the game always leaves
// through _Exit (main.cpp, debug/bench.cpp: after its own shutdown, with the
// guest's threads still running), which skips atexit handlers, so a training
// run would end with an empty .profraw (the runtime makes the file when the
// game starts). This writes the counters first (to LLVM_PROFILE_FILE; without
// it, default_<signature>.profraw in the working folder), then exits as _Exit
// does. Not a .cpp file, so runtime/CMakeLists.txt's glob of the runtime's
// sources never builds it into the normal game.

int __llvm_profile_write_file(void);
_Noreturn void __real__Exit(int status);

_Noreturn void __wrap__Exit(int status)
{
    __llvm_profile_write_file();
    __real__Exit(status);
}
