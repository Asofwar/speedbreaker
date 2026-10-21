// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Steam's flat C API, loaded at run time (no SDK headers, no import
// library: GPL-clean), with manual callback dispatch. Steam's callback
// structs are packed to 4 bytes on Linux and macOS, to 8 on Windows: a
// uint64 after an int32 is at 4 or at 8 (kAfterInt32).
#include "steam.h"
#include "online.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <online/socket.h>
#include <unistd.h>
#ifdef _WIN32
#include <compat/win/win_sys.h>
#include <fstream>
#endif

namespace online::steam
{
    namespace
    {
        // ---- the flat API

        using Pipe = int32_t;
        using Call = uint64_t;
        struct CallbackMsg { int32_t user; int32_t callback; uint8_t* param; int32_t size; };
        struct Identity { uint8_t bytes[136]; };  // SteamNetworkingIdentity: opaque, through its Set/Get calls
        struct Message { void* data; int32_t size; uint32_t connection; Identity peer; };  // SteamNetworkingMessage_t's start

        struct Api
        {
            int (*InitFlat)(char*);
            Pipe (*GetHSteamPipe)();
            void (*DispatchInit)();
            void (*RunFrame)(Pipe);
            bool (*GetNextCallback)(Pipe, CallbackMsg*);
            void (*FreeLastCallback)(Pipe);
            bool (*GetCallResult)(Pipe, Call, void*, int, int, bool*);
            void* (*User)();
            uint64_t (*GetSteamID)(void*);
            void* (*Matchmaking)();
            Call (*CreateLobby)(void*, int, int);
            Call (*JoinLobby)(void*, uint64_t);
            bool (*SetLobbyData)(void*, uint64_t, const char*, const char*);
            uint64_t (*GetLobbyOwner)(void*, uint64_t);
            void (*AddStringFilter)(void*, const char*, const char*, int);
            void (*AddDistanceFilter)(void*, int);
            Call (*RequestLobbyList)(void*);
            uint64_t (*GetLobbyByIndex)(void*, int);
            int (*GetNumLobbyMembers)(void*, uint64_t);
            uint64_t (*GetLobbyMemberByIndex)(void*, uint64_t, int);
            void* (*Messages)();
            int (*SendMessageToUser)(void*, const Identity*, const void*, uint32_t, int, int);
            int (*ReceiveMessagesOnChannel)(void*, int, Message**, int);
            bool (*AcceptSessionWithUser)(void*, const Identity*);
            void (*SetSteamID64)(Identity*, uint64_t);
            uint64_t (*GetSteamID64)(Identity*);
            void (*Release)(Message*);
            void* (*Friends)();
            const char* (*GetPersonaName)(void*);
        };

        Api a{};
        void *s_user, *s_mm, *s_net, *s_friends;
        Pipe s_pipe = 0;
        std::atomic<bool> s_active{ false };
        std::recursive_mutex s_steam;  // around every Steam call

#ifdef _WIN32
        constexpr size_t kAfterInt32 = 8;
        constexpr const char* kLibrary = "steam_api64.dll";
#else
        constexpr size_t kAfterInt32 = 4;
        constexpr const char* kLibrary = "libsteam_api.so";
#endif
        constexpr int kChannelDatagram = 0, kChannelTunnel = 1;
        constexpr int kSendUnreliable = 0, kSendNoNagle = 1, kSendReliable = 8;
        constexpr int kLobbyFriends = 1, kLobbyPublic = 2;
        // callbacks
        constexpr int kGameLobbyJoinRequested = 333, kLobbyEnter = 504, kLobbyChatUpdate = 506, kLobbyMatchList = 510, kLobbyCreated = 513,
                      kCallCompleted = 703, kSessionRequest = 1251, kSessionFailed = 1252;

        std::mutex s_mutex;
        std::condition_variable s_changed;
        uint64_t s_self = 0, s_lobby = 0, s_host = 0;
        bool s_hosting = false;
        std::string s_persona;
        std::map<uint32_t, uint64_t> s_players;  // virtual address -> SteamID
        std::map<Call, int> s_calls;             // pending call -> its result's callback id
        std::chrono::steady_clock::time_point s_nextSearch{};

        uint32_t VirtualAddress(uint64_t id)
        {
            uint64_t h = id * 0x9E3779B97F4A7C15ull;
            uint32_t low = uint32_t(h >> 40) & 0xFFFF;
            if ((low & 0xFF) == 0 || (low & 0xFF) == 255)
                low ^= 0x01;
            return (10u << 24) | (88u << 16) | low;
        }

        void Know(uint64_t id)
        {
            if (!id)
                return;
            std::lock_guard lock(s_mutex);
            s_players[VirtualAddress(id)] = id;
        }

        bool IsMember(uint64_t id)
        {
            std::lock_guard lock(s_mutex);
            auto it = s_players.find(VirtualAddress(id));
            return it != s_players.end() && it->second == id;
        }

        void Members(uint64_t lobby);

        // A message from someone: a member of our lobby, else (a player who
        // just joined, before its notice) after a fresh look at the members.
        bool FromMember(uint64_t id)
        {
            if (IsMember(id))
                return true;
            uint64_t lobby;
            {
                std::lock_guard lock(s_mutex);
                lobby = s_lobby;
            }
            if (lobby)
                Members(lobby);
            return IsMember(id);
        }

        uint64_t PlayerAt(uint32_t address)
        {
            std::lock_guard lock(s_mutex);
            auto it = s_players.find(address);
            return it == s_players.end() ? 0 : it->second;
        }

        Identity Who(uint64_t id)
        {
            Identity i{};
            a.SetSteamID64(&i, id);
            return i;
        }

        bool SendTo(uint64_t id, int channel, const void* data, size_t size, int flags)
        {
            const Identity i = Who(id);
            std::lock_guard lock(s_steam);
            return a.SendMessageToUser(s_net, &i, data, uint32_t(size), flags, channel) == 1;  // k_EResultOK
        }

        // ---- loading

        template<typename T>
        bool Bind(void* lib, T& out, std::initializer_list<const char*> names)
        {
            for (const char* n : names)
                if (void* f = dlsym(lib, n))
                {
                    out = reinterpret_cast<T>(f);
                    return true;
                }
            fprintf(stderr, "[steam] %s isn't in the library\n", *names.begin());
            return false;
        }

        void* Open(const std::string& path)
        {
            void* lib = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
            if (lib && !dlsym(lib, "SteamAPI_InitFlat"))
            {
                dlclose(lib);  // older than the flat API
                return nullptr;
            }
            return lib;
        }

        // The folders Steam's games are in: on Linux under the home folder,
        // on Windows under Steam's own folder (the registry knows it) and
        // the other libraries its libraryfolders.vdf lists.
        std::vector<std::filesystem::path> GameFolders()
        {
            std::vector<std::filesystem::path> out;
#ifdef _WIN32
            std::string steam = winsys::UserRegistryString("Software\\Valve\\Steam", "SteamPath");
            if (steam.empty())
                return out;
            const std::filesystem::path root = std::filesystem::path(steam);
            out.push_back(root / "steamapps" / "common");
            // "path"		"D:\\SteamLibrary"
            std::ifstream vdf(root / "steamapps" / "libraryfolders.vdf");
            for (std::string line; std::getline(vdf, line);)
            {
                const size_t key = line.find("\"path\"");
                if (key == std::string::npos)
                    continue;
                const size_t open = line.find('"', key + 6), close = line.rfind('"');
                if (open == std::string::npos || close <= open)
                    continue;
                std::string path;
                for (size_t i = open + 1; i < close; i++)
                {
                    if (line[i] == '\\' && i + 1 < close)
                        i++;
                    path += line[i];
                }
                const std::filesystem::path common = std::filesystem::path(path) / "steamapps" / "common";
                if (std::find(out.begin(), out.end(), common) == out.end())
                    out.push_back(common);
            }
#else
            const char* home = std::getenv("HOME");
            if (!home)
                return out;
            for (const char* root : { "/.local/share/Steam/steamapps/common", "/.steam/steam/steamapps/common", "/.steam/root/steamapps/common" })
                out.push_back(std::string(home) + root);
#endif
            return out;
        }

        // A Steam game's copy of Steam's library (libsteam_api.so,
        // steam_api64.dll): most ship one.
        void* Find()
        {
            if (const char* v = std::getenv("NFSMW_STEAM_API"); v && *v)
            {
                void* lib = Open(v);
                if (!lib)
                    fprintf(stderr, "[steam] NFSMW_STEAM_API=%s doesn't load (%s)\n", v, dlerror());
                return lib;
            }
            if (void* lib = Open(kLibrary))  // on the library path (Windows: next to the game too)
                return lib;
            std::error_code ec;
            for (const std::filesystem::path& base : GameFolders())
            {
                if (!std::filesystem::is_directory(base, ec))
                    continue;
                int seen = 0;
                for (auto it = std::filesystem::recursive_directory_iterator(base, std::filesystem::directory_options::skip_permission_denied, ec);
                     it != std::filesystem::recursive_directory_iterator() && seen < 200000; it.increment(ec), seen++)
                {
                    if (ec)
                        break;
                    if (it.depth() > 5)
                    {
                        it.disable_recursion_pending();
                        continue;
                    }
                    if (it->path().filename() != kLibrary)
                        continue;
                    if (void* lib = Open(it->path().string()))
                    {
                        fprintf(stderr, "[steam] Steam's library from %s\n", it->path().string().c_str());
                        return lib;
                    }
                }
            }
            return nullptr;
        }

        bool Load()
        {
            void* lib = Find();
            if (!lib)
            {
                fprintf(stderr, "[steam] no %s with the flat API (a Steam game's, or NFSMW_STEAM_API=<path>)\n", kLibrary);
                return false;
            }
            bool ok = Bind(lib, a.InitFlat, { "SteamAPI_InitFlat" }) && Bind(lib, a.GetHSteamPipe, { "SteamAPI_GetHSteamPipe" }) &&
                      Bind(lib, a.DispatchInit, { "SteamAPI_ManualDispatch_Init" }) && Bind(lib, a.RunFrame, { "SteamAPI_ManualDispatch_RunFrame" }) &&
                      Bind(lib, a.GetNextCallback, { "SteamAPI_ManualDispatch_GetNextCallback" }) &&
                      Bind(lib, a.FreeLastCallback, { "SteamAPI_ManualDispatch_FreeLastCallback" }) &&
                      Bind(lib, a.GetCallResult, { "SteamAPI_ManualDispatch_GetAPICallResult" }) &&
                      Bind(lib, a.User, { "SteamAPI_SteamUser_v023", "SteamAPI_SteamUser_v022", "SteamAPI_SteamUser_v021" }) &&
                      Bind(lib, a.GetSteamID, { "SteamAPI_ISteamUser_GetSteamID" }) && Bind(lib, a.Matchmaking, { "SteamAPI_SteamMatchmaking_v009" }) &&
                      Bind(lib, a.CreateLobby, { "SteamAPI_ISteamMatchmaking_CreateLobby" }) && Bind(lib, a.JoinLobby, { "SteamAPI_ISteamMatchmaking_JoinLobby" }) &&
                      Bind(lib, a.SetLobbyData, { "SteamAPI_ISteamMatchmaking_SetLobbyData" }) &&
                      Bind(lib, a.GetLobbyOwner, { "SteamAPI_ISteamMatchmaking_GetLobbyOwner" }) &&
                      Bind(lib, a.AddStringFilter, { "SteamAPI_ISteamMatchmaking_AddRequestLobbyListStringFilter" }) &&
                      Bind(lib, a.AddDistanceFilter, { "SteamAPI_ISteamMatchmaking_AddRequestLobbyListDistanceFilter" }) &&
                      Bind(lib, a.RequestLobbyList, { "SteamAPI_ISteamMatchmaking_RequestLobbyList" }) &&
                      Bind(lib, a.GetLobbyByIndex, { "SteamAPI_ISteamMatchmaking_GetLobbyByIndex" }) &&
                      Bind(lib, a.GetNumLobbyMembers, { "SteamAPI_ISteamMatchmaking_GetNumLobbyMembers" }) &&
                      Bind(lib, a.GetLobbyMemberByIndex, { "SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex" }) &&
                      Bind(lib, a.Messages, { "SteamAPI_SteamNetworkingMessages_SteamAPI_v002" }) &&
                      Bind(lib, a.SendMessageToUser, { "SteamAPI_ISteamNetworkingMessages_SendMessageToUser" }) &&
                      Bind(lib, a.ReceiveMessagesOnChannel, { "SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel" }) &&
                      Bind(lib, a.AcceptSessionWithUser, { "SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser" }) &&
                      Bind(lib, a.SetSteamID64, { "SteamAPI_SteamNetworkingIdentity_SetSteamID64" }) &&
                      Bind(lib, a.GetSteamID64, { "SteamAPI_SteamNetworkingIdentity_GetSteamID64" }) &&
                      Bind(lib, a.Release, { "SteamAPI_SteamNetworkingMessage_t_Release" }) &&
                      Bind(lib, a.Friends, { "SteamAPI_SteamFriends_v018", "SteamAPI_SteamFriends_v017" }) &&
                      Bind(lib, a.GetPersonaName, { "SteamAPI_ISteamFriends_GetPersonaName" });
            return ok;
        }

        // ---- the relay of datagrams

        struct Relay { int fd; uint32_t from; uint16_t port; };
        std::mutex s_relayMutex;
        std::map<uint64_t, Relay> s_relays;           // (sender's address << 16 | its port) -> relay socket
        std::map<uint16_t, std::pair<uint32_t, uint16_t>> s_relayPorts;  // relay's local port -> sender's address, port

        void Deliver(uint32_t from, uint16_t fromPort, uint16_t toPort, const uint8_t* data, size_t size)
        {
            // only the game's own ports: the race (3658) and voice (6000)
            if (toPort != 3658 && toPort != 6000)
                return;
            int fd;
            {
                std::lock_guard lock(s_relayMutex);
                const uint64_t key = (uint64_t(from) << 16) | fromPort;
                auto it = s_relays.find(key);
                if (it == s_relays.end())
                {
                    if (s_relays.size() >= 64)
                        return;  // (four players use a handful)
                    fd = socket(AF_INET, SOCK_DGRAM, 0);
                    sockaddr_in me{};
                    me.sin_family = AF_INET;
                    me.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                    bind(fd, reinterpret_cast<sockaddr*>(&me), sizeof(me));
                    socklen_t l = sizeof(me);
                    getsockname(fd, reinterpret_cast<sockaddr*>(&me), &l);
                    s_relays[key] = Relay{ fd, from, fromPort };
                    s_relayPorts[ntohs(me.sin_port)] = { from, fromPort };
                }
                else
                    fd = it->second.fd;
            }
            sockaddr_in to{};
            to.sin_family = AF_INET;
            // the game's sockets: on NFSMW_NET_ADDR's address when set (two copies on one machine)
            static const in_addr_t game = [] {
                in_addr g{};
                const char* v = std::getenv("NFSMW_NET_ADDR");
                return v && inet_pton(AF_INET, v, &g) == 1 ? g.s_addr : htonl(INADDR_LOOPBACK);
            }();
            to.sin_addr.s_addr = game;
            to.sin_port = htons(toPort);
            sendto(fd, data, size, 0, reinterpret_cast<sockaddr*>(&to), sizeof(to));
        }

        // ---- the TCP tunnels

        enum : uint8_t { kOpen = 1, kData = 2, kClose = 3 };
        struct Tunnel
        {
            uint64_t peer = 0;
            uint32_t id = 0;
            uint16_t port = 0;
            int listener = -1;  // the joiner's proxy, until the game connects
            int fd = -1;
        };
        std::mutex s_tunnelMutex;
        std::vector<Tunnel> s_tunnels;
        std::map<uint16_t, uint32_t> s_tunnelPeers;  // the host's side: local port -> the player's address
        std::atomic<uint32_t> s_nextTunnel{ 1 };

        void TunnelSend(const Tunnel& t, uint8_t type, const void* data = nullptr, size_t size = 0)
        {
            std::vector<uint8_t> m(7 + size);
            m[0] = type;
            memcpy(&m[1], &t.id, 4);
            memcpy(&m[5], &t.port, 2);
            if (size)
                memcpy(&m[7], data, size);
            SendTo(t.peer, kChannelTunnel, m.data(), m.size(), kSendReliable | kSendNoNagle);
        }

        void TunnelMessage(uint64_t from, const uint8_t* m, size_t size)
        {
            if (size < 7)
                return;
            uint32_t id;
            uint16_t port;
            memcpy(&id, m + 1, 4);
            memcpy(&port, m + 5, 2);
            std::lock_guard lock(s_tunnelMutex);
            auto it = std::find_if(s_tunnels.begin(), s_tunnels.end(), [&](const Tunnel& t) { return t.peer == from && t.id == id; });
            if (m[0] == kOpen && it == s_tunnels.end())
            {
                bool hosting;
                {
                    std::lock_guard l(s_mutex);
                    hosting = s_hosting;
                }
                // only to our lobby (the directory and the lobby itself), and a few at a time
                if (!hosting || (port != 30960 && port != 10901) || s_tunnels.size() >= 32)
                    return;
                // the host: a player's connection to our lobby
                const int fd = socket(AF_INET, SOCK_STREAM, 0);
                sockaddr_in me{}, lobby{};
                me.sin_family = lobby.sin_family = AF_INET;
                me.sin_addr.s_addr = lobby.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                lobby.sin_port = htons(port);
                bind(fd, reinterpret_cast<sockaddr*>(&me), sizeof(me));
                socklen_t l = sizeof(me);
                getsockname(fd, reinterpret_cast<sockaddr*>(&me), &l);
                s_tunnelPeers[ntohs(me.sin_port)] = VirtualAddress(from);
                if (connect(fd, reinterpret_cast<sockaddr*>(&lobby), sizeof(lobby)) != 0)
                {
                    online::CloseSocket(fd);
                    Tunnel t{ from, id, port };
                    TunnelSend(t, kClose);
                    return;
                }
                fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
                int one = 1;
                setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
                s_tunnels.push_back(Tunnel{ from, id, port, -1, fd });
                return;
            }
            if (it == s_tunnels.end())
                return;
            if (m[0] == kData && it->fd >= 0)
            {
                size_t done = 0;
                while (done < size - 7)
                {
                    const ssize_t n = send(it->fd, m + 7 + done, size - 7 - done, MSG_NOSIGNAL);
                    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                    {
                        pollfd w{ it->fd, POLLOUT, 0 };
                        poll(&w, 1, 50);
                        continue;
                    }
                    if (n <= 0)
                        break;
                    done += size_t(n);
                }
            }
            else if (m[0] == kClose)
            {
                if (it->fd >= 0)
                    online::CloseSocket(it->fd);
                if (it->listener >= 0)
                    online::CloseSocket(it->listener);
                s_tunnels.erase(it);
            }
        }

        void PumpTunnels()
        {
            std::vector<pollfd> fds;
            {
                std::lock_guard lock(s_tunnelMutex);
                for (auto& t : s_tunnels)
                    fds.push_back({ t.fd >= 0 ? t.fd : t.listener, POLLIN, 0 });
            }
            if (fds.empty() || poll(fds.data(), nfds_t(fds.size()), 0) <= 0)
                return;
            std::lock_guard lock(s_tunnelMutex);
            for (auto& p : fds)
            {
                if (!p.revents)
                    continue;
                auto it = std::find_if(s_tunnels.begin(), s_tunnels.end(), [&](const Tunnel& t) { return t.fd == p.fd || (t.fd < 0 && t.listener == p.fd); });
                if (it == s_tunnels.end())
                    continue;
                if (it->fd < 0)
                {
                    // the game connected to the proxy: open the tunnel
                    it->fd = accept(it->listener, nullptr, nullptr);
                    online::CloseSocket(it->listener);
                    it->listener = -1;
                    if (it->fd < 0)
                        continue;
                    fcntl(it->fd, F_SETFL, fcntl(it->fd, F_GETFL, 0) | O_NONBLOCK);
                    int one = 1;
                    setsockopt(it->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
                    TunnelSend(*it, kOpen);
                    continue;
                }
                uint8_t buffer[16384];
                const ssize_t n = recv(it->fd, buffer, sizeof(buffer), 0);
                if (n > 0)
                    TunnelSend(*it, kData, buffer, size_t(n));
                else if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
                {
                    TunnelSend(*it, kClose);
                    online::CloseSocket(it->fd);
                    s_tunnels.erase(it);
                }
            }
        }

        // ---- lobbies and callbacks

        void Search()
        {
            std::lock_guard lock(s_steam);
            a.AddStringFilter(s_mm, "speedbreaker", "1", 0);  // k_ELobbyComparisonEqual
            a.AddDistanceFilter(s_mm, 3);                      // worldwide
            s_calls[a.RequestLobbyList(s_mm)] = kLobbyMatchList;
            s_nextSearch = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        }

        void Join(uint64_t lobby)
        {
            fprintf(stderr, "[steam] joining lobby %llu\n", (unsigned long long)lobby);
            std::lock_guard lock(s_steam);
            s_calls[a.JoinLobby(s_mm, lobby)] = kLobbyEnter;
        }

        void Members(uint64_t lobby)
        {
            std::lock_guard lock(s_steam);
            const int n = a.GetNumLobbyMembers(s_mm, lobby);
            for (int i = 0; i < n; i++)
                Know(a.GetLobbyMemberByIndex(s_mm, lobby, i));
        }

        void Handle(int callback, const uint8_t* p)
        {
            auto u64 = [p](size_t at) { uint64_t v; memcpy(&v, p + at, 8); return v; };
            auto u32 = [p](size_t at) { uint32_t v; memcpy(&v, p + at, 4); return v; };
            switch (callback)
            {
            case kLobbyCreated:  // { int32 result; uint64 lobby }
            {
                const uint64_t lobby = u64(kAfterInt32);
                if (u32(0) != 1)
                {
                    fprintf(stderr, "[steam] the lobby wasn't made (result %u)\n", u32(0));
                    break;
                }
                {
                    std::lock_guard lock(s_steam);
                    a.SetLobbyData(s_mm, lobby, "speedbreaker", "1");
                    a.SetLobbyData(s_mm, lobby, "host", Gamertag().c_str());
                }
                {
                    std::lock_guard lock(s_mutex);
                    s_lobby = lobby;
                    s_host = s_self;
                }
                fprintf(stderr, "[steam] hosting Steam lobby %llu\n", (unsigned long long)lobby);
                s_changed.notify_all();
                break;
            }
            case kLobbyMatchList:  // { uint32 count }
            {
                if (u32(0) == 0)
                    break;  // another search in a few seconds
                uint64_t lobby;
                {
                    std::lock_guard lock(s_steam);
                    lobby = a.GetLobbyByIndex(s_mm, 0);
                }
                Join(lobby);
                break;
            }
            case kGameLobbyJoinRequested:  // a friend's invitation: { uint64 lobby; uint64 friend }
                Join(u64(0));
                break;
            case kLobbyEnter:  // { uint64 lobby; uint32 permissions; bool locked; uint32 response }
            {
                if (u32(16) != 1)
                {
                    fprintf(stderr, "[steam] couldn't join lobby %llu (response %u)\n", (unsigned long long)u64(0), u32(16));
                    break;
                }
                uint64_t owner;
                {
                    std::lock_guard lock(s_steam);
                    owner = a.GetLobbyOwner(s_mm, u64(0));
                }
                Members(u64(0));
                {
                    std::lock_guard lock(s_mutex);
                    s_lobby = u64(0);
                    if (!s_hosting)
                        s_host = owner;
                }
                Know(owner);
                fprintf(stderr, "[steam] in lobby %llu, hosted by %llu\n", (unsigned long long)u64(0), (unsigned long long)owner);
                s_changed.notify_all();
                break;
            }
            case kLobbyChatUpdate:  // { uint64 lobby; uint64 changed; uint64 making; uint32 state }
                Members(u64(0));
                break;
            case kSessionRequest:  // { SteamNetworkingIdentity remote }
            {
                Identity i;
                memcpy(&i, p, sizeof(i));
                uint64_t id;
                {
                    std::lock_guard lock(s_steam);
                    id = a.GetSteamID64(&i);
                }
                if (!FromMember(id))
                {
                    fprintf(stderr, "[steam] a message from %llu, not in our lobby: refused\n", (unsigned long long)id);
                    break;
                }
                std::lock_guard lock(s_steam);
                a.AcceptSessionWithUser(s_net, &i);
                break;
            }
            case kSessionFailed:
                fprintf(stderr, "[steam] a connection to a player failed\n");
                break;
            default:
                break;
            }
        }

        void Dispatch()
        {
            std::lock_guard lock(s_steam);
            a.RunFrame(s_pipe);
            CallbackMsg m{};
            while (a.GetNextCallback(s_pipe, &m))
            {
                if (m.callback == kCallCompleted)
                {
                    // { uint64 call; int32 callback; uint32 size }
                    Call call;
                    int32_t id;
                    uint32_t size;
                    memcpy(&call, m.param, 8);
                    memcpy(&id, m.param + 8, 4);
                    memcpy(&size, m.param + 12, 4);
                    std::vector<uint8_t> result(std::max<uint32_t>(size, 8));
                    bool failed = false;
                    if (a.GetCallResult(s_pipe, call, result.data(), int(size), id, &failed) && !failed)
                    {
                        a.FreeLastCallback(s_pipe);
                        s_calls.erase(call);
                        Handle(id, result.data());
                        continue;
                    }
                }
                else
                {
                    std::vector<uint8_t> copy(m.param, m.param + std::max(m.size, 0));
                    const int id = m.callback;
                    a.FreeLastCallback(s_pipe);
                    copy.resize(std::max<size_t>(copy.size(), 160));
                    Handle(id, copy.data());
                    continue;
                }
                a.FreeLastCallback(s_pipe);
            }
        }

        void Receive()
        {
            for (int channel : { kChannelDatagram, kChannelTunnel })
            {
                Message* messages[32];
                int n;
                {
                    std::lock_guard lock(s_steam);
                    n = a.ReceiveMessagesOnChannel(s_net, channel, messages, 32);
                }
                for (int i = 0; i < n; i++)
                {
                    Message* m = messages[i];
                    uint64_t from;
                    {
                        std::lock_guard lock(s_steam);
                        from = a.GetSteamID64(&m->peer);
                    }
                    const bool member = FromMember(from);
                    const uint8_t* d = static_cast<const uint8_t*>(m->data);
                    // (a stranger's message, not one of our lobby's members: dropped)
                    if (member && channel == kChannelDatagram && m->size >= 4)
                        Deliver(VirtualAddress(from), uint16_t(d[0] << 8 | d[1]), uint16_t(d[2] << 8 | d[3]), d + 4, size_t(m->size - 4));
                    else if (member && channel == kChannelTunnel)
                        TunnelMessage(from, d, size_t(m->size));
                    std::lock_guard lock(s_steam);
                    a.Release(m);
                }
            }
        }

        void Loop()
        {
            while (true)
            {
                Dispatch();
                Receive();
                PumpTunnels();
                bool searching;
                {
                    std::lock_guard lock(s_mutex);
                    searching = !s_hosting && !s_lobby;
                }
                if (searching && std::chrono::steady_clock::now() >= s_nextSearch)
                    Search();
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }
    }

    bool Start(bool host)
    {
        if (!Load())
            return false;
        // AppID 480 (Spacewar): every Steam user has it.
        setenv("SteamAppId", "480", 1);
        setenv("SteamGameId", "480", 1);
        char error[1024] = {};
        if (const int r = a.InitFlat(error); r != 0)
        {
            fprintf(stderr, "[steam] Steam doesn't start (%d: %s); is it running?\n", r, error);
            return false;
        }
        a.DispatchInit();
        s_pipe = a.GetHSteamPipe();
        s_user = a.User();
        s_mm = a.Matchmaking();
        s_net = a.Messages();
        s_friends = a.Friends();
        s_self = a.GetSteamID(s_user);
        if (const char* name = a.GetPersonaName(s_friends))
            s_persona = name;
        Know(s_self);
        s_hosting = host;
        s_active = true;
        fprintf(stderr, "[steam] signed in as %s (%llu), address 10.88.%u.%u\n", s_persona.c_str(), (unsigned long long)s_self,
            (VirtualAddress(s_self) >> 8) & 255, VirtualAddress(s_self) & 255);
        if (host)
        {
            const char* kind = std::getenv("NFSMW_STEAM_LOBBY");
            std::lock_guard lock(s_steam);
            s_calls[a.CreateLobby(s_mm, kind && std::string(kind) == "friends" ? kLobbyFriends : kLobbyPublic, 4)] = kLobbyCreated;
        }
        else if (const char* id = std::getenv("NFSMW_STEAM_JOIN"); id && *id)
            Join(strtoull(id, nullptr, 10));
        std::thread(Loop).detach();
        return true;
    }

    bool Active() { return s_active.load(); }

    const char* PersonaName() { return s_persona.c_str(); }

    uint32_t OwnAddress() { return VirtualAddress(s_self); }

    uint32_t HostAddress(int seconds)
    {
        std::unique_lock lock(s_mutex);
        s_changed.wait_for(lock, std::chrono::seconds(seconds), [] { return s_host != 0; });
        return s_host ? VirtualAddress(s_host) : 0;
    }

    ssize_t Send(int fd, uint32_t to, uint16_t port, const void* data, size_t size)
    {
        const uint64_t id = PlayerAt(to);
        if (!id)
        {
            errno = EHOSTUNREACH;
            return -1;
        }
        sockaddr_in me{};
        socklen_t l = sizeof(me);
        getsockname(fd, reinterpret_cast<sockaddr*>(&me), &l);
        if (me.sin_port == 0)
        {
            // an unbound socket: give it a port, as sendto does
            me.sin_family = AF_INET;
            me.sin_addr.s_addr = htonl(INADDR_ANY);
            bind(fd, reinterpret_cast<sockaddr*>(&me), sizeof(me));
            l = sizeof(me);
            getsockname(fd, reinterpret_cast<sockaddr*>(&me), &l);
        }
        std::vector<uint8_t> m(4 + size);
        const uint16_t from = ntohs(me.sin_port);
        m[0] = uint8_t(from >> 8);
        m[1] = uint8_t(from);
        m[2] = uint8_t(port >> 8);
        m[3] = uint8_t(port);
        memcpy(m.data() + 4, data, size);
        return SendTo(id, kChannelDatagram, m.data(), m.size(), kSendUnreliable | kSendNoNagle) ? ssize_t(size) : -1;
    }

    bool TranslateSource(sockaddr_in& from)
    {
        if (from.sin_addr.s_addr != htonl(INADDR_LOOPBACK))
            return false;
        std::lock_guard lock(s_relayMutex);
        auto it = s_relayPorts.find(ntohs(from.sin_port));
        if (it == s_relayPorts.end())
            return false;
        from.sin_addr.s_addr = htonl(it->second.first);
        from.sin_port = htons(it->second.second);
        return true;
    }

    bool Connect(uint32_t to, uint16_t port, sockaddr_in& proxy)
    {
        const uint64_t id = PlayerAt(to);
        if (!id)
            return false;
        const int listener = socket(AF_INET, SOCK_STREAM, 0);
        proxy = sockaddr_in{};
        proxy.sin_family = AF_INET;
        proxy.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(listener, reinterpret_cast<sockaddr*>(&proxy), sizeof(proxy)) != 0 || listen(listener, 1) != 0)
        {
            online::CloseSocket(listener);
            return false;
        }
        socklen_t l = sizeof(proxy);
        getsockname(listener, reinterpret_cast<sockaddr*>(&proxy), &l);
        std::lock_guard lock(s_tunnelMutex);
        s_tunnels.push_back(Tunnel{ id, s_nextTunnel++, port, listener, -1 });
        return true;
    }

    bool TunnelPeer(uint16_t localPort, uint32_t& address)
    {
        std::lock_guard lock(s_tunnelMutex);
        auto it = s_tunnelPeers.find(localPort);
        if (it == s_tunnelPeers.end())
            return false;
        address = it->second;
        return true;
    }
}
