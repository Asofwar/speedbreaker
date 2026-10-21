#pragma once
#define RLIMIT_STACK 3
#define RLIM_INFINITY (~0ULL)
typedef unsigned long long rlim_t;
struct rlimit { rlim_t rlim_cur, rlim_max; };
inline int getrlimit(int, rlimit* l) { l->rlim_cur = 8ull << 20; l->rlim_max = RLIM_INFINITY; return 0; }
// getrusage: the peak working set as ru_maxrss, in KB as on Linux
// (compat/win/win_compat.cpp).
#define RUSAGE_SELF 0
struct rusage { long ru_maxrss; };
int getrusage(int who, struct rusage* usage);
