// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Closing a socket: close() on Linux and macOS; on Windows a socket isn't a
// file descriptor and needs closesocket (compat/win/sb_net.h).
#pragma once
#ifdef _WIN32
#include <compat/win/sb_net.h>
#else
#include <unistd.h>
#endif

namespace online
{
    inline int CloseSocket(int fd)
    {
#ifdef _WIN32
        return sbnet::CloseSocket(fd);
#else
        return ::close(fd);
#endif
    }
}
