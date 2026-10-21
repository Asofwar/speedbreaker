// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Online play: the game's own Xbox Live race, against an EA lobby that
// SpeedBreaker runs itself. NFS Most Wanted (360) has no System Link: its
// races are found through EA's lobby servers ("Aries"/DirtySock, PROD
// nfs-xbox-2006) and then run peer to peer (UDP 3658, voice 6000). Those
// servers are long gone; online/lobby.cpp is a stand-in for the part the
// game uses, run inside the copy that hosts:
//
//   - Off: no Xbox Live (the 360's answer for a console that isn't signed
//     in).
//   - Host: this copy runs the lobby (TCP 30960 directory, 10901 lobby)
//     and answers the local network's searches (UDP 10902); its own game
//     uses it at 127.0.0.1.
//   - Join: this copy's game uses a host's lobby: the one named in
//     online.toml (server = "address"), or the first that answers a search
//     of the local network.
//
// Either way the console is signed in to Xbox Live as far as the game
// knows (kernel/xmsg.cpp, imports_net.cpp, imports_user.cpp), under the
// gamertag of online.toml (gamertag = "name"; else the system's user
// name). The environment overrides: NFSMW_ONLINE=off|host|join (the
// setting), NFSMW_NET_HOST=<address> (the server), NFSMW_GAMERTAG=<name>.
// NFSMW_FAKE_LIVE=1 signs in with no lobby of our own (a server run apart,
// such as victorgbd's nfsmw-server-lan).
#pragma once
#include <cstdint>
#include <string>

namespace online
{
    enum class Mode : int32_t { Off = 0, Host = 1, Join = 2 };

    // The mode of this run (settings: online.mode; NFSMW_ONLINE).
    Mode GetMode();

    // Whether the game sees a console signed in to Xbox Live: a mode other
    // than Off, or NFSMW_FAKE_LIVE. Not before settings::Load().
    bool Enabled();

    // The signed-in profile's gamertag: "Player" when online play is off
    // (the name every save so far was made with), else online.toml's, the
    // environment's or the system user's; at most 15 characters.
    const std::string& Gamertag();

    // The lobby server's address, in network order: what every name the game
    // looks up resolves to and the EA service's address (XOnlineGetServiceInfo).
    // Join without a server named waits a few seconds for the local network's
    // answer the first time.
    uint32_t ServerAddress();

    // After settings::Load(): reads online.toml (writing it with its defaults
    // when there is none), starts the lobby (Host) or the search (Join).
    void Start();

    // online/lobby.cpp: the lobby server's thread (Host). False when its
    // ports can't be had (another copy hosting on this machine).
    bool StartLobby();
    // How many players are in the lobby now (the local search's answer).
    int LobbyPlayers();
}

// kernel/imports_net.cpp: this machine's address as the game knows it (host
// order: the four bytes read big-endian), NFSMW_NET_ADDR's if set.
uint32_t NetLocalAddressHost();
