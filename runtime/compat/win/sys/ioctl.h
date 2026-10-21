#pragma once
#define FIONREAD 0x541B
namespace sbnet
{
    int Ioctl(int fd, unsigned long request, int* value);  // compat/win/win_net.cpp
}
// A socket's FIONREAD.
inline int ioctl(int fd, unsigned long request, int* value) { return sbnet::Ioctl(fd, request, value); }
