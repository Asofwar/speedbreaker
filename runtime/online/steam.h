// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Online play over Steam (online.transport = "steam"): no address to type,
// no port to open. Every player gets a virtual address, 10.88.x.y, made
// from its SteamID; what the game sends to one goes through Valve's relay
// network (ISteamNetworkingMessages) to that player:
//
//   - UDP (the race, 3658; voice, 6000): one message a datagram, with its
//     source and destination ports. On the other side it reaches the
//     game's socket from a local relay socket, whose address recvfrom
//     turns back into the sender's virtual one.
//   - TCP (the lobby, 30960 and 10901): connect() to a virtual address goes
//     to a local proxy instead, and its bytes travel reliably to the host,
//     which opens the connection to its own lobby (online/lobby.cpp asks
//     TunnelPeer() who it is).
//
// The host makes a Steam lobby (AppID 480, "Spacewar": every Steam user
// has it) marked speedbreaker=1; joining finds such a lobby (a friend's
// invitation, or the list) and its owner is the host. Steam's own library
// (libsteam_api.so) isn't shipped: it is loaded from a Steam game that has
// it (most do), or NFSMW_STEAM_API=<path>.
#pragma once
#include <cstddef>
#include <cstdint>
#include <netinet/in.h>
#include <sys/types.h>

namespace online::steam
{
    // Loads Steam, signs in (as AppID 480), and makes (host) or finds (join)
    // the lobby. False when Steam can't be had: no library, Steam not
    // running.
    bool Start(bool host);
    bool Active();

    // The player's Steam name, for its gamertag ("" if not known).
    const char* PersonaName();

    // Virtual addresses, host order (the four bytes read big-endian).
    constexpr bool IsVirtual(uint32_t address) { return (address >> 16) == ((10u << 8) | 88u); }
    uint32_t OwnAddress();
    // The host's (the lobby's owner); waits up to `seconds` for the lobby.
    uint32_t HostAddress(int seconds);

    // UDP: a datagram from the game's socket `fd` (its port is the source
    // port) to a virtual address.
    ssize_t Send(int fd, uint32_t to, uint16_t port, const void* data, size_t size);
    // A datagram received from a relay socket: its source becomes the
    // sender's virtual address and port. False if it isn't one.
    bool TranslateSource(sockaddr_in& from);

    // TCP: the local proxy to connect to instead of a virtual address.
    bool Connect(uint32_t to, uint16_t port, sockaddr_in& proxy);
    // The host's lobby: the virtual address of the player behind a tunnelled
    // connection, by the connection's local port. False if it isn't one.
    bool TunnelPeer(uint16_t localPort, uint32_t& address);
}
