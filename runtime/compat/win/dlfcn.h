#pragma once
// dlopen and the rest on LoadLibrary (compat/win/win_compat.cpp); the file
// name is UTF-8.
#define RTLD_LAZY 0x1
#define RTLD_NOW 0x2
#define RTLD_LOCAL 0x0
#define RTLD_GLOBAL 0x100
void* dlopen(const char* file, int flags);
void* dlsym(void* lib, const char* name);
int dlclose(void* lib);
const char* dlerror();
typedef struct { const char* dli_fname; void* dli_fbase; const char* dli_sname; void* dli_saddr; } Dl_info;
inline int dladdr(const void*, Dl_info* i) { if (i) { i->dli_fname = nullptr; i->dli_fbase = nullptr; i->dli_sname = nullptr; i->dli_saddr = nullptr; } return 0; }
