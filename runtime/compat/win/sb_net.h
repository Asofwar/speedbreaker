// SpeedBreaker Windows port: BSD sockets on Winsock. GPL-3.0-or-later.
//
// The network code (kernel/imports_net.cpp, online/) is written against
// POSIX sockets. On Windows compat/win's <sys/socket.h>, <netinet/in.h>,
// <arpa/inet.h>, <netdb.h>, <poll.h>, <ifaddrs.h> and <net/if.h> come here:
// POSIX's types, constants and functions, declared in namespace sbnet and
// brought into the global namespace by using-declarations, so that none of
// them is Winsock's own symbol (whose types differ: a SOCKET is 64 bits,
// lengths are int). compat/win/win_net.cpp implements them on Winsock,
// where:
//   - a socket is an int (Winsock's handles fit), tracked so that fcntl's
//     O_NONBLOCK, ioctl's FIONREAD and online/socket.h's CloseSocket can
//     tell it from a file descriptor;
//   - a failure sets errno to the POSIX code (WSAEWOULDBLOCK: EAGAIN, or
//     EINPROGRESS from connect);
//   - UDP sockets don't report an ICMP "port unreachable" from an earlier
//     send as a failed receive (Winsock's WSAECONNRESET; Linux reports
//     nothing on an unconnected socket);
//   - MSG_DONTWAIT is honoured on a blocking socket, MSG_NOSIGNAL is 0.
// The constants have Winsock's values, which go to it unchanged.
#pragma once
#ifdef _WIN32
#include <cstddef>
#include <cstdint>
#include <sys/types.h>

namespace sbnet
{
    using socklen_t = int;
    using sa_family_t = uint16_t;
    using in_port_t = uint16_t;
    using in_addr_t = uint32_t;
    using nfds_t = unsigned long;

    struct sockaddr
    {
        sa_family_t sa_family;
        char sa_data[14];
    };
    struct in_addr
    {
        in_addr_t s_addr;
    };
    struct sockaddr_in
    {
        sa_family_t sin_family;
        in_port_t sin_port;
        in_addr sin_addr;
        unsigned char sin_zero[8];
    };
    struct in6_addr
    {
        uint8_t s6_addr[16];
    };
    struct sockaddr_in6
    {
        sa_family_t sin6_family;
        in_port_t sin6_port;
        uint32_t sin6_flowinfo;
        in6_addr sin6_addr;
        uint32_t sin6_scope_id;
    };
    struct sockaddr_storage
    {
        sa_family_t ss_family;
        char ss_pad1[6];
        int64_t ss_align;
        char ss_pad2[112];
    };
    // Winsock's ADDRINFOA, field for field.
    struct addrinfo
    {
        int ai_flags;
        int ai_family;
        int ai_socktype;
        int ai_protocol;
        size_t ai_addrlen;
        char* ai_canonname;
        sockaddr* ai_addr;
        addrinfo* ai_next;
    };
    struct pollfd
    {
        int fd;
        short events;
        short revents;
    };
    struct ifaddrs
    {
        ifaddrs* ifa_next;
        char* ifa_name;
        unsigned int ifa_flags;
        sockaddr* ifa_addr;
        sockaddr* ifa_netmask;
        sockaddr* ifa_broadaddr;
        void* ifa_data;
    };

    int socket(int domain, int type, int protocol);
    int bind(int s, const sockaddr* addr, socklen_t len);
    int connect(int s, const sockaddr* addr, socklen_t len);
    int listen(int s, int backlog);
    int accept(int s, sockaddr* addr, socklen_t* len);
    int shutdown(int s, int how);
    ssize_t send(int s, const void* data, size_t size, int flags);
    ssize_t recv(int s, void* data, size_t size, int flags);
    ssize_t sendto(int s, const void* data, size_t size, int flags, const sockaddr* to, socklen_t len);
    ssize_t recvfrom(int s, void* data, size_t size, int flags, sockaddr* from, socklen_t* len);
    int setsockopt(int s, int level, int name, const void* value, socklen_t len);
    int getsockopt(int s, int level, int name, void* value, socklen_t* len);
    int getsockname(int s, sockaddr* addr, socklen_t* len);
    int getpeername(int s, sockaddr* addr, socklen_t* len);
    int poll(pollfd* fds, nfds_t count, int timeoutMs);
    int inet_pton(int family, const char* text, void* out);
    const char* inet_ntop(int family, const void* in, char* out, socklen_t size);
    char* inet_ntoa(in_addr a);
    in_addr_t inet_addr(const char* text);
    int getaddrinfo(const char* node, const char* service, const addrinfo* hints, addrinfo** out);
    void freeaddrinfo(addrinfo* list);
    const char* gai_strerror(int code);
    int getifaddrs(ifaddrs** out);
    void freeifaddrs(ifaddrs* list);
    int gethostname(char* name, size_t size);

    // For compat/win: whether `fd` is a socket of ours, its fcntl and ioctl,
    // and closing it.
    bool IsSocket(int fd);
    int Fcntl(int fd, int cmd, long arg);
    int Ioctl(int fd, unsigned long request, int* value);
    int CloseSocket(int fd);

    constexpr uint16_t htons(uint16_t v) { return uint16_t((v >> 8) | (v << 8)); }
    constexpr uint16_t ntohs(uint16_t v) { return htons(v); }
    constexpr uint32_t htonl(uint32_t v) { return __builtin_bswap32(v); }
    constexpr uint32_t ntohl(uint32_t v) { return __builtin_bswap32(v); }

    // POSIX's poll bits (win_net.cpp converts them to Winsock's).
    constexpr short kPollIn = 0x001, kPollPri = 0x002, kPollOut = 0x004, kPollErr = 0x008, kPollHup = 0x010, kPollNval = 0x020;
    // Ours: a receive that doesn't wait (Winsock has no such flag).
    constexpr int kMsgDontWait = 0x40000000;
    // getifaddrs' flags, Linux's values (Winsock's own IFF_ are others).
    constexpr unsigned kIffUp = 0x1, kIffBroadcast = 0x2, kIffLoopback = 0x8, kIffRunning = 0x40;
}

#ifndef SB_NET_IMPL
using sbnet::socklen_t;
using sbnet::sa_family_t;
using sbnet::in_port_t;
using sbnet::in_addr_t;
using sbnet::nfds_t;
using sbnet::sockaddr;
using sbnet::in_addr;
using sbnet::sockaddr_in;
using sbnet::in6_addr;
using sbnet::sockaddr_in6;
using sbnet::sockaddr_storage;
using sbnet::addrinfo;
using sbnet::pollfd;
using sbnet::ifaddrs;
using sbnet::socket;
using sbnet::bind;
using sbnet::connect;
using sbnet::listen;
using sbnet::accept;
using sbnet::shutdown;
using sbnet::send;
using sbnet::recv;
using sbnet::sendto;
using sbnet::recvfrom;
using sbnet::setsockopt;
using sbnet::getsockopt;
using sbnet::getsockname;
using sbnet::getpeername;
using sbnet::poll;
using sbnet::inet_pton;
using sbnet::inet_ntop;
using sbnet::inet_ntoa;
using sbnet::inet_addr;
using sbnet::getaddrinfo;
using sbnet::freeaddrinfo;
using sbnet::gai_strerror;
using sbnet::getifaddrs;
using sbnet::freeifaddrs;
using sbnet::gethostname;
using sbnet::htons;
using sbnet::ntohs;
using sbnet::htonl;
using sbnet::ntohl;

#define AF_UNSPEC 0
#define AF_INET 2
#define AF_INET6 23
#define PF_INET AF_INET
#define SOCK_STREAM 1
#define SOCK_DGRAM 2
#define IPPROTO_IP 0
#define IPPROTO_TCP 6
#define IPPROTO_UDP 17
#define SOL_SOCKET 0xFFFF
#define SO_REUSEADDR 0x0004
#define SO_KEEPALIVE 0x0008
#define SO_BROADCAST 0x0020
#define SO_LINGER 0x0080
#define SO_SNDBUF 0x1001
#define SO_RCVBUF 0x1002
#define SO_SNDTIMEO 0x1005
#define SO_RCVTIMEO 0x1006
#define SO_ERROR 0x1007
#define SO_TYPE 0x1008
#define TCP_NODELAY 0x0001
#define MSG_OOB 0x1
#define MSG_PEEK 0x2
#define MSG_NOSIGNAL 0
#define MSG_DONTWAIT sbnet::kMsgDontWait
#define SHUT_RD 0
#define SHUT_WR 1
#define SHUT_RDWR 2
#define INADDR_ANY 0x00000000u
#define INADDR_LOOPBACK 0x7F000001u
#define INADDR_BROADCAST 0xFFFFFFFFu
#define INADDR_NONE 0xFFFFFFFFu
#define INET_ADDRSTRLEN 16
#define INET6_ADDRSTRLEN 46
#define POLLIN sbnet::kPollIn
#define POLLPRI sbnet::kPollPri
#define POLLOUT sbnet::kPollOut
#define POLLERR sbnet::kPollErr
#define POLLHUP sbnet::kPollHup
#define POLLNVAL sbnet::kPollNval
#define AI_PASSIVE 0x01
#define AI_CANONNAME 0x02
#define AI_NUMERICHOST 0x04
#define AI_ADDRCONFIG 0x0400
#define EAI_AGAIN 11002
#define EAI_FAIL 11003
#define EAI_NONAME 11001
#define IFF_UP sbnet::kIffUp
#define IFF_BROADCAST sbnet::kIffBroadcast
#define IFF_LOOPBACK sbnet::kIffLoopback
#define IFF_RUNNING sbnet::kIffRunning
#define IFNAMSIZ 256
#endif
#endif
