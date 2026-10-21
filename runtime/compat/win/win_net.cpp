// SpeedBreaker Windows port: BSD sockets on Winsock (compat/win/sb_net.h).
// GPL-3.0-or-later.
// Ours first: Winsock's s_addr and s6_addr are macros (S_un.S_addr, u.Byte),
// which would rename the fields of sb_net.h's structs.
#define SB_NET_IMPL
#include "sb_net.h"
#undef _INC_WINDOWS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <mstcpip.h>
#undef s_addr
#undef s6_addr

#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#ifndef SIO_UDP_NETRESET
#define SIO_UDP_NETRESET _WSAIOW(IOC_VENDOR, 15)
#endif

static_assert(sizeof(sbnet::sockaddr) == sizeof(::sockaddr) && sizeof(sbnet::sockaddr_in) == sizeof(::sockaddr_in));
static_assert(sizeof(sbnet::sockaddr_storage) == sizeof(::sockaddr_storage) && sizeof(sbnet::sockaddr_in6) == sizeof(::sockaddr_in6));
static_assert(sizeof(sbnet::addrinfo) == sizeof(::ADDRINFOA) && offsetof(sbnet::addrinfo, ai_addrlen) == offsetof(::ADDRINFOA, ai_addrlen) &&
              offsetof(sbnet::addrinfo, ai_canonname) == offsetof(::ADDRINFOA, ai_canonname) &&
              offsetof(sbnet::addrinfo, ai_addr) == offsetof(::ADDRINFOA, ai_addr) && offsetof(sbnet::addrinfo, ai_next) == offsetof(::ADDRINFOA, ai_next));

namespace sbnet
{
    namespace
    {
        void Start()
        {
            static const bool started = [] {
                WSADATA data;
                return WSAStartup(MAKEWORD(2, 2), &data) == 0;
            }();
            (void)started;
        }

        struct Info
        {
            bool nonblocking = false;
        };
        std::mutex s_mutex;
        std::unordered_map<int, Info> s_sockets;

        SOCKET S(int fd)
        {
            return SOCKET(uint32_t(fd));
        }

        int Errno(int wsa, bool connecting)
        {
            switch (wsa)
            {
            case WSAEWOULDBLOCK: return connecting ? EINPROGRESS : EAGAIN;
            case WSAEINPROGRESS: return EINPROGRESS;
            case WSAEALREADY: return EALREADY;
            case WSAECONNREFUSED: return ECONNREFUSED;
            case WSAECONNRESET: return ECONNRESET;
            case WSAECONNABORTED: return ECONNABORTED;
            case WSAEHOSTUNREACH: return EHOSTUNREACH;
            case WSAENETUNREACH: return ENETUNREACH;
            case WSAENETDOWN: return ENETDOWN;
            case WSAENETRESET: return ENETRESET;
            case WSAETIMEDOUT: return ETIMEDOUT;
            case WSAEADDRINUSE: return EADDRINUSE;
            case WSAEADDRNOTAVAIL: return EADDRNOTAVAIL;
            case WSAEISCONN: return EISCONN;
            case WSAENOTCONN: return ENOTCONN;
            case WSAESHUTDOWN: return EPIPE;
            case WSAEMSGSIZE: return EMSGSIZE;
            case WSAENOBUFS: return ENOBUFS;
            case WSAENOTSOCK: return ENOTSOCK;
            case WSAEINTR: return EINTR;
            case WSAEBADF: return EBADF;
            case WSAEACCES: return EACCES;
            case WSAEFAULT: return EFAULT;
            case WSAEINVAL: return EINVAL;
            case WSAEAFNOSUPPORT: return EAFNOSUPPORT;
            case WSAEPROTONOSUPPORT: return EPROTONOSUPPORT;
            case WSAEOPNOTSUPP: return EOPNOTSUPP;
            case WSAEMFILE: return EMFILE;
            default: return EIO;
            }
        }

        int Fail(bool connecting = false)
        {
            errno = Errno(WSAGetLastError(), connecting);
            return -1;
        }

        int Track(SOCKET s)
        {
            // Not inherited by a process the game starts.
            SetHandleInformation(reinterpret_cast<HANDLE>(s), HANDLE_FLAG_INHERIT, 0);
            const int fd = int(uint32_t(s));
            std::lock_guard lock(s_mutex);
            s_sockets[fd] = Info{};
            return fd;
        }

        bool Blocking(int fd)
        {
            std::lock_guard lock(s_mutex);
            auto it = s_sockets.find(fd);
            return it == s_sockets.end() || !it->second.nonblocking;
        }

        // MSG_DONTWAIT on a blocking socket: nothing to receive is EAGAIN.
        bool WouldWait(int fd, int flags)
        {
            if (!(flags & kMsgDontWait) || !Blocking(fd))
                return false;
            WSAPOLLFD p{ S(fd), POLLRDNORM, 0 };
            if (WSAPoll(&p, 1, 0) > 0)
                return false;
            errno = EAGAIN;
            return true;
        }

        int Len(size_t size)
        {
            return size > size_t(INT_MAX) ? INT_MAX : int(size);
        }

        std::string Utf8(const wchar_t* w)
        {
            if (!w)
                return {};
            const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
            std::string s(n > 0 ? size_t(n - 1) : 0, '\0');
            if (n > 1)
                WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
            return s;
        }
    }

    int socket(int domain, int type, int protocol)
    {
        Start();
        SOCKET s = ::socket(domain, type, protocol);
        if (s == INVALID_SOCKET)
            return Fail();
        if (type == SOCK_DGRAM)
        {
            // Linux reports nothing on an unconnected UDP socket after an
            // ICMP "port unreachable"; Winsock fails the next receive with
            // WSAECONNRESET (or WSAENETRESET for a TTL's expiry).
            BOOL off = FALSE;
            DWORD returned = 0;
            WSAIoctl(s, SIO_UDP_CONNRESET, &off, sizeof(off), nullptr, 0, &returned, nullptr, nullptr);
            WSAIoctl(s, SIO_UDP_NETRESET, &off, sizeof(off), nullptr, 0, &returned, nullptr, nullptr);
        }
        return Track(s);
    }

    int bind(int s, const sockaddr* addr, socklen_t len)
    {
        return ::bind(S(s), reinterpret_cast<const ::sockaddr*>(addr), len) == 0 ? 0 : Fail();
    }

    int connect(int s, const sockaddr* addr, socklen_t len)
    {
        return ::connect(S(s), reinterpret_cast<const ::sockaddr*>(addr), len) == 0 ? 0 : Fail(true);
    }

    int listen(int s, int backlog)
    {
        return ::listen(S(s), backlog) == 0 ? 0 : Fail();
    }

    int accept(int s, sockaddr* addr, socklen_t* len)
    {
        SOCKET c = ::accept(S(s), reinterpret_cast<::sockaddr*>(addr), len);
        if (c == INVALID_SOCKET)
            return Fail();
        // Blocking, as accept's sockets are on Linux (Winsock's take the
        // listening socket's mode).
        u_long off = 0;
        ioctlsocket(c, FIONBIO, &off);
        return Track(c);
    }

    int shutdown(int s, int how)
    {
        return ::shutdown(S(s), how) == 0 ? 0 : Fail();
    }

    ssize_t send(int s, const void* data, size_t size, int flags)
    {
        const int n = ::send(S(s), static_cast<const char*>(data), Len(size), flags & ~kMsgDontWait);
        return n == SOCKET_ERROR ? Fail() : n;
    }

    ssize_t recv(int s, void* data, size_t size, int flags)
    {
        if (WouldWait(s, flags))
            return -1;
        const int n = ::recv(S(s), static_cast<char*>(data), Len(size), flags & ~kMsgDontWait);
        if (n == SOCKET_ERROR)
            return WSAGetLastError() == WSAEMSGSIZE ? ssize_t(size) : Fail();  // a datagram cut short: Linux gives its first bytes
        return n;
    }

    ssize_t sendto(int s, const void* data, size_t size, int flags, const sockaddr* to, socklen_t len)
    {
        const int n = ::sendto(S(s), static_cast<const char*>(data), Len(size), flags & ~kMsgDontWait, reinterpret_cast<const ::sockaddr*>(to), len);
        return n == SOCKET_ERROR ? Fail() : n;
    }

    ssize_t recvfrom(int s, void* data, size_t size, int flags, sockaddr* from, socklen_t* len)
    {
        if (WouldWait(s, flags))
            return -1;
        const int n = ::recvfrom(S(s), static_cast<char*>(data), Len(size), flags & ~kMsgDontWait, reinterpret_cast<::sockaddr*>(from), len);
        if (n == SOCKET_ERROR)
            return WSAGetLastError() == WSAEMSGSIZE ? ssize_t(size) : Fail();
        return n;
    }

    int setsockopt(int s, int level, int name, const void* value, socklen_t len)
    {
        return ::setsockopt(S(s), level, name, static_cast<const char*>(value), len) == 0 ? 0 : Fail();
    }

    int getsockopt(int s, int level, int name, void* value, socklen_t* len)
    {
        if (::getsockopt(S(s), level, name, static_cast<char*>(value), len) != 0)
            return Fail();
        if (level == SOL_SOCKET && name == SO_ERROR && value && len && *len >= int(sizeof(int)))
        {
            int* e = static_cast<int*>(value);
            if (*e)
                *e = Errno(*e, false);  // a WSA code, as errno's
        }
        return 0;
    }

    int getsockname(int s, sockaddr* addr, socklen_t* len)
    {
        return ::getsockname(S(s), reinterpret_cast<::sockaddr*>(addr), len) == 0 ? 0 : Fail();
    }

    int getpeername(int s, sockaddr* addr, socklen_t* len)
    {
        return ::getpeername(S(s), reinterpret_cast<::sockaddr*>(addr), len) == 0 ? 0 : Fail();
    }

    int poll(pollfd* fds, nfds_t count, int timeoutMs)
    {
        if (count == 0)
        {
            Sleep(timeoutMs < 0 ? INFINITE : DWORD(timeoutMs));
            return 0;
        }
        std::vector<WSAPOLLFD> w(count);
        for (nfds_t i = 0; i < count; i++)
        {
            w[i].fd = fds[i].fd < 0 ? INVALID_SOCKET : S(fds[i].fd);
            w[i].events = SHORT(((fds[i].events & kPollIn) ? POLLRDNORM : 0) | ((fds[i].events & kPollOut) ? POLLWRNORM : 0));
            w[i].revents = 0;
        }
        const int n = WSAPoll(w.data(), ULONG(count), timeoutMs);
        if (n == SOCKET_ERROR)
            return Fail();
        for (nfds_t i = 0; i < count; i++)
        {
            const SHORT r = w[i].revents;
            short out = 0;
            if (r & (POLLRDNORM | POLLRDBAND))
                out |= kPollIn;
            if (r & POLLWRNORM)
                out |= kPollOut;
            if (r & POLLERR)
                out |= kPollErr;
            if (r & POLLHUP)
                out |= short(kPollHup | (fds[i].events & kPollIn));  // as on Linux: readable, the read returns 0
            if (r & POLLNVAL)
                out |= kPollNval;
            fds[i].revents = out;
        }
        return n;
    }

    int inet_pton(int family, const char* text, void* out)
    {
        return ::inet_pton(family, text, out);
    }

    const char* inet_ntop(int family, const void* in, char* out, socklen_t size)
    {
        return ::inet_ntop(family, const_cast<void*>(in), out, size_t(size));
    }

    char* inet_ntoa(in_addr a)
    {
        ::in_addr w;
        w.S_un.S_addr = a.s_addr;
        return ::inet_ntoa(w);
    }

    in_addr_t inet_addr(const char* text)
    {
        return in_addr_t(::inet_addr(text));
    }

    int getaddrinfo(const char* node, const char* service, const addrinfo* hints, addrinfo** out)
    {
        Start();
        return ::getaddrinfo(node, service, reinterpret_cast<const ::ADDRINFOA*>(hints), reinterpret_cast<::ADDRINFOA**>(out));
    }

    void freeaddrinfo(addrinfo* list)
    {
        ::freeaddrinfo(reinterpret_cast<::ADDRINFOA*>(list));
    }

    const char* gai_strerror(int code)
    {
        return ::gai_strerrorA(code);
    }

    int gethostname(char* name, size_t size)
    {
        Start();
        return ::gethostname(name, Len(size)) == 0 ? 0 : Fail();
    }

    namespace
    {
        // One getifaddrs entry with what it points to.
        struct Interface
        {
            ifaddrs entry{};
            sockaddr_in address{}, netmask{}, broadcast{};
            std::string name;
        };
    }

    // The IPv4 addresses of the adapters, named by their friendly names
    // ("Ethernet", "Wi-Fi", "Tailscale", "ZeroTier One [...]").
    int getifaddrs(ifaddrs** out)
    {
        *out = nullptr;
        ULONG size = 16 * 1024;
        std::vector<uint8_t> buffer;
        ULONG result = ERROR_BUFFER_OVERFLOW;
        for (int tries = 0; tries < 4 && result == ERROR_BUFFER_OVERFLOW; tries++)
        {
            buffer.resize(size);
            result = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr,
                                          reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
        }
        if (result != NO_ERROR)
        {
            errno = result == ERROR_NO_DATA ? 0 : EIO;
            return result == ERROR_NO_DATA ? 0 : -1;
        }
        ifaddrs** tail = out;
        for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); a; a = a->Next)
        {
            for (auto* u = a->FirstUnicastAddress; u; u = u->Next)
            {
                if (!u->Address.lpSockaddr || u->Address.lpSockaddr->sa_family != AF_INET)
                    continue;
                auto* i = new Interface;
                i->name = Utf8(a->FriendlyName);
                std::memcpy(&i->address, u->Address.lpSockaddr, sizeof(i->address));
                const uint32_t prefix = u->OnLinkPrefixLength > 32 ? 32 : u->OnLinkPrefixLength;
                const uint32_t mask = prefix ? htonl(uint32_t(0xFFFFFFFFull << (32 - prefix))) : 0;
                i->netmask.sin_family = AF_INET;
                i->netmask.sin_addr.s_addr = mask;
                i->broadcast.sin_family = AF_INET;
                i->broadcast.sin_addr.s_addr = i->address.sin_addr.s_addr | ~mask;
                i->entry.ifa_name = i->name.data();
                i->entry.ifa_addr = reinterpret_cast<sockaddr*>(&i->address);
                i->entry.ifa_netmask = reinterpret_cast<sockaddr*>(&i->netmask);
                if (a->OperStatus == IfOperStatusUp)
                    i->entry.ifa_flags |= kIffUp | kIffRunning;
                if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
                    i->entry.ifa_flags |= kIffLoopback;
                else
                {
                    i->entry.ifa_flags |= kIffBroadcast;
                    i->entry.ifa_broadaddr = reinterpret_cast<sockaddr*>(&i->broadcast);
                }
                *tail = &i->entry;
                tail = &i->entry.ifa_next;
            }
        }
        return 0;
    }

    void freeifaddrs(ifaddrs* list)
    {
        while (list)
        {
            ifaddrs* next = list->ifa_next;
            delete reinterpret_cast<Interface*>(list);  // entry is Interface's first member
            list = next;
        }
    }

    bool IsSocket(int fd)
    {
        std::lock_guard lock(s_mutex);
        return s_sockets.count(fd) != 0;
    }

    int Fcntl(int fd, int cmd, long arg)
    {
        std::lock_guard lock(s_mutex);
        auto it = s_sockets.find(fd);
        if (it == s_sockets.end())
            return 0;  // a file: no flags to speak of
        if (cmd == 3)  // F_GETFL
            return 2 | (it->second.nonblocking ? 0x800 : 0);  // O_RDWR, O_NONBLOCK
        u_long on = (arg & 0x800) ? 1 : 0;  // F_SETFL
        if (ioctlsocket(S(fd), FIONBIO, &on) != 0)
            return Fail();
        it->second.nonblocking = on != 0;
        return 0;
    }

    int Ioctl(int fd, unsigned long request, int* value)
    {
        if (request != 0x541B || !value)  // FIONREAD (compat/win/sys/ioctl.h)
        {
            errno = EINVAL;
            return -1;
        }
        u_long n = 0;
        if (ioctlsocket(S(fd), FIONREAD, &n) != 0)
            return Fail();
        *value = n > u_long(INT_MAX) ? INT_MAX : int(n);
        return 0;
    }

    int CloseSocket(int fd)
    {
        {
            std::lock_guard lock(s_mutex);
            s_sockets.erase(fd);
        }
        return closesocket(S(fd)) == 0 ? 0 : Fail();
    }
}
