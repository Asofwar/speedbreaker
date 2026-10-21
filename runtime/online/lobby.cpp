// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// The EA lobby, as far as NFS Most Wanted (360) uses it, run by the copy
// that hosts (online/online.h). Written from what the game sends and
// expects: the message flow below, as logged while it played against
// victorgbd's nfsmw-server-lan (a separate Python program, which found the
// hard parts: the user sets, the records the peer-to-peer link is built
// from).
//
// Messages: a 12-byte header (4 letters, a 4-byte status, the length with
// the header, big-endian), then KEY=value lines and a 0. Directory, TCP
// 30960: @tic (an offer to encrypt; no answer keeps it plain), @dir (the
// lobby's address and port). Lobby, TCP 10901, after it:
//
//   addr skey news auth pers sele    log-in; the player's record (+who)
//   user                             a profile (its stats)
//   usea / +uss                      the quick match's search: the rooms
//   ucre ujoi ulea udel uadm         rooms ("user sets"); members +usm, +usr
//   gcre gjoi gset gsta glea gdel    the race (game) in a room; +mgm, +ses
//   auxi mesg                        a player's state text; messages (+msg)
//   ~png                             alive (both ways, every 20 s)
//
// The parts the game is particular about (from nfsmw-server-lan's notes):
//   - A room's member list (its slot-0 user set) is filled only by +usm:
//     S slot, I, N, F, G, X. The owner is put in slot 0 when it creates
//     the room; a joining player subscribes (sele USERSET0=<room>) first.
//   - +who/+usr carry G (the player's game, 0: none; a game with 0 is lost)
//     and MA (the MADDR the player sent: its XUID and XNADDR, which the
//     peer-to-peer link is built from).
//   - The game record (gcre gjoi gset +mgm +ses) has per player OPPO OPID
//     ADDR LADDR MADDR OPPART OPPARAM OPFLAG PRES; SESS is the host's Xbox
//     session ("$..."), set by its gset, which the others connect with.
//   - +ses (the race starts) goes out at the host's gsta, not before.
//   - Unknown messages get the same message back, empty.
#include "online.h"
#include "steam.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace online
{
    namespace
    {
        constexpr uint16_t kDirectoryPort = 30960, kLobbyPort = 10901, kSearchPort = 10902;

        using Fields = std::vector<std::pair<std::string, std::string>>;

        std::string Get(const Fields& f, const char* key, const std::string& fallback = "")
        {
            for (auto& [k, v] : f)
                if (k == key)
                    return v;
            return fallback;
        }

        std::string Pack(const char* command, const Fields& fields = {}, const char* status = "\0\0\0\0")
        {
            std::string body;
            for (auto& [k, v] : fields)
                body += k + "=" + v + "\n";
            body += '\0';
            std::string m(command, 4);
            m.append(status, 4);
            const uint32_t length = uint32_t(12 + body.size());
            m += char(length >> 24);
            m += char(length >> 16);
            m += char(length >> 8);
            m += char(length);
            return m + body;
        }

        Fields Parse(const char* body, size_t size)
        {
            Fields out;
            std::string text(body, strnlen(body, size));
            size_t at = 0;
            while (at < text.size())
            {
                size_t end = text.find('\n', at);
                if (end == std::string::npos)
                    end = text.size();
                std::string line = text.substr(at, end - at);
                if (size_t eq = line.find('='); eq != std::string::npos)
                    out.emplace_back(line.substr(0, eq), line.substr(eq + 1));
                at = end + 1;
            }
            return out;
        }

        // A small decimal number from the peer, or -1 (too long, not digits).
        int Number(const std::string& v)
        {
            if (v.empty() || v.size() > 9 || !std::all_of(v.begin(), v.end(), [](char c) { return c >= '0' && c <= '9'; }))
                return -1;
            return std::atoi(v.c_str());
        }

        std::string Now()
        {
            const std::time_t t = std::time(nullptr);
            std::tm tm{};
            localtime_r(&t, &tm);
            char b[32];
            std::strftime(b, sizeof(b), "%Y.%m.%d-%H:%M:%S", &tm);
            return b;
        }

        std::mt19937& Random()
        {
            static std::mt19937 r{ std::random_device{}() };
            return r;
        }

        std::string Hex(int bytes)
        {
            static const char* digits = "0123456789abcdef";
            std::string s;
            for (int i = 0; i < bytes * 2; i++)
                s += digits[Random()() & 15];
            return s;
        }

        std::string Ip(uint32_t networkOrder)
        {
            in_addr a{ networkOrder };
            return inet_ntoa(a);
        }

        struct Game;
        struct Room;

        struct Session
        {
            int fd = -1;
            bool lobby = false;  // on 10901 (the directory's connections only ask @dir)
            std::string in, out;
            int id = 0;
            std::string ip, local;  // the player's address; the lobby's, as it reached us
            std::string name = "USER", persona, maddr, auxi;
            bool authenticated = false;
            bool self = false;  // the host's own game (at 127.0.0.1): to each player, at the address that player reaches us at
            int room = 0;                    // the room it is in (0: none)
            std::shared_ptr<Game> game;      // the race it is in
            std::map<std::string, int> sets;  // user sets it follows: room name -> slot
            std::chrono::steady_clock::time_point lastPing = std::chrono::steady_clock::now();

            std::string Persona() const { return persona.empty() ? name : persona; }
        };

        struct Room
        {
            int id = 0;
            std::string type, sysFlags, custFlags, ownerName, size, name, desc, params;
            int count = 1;
            Session* owner = nullptr;
        };

        struct Game
        {
            int ident = 0;
            std::string name, host, room;
            Session* hostSession = nullptr;
            std::map<std::string, std::string> fields;  // as gcre and gset gave them
            std::vector<Session*> players;
            std::string seed, when;
        };

        class Lobby
        {
        public:
            bool Listen()
            {
                m_directory = ListenTcp(kDirectoryPort);
                m_lobby = ListenTcp(kLobbyPort);
                m_search = ListenUdp(kSearchPort);
                if (m_directory < 0 || m_lobby < 0)
                {
                    fprintf(stderr, "[lobby] can't listen on TCP %u and %u (another copy hosting here?): no lobby\n", kDirectoryPort, kLobbyPort);
                    return false;
                }
                fprintf(stderr, "[lobby] listening: directory %u, lobby %u, the local network's search %u/udp%s\n", kDirectoryPort, kLobbyPort,
                    kSearchPort, m_search < 0 ? " (taken: not found by searches)" : "");
                return true;
            }

            void Run()
            {
                while (true)
                {
                    std::vector<pollfd> fds;
                    for (int fd : { m_directory, m_lobby, m_search })
                        if (fd >= 0)
                            fds.push_back({ fd, POLLIN, 0 });
                    for (auto& s : m_sessions)
                        fds.push_back({ s->fd, short(POLLIN | (s->out.empty() ? 0 : POLLOUT)), 0 });
                    poll(fds.data(), nfds_t(fds.size()), 1000);
                    for (auto& p : fds)
                    {
                        if (!p.revents)
                            continue;
                        if (p.fd == m_directory || p.fd == m_lobby)
                            Accept(p.fd, p.fd == m_lobby);
                        else if (p.fd == m_search)
                            AnswerSearch();
                        else if (Session* s = Find(p.fd))
                        {
                            if (p.revents & POLLOUT)
                                Flush(*s);
                            if (p.revents & (POLLIN | POLLHUP | POLLERR))
                                Read(*s);
                        }
                    }
                    Ping();
                    Reap();
                }
            }

            int Players() const { return int(std::count_if(m_sessions.begin(), m_sessions.end(), [](auto& s) { return s->authenticated; })); }

        private:
            int m_directory = -1, m_lobby = -1, m_search = -1, m_nextId = 1000, m_nextSession = 1;
            std::vector<std::unique_ptr<Session>> m_sessions;
            std::map<int, Room> m_rooms;
            std::map<std::string, std::shared_ptr<Game>> m_games;
            std::vector<int> m_closed;

            static int ListenTcp(uint16_t port)
            {
                const int fd = socket(AF_INET, SOCK_STREAM, 0);
                if (fd < 0)
                    return -1;
                int one = 1;
                setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
                sockaddr_in a{};
                a.sin_family = AF_INET;
                a.sin_port = htons(port);
                a.sin_addr.s_addr = htonl(INADDR_ANY);
                if (bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 || listen(fd, 16) != 0)
                {
                    close(fd);
                    return -1;
                }
                fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
                return fd;
            }

            static int ListenUdp(uint16_t port)
            {
                const int fd = socket(AF_INET, SOCK_DGRAM, 0);
                if (fd < 0)
                    return -1;
                int one = 1;
                setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
                setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
                sockaddr_in a{};
                a.sin_family = AF_INET;
                a.sin_port = htons(port);
                a.sin_addr.s_addr = htonl(INADDR_ANY);
                if (bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0)
                {
                    close(fd);
                    return -1;
                }
                return fd;
            }

            Session* Find(int fd)
            {
                for (auto& s : m_sessions)
                    if (s->fd == fd)
                        return s.get();
                return nullptr;
            }

            void Accept(int listener, bool lobby)
            {
                sockaddr_in peer{}, local{};
                socklen_t l = sizeof(peer);
                const int fd = accept(listener, reinterpret_cast<sockaddr*>(&peer), &l);
                if (fd < 0)
                    return;
                fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
                int one = 1;
                setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
                l = sizeof(local);
                getsockname(fd, reinterpret_cast<sockaddr*>(&local), &l);
                auto s = std::make_unique<Session>();
                s->fd = fd;
                s->lobby = lobby;
                s->id = m_nextSession++;
                // The host's own game reaches us at 127.0.0.1: the others must
                // be given its address on the network instead.
                const bool fromHere = (ntohl(peer.sin_addr.s_addr) >> 24) == 127;
                s->ip = peer.sin_addr.s_addr == htonl(INADDR_LOOPBACK) || (fromHere && steam::Active()) ? Ip(htonl(NetLocalAddressHost()))
                                                                                                         : Ip(peer.sin_addr.s_addr);
                // the lobby's address for this player (@dir, news): the one it
                // reached us at
                s->local = Ip(local.sin_addr.s_addr);
                // a player over Steam: the tunnel knows its virtual address,
                // and it reaches us at ours (through its tunnel)
                if (uint32_t player; fromHere && steam::TunnelPeer(ntohs(peer.sin_port), player))
                {
                    s->ip = Ip(htonl(player));
                    s->local = Ip(htonl(NetLocalAddressHost()));
                }
                s->self = peer.sin_addr.s_addr == htonl(INADDR_LOOPBACK) && !steam::Active();
                fprintf(stderr, "[lobby] %s connects (%s)\n", s->ip.c_str(), lobby ? "lobby" : "directory");
                m_sessions.push_back(std::move(s));
            }

            void AnswerSearch()
            {
                char buffer[256];
                sockaddr_in from{};
                socklen_t l = sizeof(from);
                const ssize_t n = recvfrom(m_search, buffer, sizeof(buffer) - 1, 0, reinterpret_cast<sockaddr*>(&from), &l);
                if (n <= 0)
                    return;
                buffer[n] = 0;
                if (strncmp(buffer, "NFSMW_DISCOVER", 14) != 0)
                    return;
                const std::string answer = "NFSMW_SERVER\nNAME=" + Gamertag() + "\nPLAYERS=" + std::to_string(Players()) + "\n";
                sendto(m_search, answer.data(), answer.size(), 0, reinterpret_cast<sockaddr*>(&from), l);
            }

            void Send(Session& s, const std::string& message)
            {
                s.out += message;
                Flush(s);
            }

            void Flush(Session& s)
            {
                while (!s.out.empty())
                {
                    const ssize_t n = send(s.fd, s.out.data(), s.out.size(), MSG_NOSIGNAL);
                    if (n <= 0)
                        return;  // the rest when it can take it (POLLOUT); a dead socket is found by the read
                    s.out.erase(0, size_t(n));
                }
            }

            void Read(Session& s)
            {
                char buffer[16384];
                const ssize_t n = recv(s.fd, buffer, sizeof(buffer), 0);
                if (n <= 0)
                {
                    if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
                        m_closed.push_back(s.fd);
                    return;
                }
                s.in.append(buffer, size_t(n));
                while (s.in.size() >= 12)
                {
                    const uint32_t length = (uint32_t(uint8_t(s.in[8])) << 24) | (uint32_t(uint8_t(s.in[9])) << 16) | (uint32_t(uint8_t(s.in[10])) << 8) |
                                            uint8_t(s.in[11]);
                    if (length < 12 || length > (1u << 20))
                    {
                        fprintf(stderr, "[lobby] %s: a message of %u bytes: dropped\n", s.ip.c_str(), length);
                        m_closed.push_back(s.fd);
                        return;
                    }
                    if (s.in.size() < length)
                        return;
                    const std::string command = s.in.substr(0, 4);
                    const Fields fields = Parse(s.in.data() + 12, length - 12);
                    s.in.erase(0, length);
                    try
                    {
                        Handle(s, command, fields);
                    }
                    catch (const std::exception& e)
                    {
                        fprintf(stderr, "[lobby] %s: '%s' failed (%s): dropped\n", s.ip.c_str(), command.c_str(), e.what());
                        m_closed.push_back(s.fd);
                        return;
                    }
                }
            }

            void Ping()
            {
                const auto now = std::chrono::steady_clock::now();
                for (auto& s : m_sessions)
                    if (s->lobby && now - s->lastPing > std::chrono::seconds(20))
                    {
                        s->lastPing = now;
                        Send(*s, Pack("~png", { { "REF", Now() }, { "TIME", "2" } }));
                    }
            }

            void Reap()
            {
                for (int fd : m_closed)
                {
                    auto it = std::find_if(m_sessions.begin(), m_sessions.end(), [fd](auto& s) { return s->fd == fd; });
                    if (it == m_sessions.end())
                        continue;
                    Forget(**it);
                    fprintf(stderr, "[lobby] %s (%s) leaves\n", (*it)->ip.c_str(), (*it)->Persona().c_str());
                    close(fd);
                    m_sessions.erase(it);
                }
                m_closed.clear();
            }

            // ---- records

            // A player's address as `viewer` sees it: the host's own game at the
            // address the viewer reaches the lobby at (its address on the
            // viewer's network: a LAN's, a VPN's, the public one).
            static const std::string& AddressFor(const Session& s, const Session& viewer) { return s.self && !viewer.self ? viewer.local : s.ip; }

            std::string UserRecord(const Session& s, const Session& viewer, const char* command = "+who")
            {
                const std::string& a = AddressFor(s, viewer);
                return Pack(command, { { "I", std::to_string(s.id) }, { "M", s.name }, { "N", s.Persona() }, { "MA", s.maddr }, { "A", a }, { "LA", a },
                                         { "G", s.game ? std::to_string(s.game->ident) : "0" }, { "P", "1" }, { "F", "U" }, { "LO", "enUS" }, { "PRES", "1" },
                                         { "X", s.auxi } });
            }

            static Fields RoomRecord(const Room& r)
            {
                return { { "I", std::to_string(r.id) }, { "T", r.type }, { "SF", r.sysFlags }, { "CF", r.custFlags }, { "O", r.ownerName }, { "S", r.size },
                    { "C", std::to_string(r.count) }, { "N", r.name }, { "D", r.desc }, { "P", r.params } };
            }

            static std::string MemberRecord(const Session& s, int slot, bool remove)
            {
                if (remove)
                    return Pack("+usm", { { "S", std::to_string(slot) }, { "I", std::to_string(s.id) } });
                return Pack("+usm", { { "S", std::to_string(slot) }, { "I", std::to_string(s.id) }, { "N", s.Persona() }, { "F", "U" },
                                        { "G", s.game ? std::to_string(s.game->ident) : "0" }, { "X", s.auxi } });
            }

            static Fields GameRecord(const Game& g, const Session& viewer)
            {
                auto field = [&](const char* key, const std::string& fallback) {
                    auto it = g.fields.find(key);
                    return it != g.fields.end() ? it->second : fallback;
                };
                Fields f = { { "IDENT", std::to_string(g.ident) }, { "NAME", g.name }, { "SELF", viewer.Persona() }, { "HOST", g.host },
                    { "PARAMS", field("PARAMS", "") }, { "PLATPARAMS", field("PLATPARAMS", "") }, { "ROOM", g.room }, { "CUSTFLAGS", field("CUSTFLAGS", "0") },
                    { "SYSFLAGS", field("SYSFLAGS", "0") }, { "COUNT", std::to_string(g.players.size()) }, { "PRIV", "0" },
                    { "MINSIZE", field("MINSIZE", "2") }, { "MAXSIZE", field("MAXSIZE", "4") }, { "NUMPART", "1" }, { "PARTSIZE0", field("MAXSIZE", "4") },
                    { "PARTPARAMS0", "" }, { "SEED", g.seed }, { "WHEN", g.when }, { "AUTH", "" }, { "SESS", field("SESS", "") } };
                for (size_t i = 0; i < g.players.size(); i++)
                {
                    const Session& p = *g.players[i];
                    const std::string n = std::to_string(i);
                    const std::string& a = AddressFor(p, viewer);
                    f.insert(f.end(), { { "OPPO" + n, p.Persona() }, { "OPID" + n, std::to_string(p.id) }, { "ADDR" + n, a }, { "LADDR" + n, a },
                                          { "MADDR" + n, p.maddr }, { "OPPART" + n, "0" }, { "OPPARAM" + n, "" }, { "OPFLAG" + n, "0" }, { "PRES" + n, "0" } });
                }
                return f;
            }

            // ---- rooms and games

            Room* RoomByName(const std::string& name)
            {
                for (auto& [id, r] : m_rooms)
                    if (r.name == name)
                        return &r;
                return nullptr;
            }

            std::vector<Session*> Members(const Room& r)
            {
                std::vector<Session*> out;
                for (auto& s : m_sessions)
                    if (s->room == r.id)
                        out.push_back(s.get());
                return out;
            }

            // +usm for `who` to every follower of the room; `requester`'s copy is returned.
            std::vector<std::string> NotifySet(const Room& r, const Session& who, const Session* requester, bool remove)
            {
                std::vector<std::string> own;
                for (auto& o : m_sessions)
                {
                    auto it = o->sets.find(r.name);
                    if (it == o->sets.end())
                        continue;
                    std::string m = MemberRecord(who, it->second, remove);
                    if (o.get() == requester)
                        own.push_back(std::move(m));
                    else
                        Send(*o, m);
                }
                return own;
            }

            void DropRoom(int id)
            {
                auto it = m_rooms.find(id);
                if (it == m_rooms.end())
                    return;
                Room& r = it->second;
                for (Session* m : Members(r))
                    if (m != r.owner)
                    {
                        NotifySet(r, *m, nullptr, true);
                        m->room = 0;
                    }
                if (r.owner && r.owner->room == id)
                    r.owner->room = 0;
                fprintf(stderr, "[lobby] room %s of %s closed\n", r.name.c_str(), r.ownerName.c_str());
                m_rooms.erase(it);
            }

            void NotifyGame(const Game& g, const char* command, const Session* skip)
            {
                for (Session* p : g.players)
                    if (p != skip)
                        Send(*p, Pack(command, GameRecord(g, *p)));
            }

            void LeaveGame(Session& s)
            {
                std::shared_ptr<Game> g = s.game;
                if (!g)
                    return;
                std::erase(g->players, &s);
                s.game.reset();
                if (g->players.empty() || g->hostSession == &s)
                {
                    for (Session* p : g->players)
                        p->game.reset();
                    if (auto it = m_games.find(g->name); it != m_games.end() && it->second == g)
                        m_games.erase(it);
                    fprintf(stderr, "[lobby] race %s closed\n", g->name.c_str());
                }
                else
                    NotifyGame(*g, "+mgm", nullptr);
            }

            void Forget(Session& s)
            {
                LeaveGame(s);
                if (auto it = m_rooms.find(s.room); it != m_rooms.end())
                    NotifySet(it->second, s, &s, true);
                std::vector<int> owned;
                for (auto& [id, r] : m_rooms)
                    if (r.owner == &s)
                        owned.push_back(id);
                for (int id : owned)
                    DropRoom(id);
            }

            // ---- messages

            void Handle(Session& s, const std::string& command, const Fields& f)
            {
                std::vector<std::string> reply;
                const std::string me = s.Persona();
                if (command == "@tic" || command == "addr" || command == "~png")
                {
                }
                else if (command == "@dir")
                    reply.push_back(Pack("@dir", { { "ADDR", s.local }, { "PORT", std::to_string(kLobbyPort) },
                                                     { "SESS", std::to_string(1000000000 + Random()() % 900000000) }, { "MASK", Hex(16) } }));
                else if (command == "skey")
                    reply.push_back(Pack("skey", { { "SKEY", "$" + Hex(16) } }));
                else if (command == "news")
                    reply.push_back(Pack("news", { { "BUDDY_SERVER", s.local }, { "BUDDY_PORT", "10899" }, { "TOSAC_URL", "" }, { "TOSA_URL", "" }, { "TOS_URL", "" } }));
                else if (command == "auth")
                {
                    s.name = Get(f, "NAME", Get(f, "GTAG", s.name));
                    if (s.name.empty())
                        s.name = "USER";
                    s.maddr = Get(f, "MADDR", s.maddr);
                    s.authenticated = true;
                    reply.push_back(Pack("auth", { { "NAME", s.name }, { "ADDR", s.ip }, { "PERSONAS", s.name }, { "LOC", "enUS" }, { "MAIL", "lan@lan" },
                                                     { "SPAM", "NN" }, { "TOS", "1" }, { "SINCE", "2005.11.15-00:00:00" }, { "LAST", "2005.11.15-00:00:00" } }));
                }
                else if (command == "pers")
                {
                    s.persona = Get(f, "PERS", Get(f, "GTAG", s.name));
                    s.maddr = Get(f, "MADDR", s.maddr);
                    fprintf(stderr, "[lobby] %s is %s\n", s.ip.c_str(), s.Persona().c_str());
                    reply.push_back(Pack("pers", { { "PERS", s.Persona() }, { "LKEY", Hex(16) }, { "LOC", "enUS" }, { "A", s.ip }, { "LA", s.ip }, { "IDLE", "600000" } }));
                }
                else if (command == "sele")
                {
                    reply.push_back(Pack("sele", f));
                    reply.push_back(UserRecord(s, s));
                    for (auto& [k, v] : f)
                    {
                        const int slot = k.rfind("USERSET", 0) == 0 ? Number(k.substr(7)) : -1;
                        if (slot < 0 || slot > 8)
                            continue;
                        std::erase_if(s.sets, [slot](auto& e) { return e.second == slot; });
                        if (v.empty())
                            continue;
                        s.sets[v] = slot;
                        if (Room* r = RoomByName(v))
                        {
                            reply.push_back(Pack("+ust", RoomRecord(*r)));
                            for (Session* m : Members(*r))
                                reply.push_back(MemberRecord(*m, slot, false));
                        }
                    }
                }
                else if (command == "user")
                {
                    std::string zeros;
                    for (int i = 0; i < 44; i++)
                        zeros += i ? ",0" : "0";
                    reply.push_back(Pack("user", { { "PERS", Get(f, "PERS", me) }, { "LAST", "2005.11.15-00:00:00" }, { "EXPR", "1072566000" }, { "STAT", zeros },
                                                     { "LWSTAT", zeros }, { "LMSTAT", zeros }, { "LGAME", "0" } }));
                }
                else if (command == "usea")
                {
                    const std::string wanted = Get(f, "NAME");
                    std::vector<const Room*> found;
                    for (auto& [id, r] : m_rooms)
                        if (r.owner != &s && (wanted.empty() || r.name == wanted))
                            found.push_back(&r);
                    reply.push_back(Pack("usea", { { "START", Get(f, "START", "0") }, { "COUNT", std::to_string(found.size()) } }));
                    for (const Room* r : found)
                        reply.push_back(Pack("+uss", RoomRecord(*r)));
                }
                else if (command == "ucre")
                {
                    std::vector<int> old;
                    for (auto& [id, r] : m_rooms)
                        if (r.owner == &s)
                            old.push_back(id);
                    for (int id : old)
                        DropRoom(id);
                    Room r;
                    r.id = ++m_nextId;
                    r.type = Get(f, "TYPE", "0");
                    r.sysFlags = Get(f, "SYSFLAGS");
                    r.custFlags = Get(f, "CUSTFLAGS");
                    r.ownerName = me;
                    r.size = Get(f, "SIZE", "4");
                    r.name = Get(f, "NAME");
                    r.params = Get(f, "PARAMS");
                    r.owner = &s;
                    m_rooms[r.id] = r;
                    s.room = r.id;
                    std::erase_if(s.sets, [](auto& e) { return e.second == 0; });
                    s.sets[r.name] = 0;
                    fprintf(stderr, "[lobby] %s opens room %s\n", me.c_str(), r.name.c_str());
                    reply.push_back(Pack("ucre", RoomRecord(r)));
                    reply.push_back(UserRecord(s, s, "+usr"));
                    reply.push_back(MemberRecord(s, 0, false));
                }
                else if (command == "uadm")
                {
                    if (auto it = m_rooms.find(s.room); it != m_rooms.end())
                    {
                        Room& r = it->second;
                        // its followers know it by name: they follow the new one
                        const std::string newName = Get(f, "NAME", r.name);
                        if (newName != r.name)
                            for (auto& o : m_sessions)
                                if (auto e = o->sets.find(r.name); e != o->sets.end())
                                {
                                    const int slot = e->second;
                                    o->sets.erase(e);
                                    o->sets[newName] = slot;
                                }
                        r.name = newName;
                        r.desc = Get(f, "DESC", r.desc);
                        r.params = Get(f, "PARAMS", r.params);
                        r.custFlags = Get(f, "CUSTFLAGS", r.custFlags);
                        reply.push_back(Pack("uadm", RoomRecord(r)));
                    }
                    else
                        reply.push_back(Pack("uadm"));
                }
                else if (command == "ujoi")
                {
                    Room* r = nullptr;
                    for (const char* key : { "I", "IDENT", "ROOM" })
                        if (int id = Number(Get(f, key)); id >= 0)
                            if (auto it = m_rooms.find(id); it != m_rooms.end())
                                r = &it->second;
                    if (!r)
                        r = RoomByName(Get(f, "NAME", Get(f, "N")));
                    if (!r)
                        reply.push_back(Pack("ujoi", {}, "nfnd"));
                    else
                    {
                        r->count++;
                        s.room = r->id;
                        fprintf(stderr, "[lobby] %s joins room %s\n", me.c_str(), r->name.c_str());
                        reply.push_back(Pack("ujoi", RoomRecord(*r)));
                        for (Session* o : Members(*r))
                            if (o != &s)
                            {
                                Send(*o, UserRecord(s, *o, "+usr"));
                                reply.push_back(UserRecord(*o, s, "+usr"));
                            }
                        reply.push_back(UserRecord(s, s, "+usr"));
                        for (auto& m : NotifySet(*r, s, &s, false))
                            reply.push_back(std::move(m));
                    }
                }
                else if (command == "ulea")
                {
                    if (auto it = m_rooms.find(s.room); it != m_rooms.end())
                    {
                        if (it->second.count > 1)
                            it->second.count--;
                        s.room = 0;
                        reply.push_back(Pack("ulea"));
                        for (auto& m : NotifySet(it->second, s, &s, true))
                            reply.push_back(std::move(m));
                    }
                    else
                    {
                        s.room = 0;
                        reply.push_back(Pack("ulea"));
                    }
                }
                else if (command == "udel")
                {
                    Room* r = RoomByName(Get(f, "NAME"));
                    if (!r)
                        if (auto it = m_rooms.find(s.room); it != m_rooms.end())
                            r = &it->second;
                    if (r && r->owner == &s)
                        DropRoom(r->id);
                    reply.push_back(Pack("udel"));
                }
                else if (command == "gcre")
                {
                    auto g = std::make_shared<Game>();
                    g->ident = ++m_nextId;
                    g->name = Get(f, "NAME", me);
                    g->host = me;
                    g->hostSession = &s;
                    g->room = std::to_string(s.room);
                    for (auto& [k, v] : f)
                        if (k != "NAME")
                            g->fields[k] = v;
                    g->seed = std::to_string(1 + Random()() % 2147483646u);
                    g->when = Now();
                    g->players.push_back(&s);
                    LeaveGame(s);
                    m_games[g->name] = g;
                    s.game = g;
                    fprintf(stderr, "[lobby] %s opens race %s\n", me.c_str(), g->name.c_str());
                    reply.push_back(Pack("gcre", GameRecord(*g, s)));
                    reply.push_back(UserRecord(s, s));
                }
                else if (command == "gjoi")
                {
                    auto it = m_games.find(Get(f, "NAME"));
                    if (it == m_games.end())
                        reply.push_back(Pack("gjoi", {}, "nfnd"));
                    else
                    {
                        std::shared_ptr<Game> g = it->second;
                        if (s.game != g)
                            LeaveGame(s);
                        if (std::find(g->players.begin(), g->players.end(), &s) == g->players.end())
                            g->players.push_back(&s);
                        s.game = g;
                        fprintf(stderr, "[lobby] %s joins race %s (%zu players)\n", me.c_str(), g->name.c_str(), g->players.size());
                        NotifyGame(*g, "+mgm", &s);
                        reply.push_back(Pack("gjoi", GameRecord(*g, s)));
                        reply.push_back(UserRecord(s, s));
                    }
                }
                else if (command == "gset")
                {
                    if (std::shared_ptr<Game> g = s.game)
                    {
                        for (auto& [k, v] : f)
                            if (k != "NAME")
                                g->fields[k] = v;
                        NotifyGame(*g, "+mgm", &s);
                        reply.push_back(Pack("gset", GameRecord(*g, s)));
                    }
                    else
                        reply.push_back(Pack("gset"));
                }
                else if (command == "gsta")
                {
                    if (std::shared_ptr<Game> g = s.game)
                    {
                        fprintf(stderr, "[lobby] race %s starts (%zu players)\n", g->name.c_str(), g->players.size());
                        NotifyGame(*g, "+ses", nullptr);
                    }
                    reply.push_back(Pack("gsta"));
                }
                else if (command == "glea" || command == "gdel")
                {
                    LeaveGame(s);
                    reply.push_back(Pack(command.c_str()));
                }
                else if (command == "auxi")
                {
                    s.auxi = Get(f, "TEXT");
                    reply.push_back(Pack("auxi", f));
                    if (auto it = m_rooms.find(s.room); it != m_rooms.end())
                    {
                        for (Session* o : Members(it->second))
                            if (o != &s)
                                Send(*o, UserRecord(s, *o, "+usr"));
                        reply.push_back(UserRecord(s, s, "+usr"));
                        for (auto& m : NotifySet(it->second, s, &s, false))
                            reply.push_back(std::move(m));
                    }
                }
                else if (command == "mesg")
                {
                    std::vector<Session*> to;
                    if (s.game)
                        for (Session* p : s.game->players)
                            if (p != &s)
                                to.push_back(p);
                    if (to.empty())
                        if (auto it = m_rooms.find(s.room); it != m_rooms.end())
                            for (Session* o : Members(it->second))
                                if (o != &s)
                                    to.push_back(o);
                    for (Session* o : to)
                        Send(*o, Pack("+msg", { { "N", me }, { "T", Get(f, "TEXT") }, { "F", Get(f, "ATTR") } }));
                    reply.push_back(Pack("mesg"));
                }
                else if (command == "snap")
                    reply.push_back(Pack("snap", { { "CHAN", Get(f, "CHAN", "0") }, { "START", Get(f, "START", "0") }, { "RANGE", "0" } }));
                else
                {
                    if (command != "onln" && command != "rank")
                        fprintf(stderr, "[lobby] %s: '%s' not known: answered empty\n", me.c_str(), command.c_str());
                    reply.push_back(Pack(command.c_str()));
                }
                for (auto& m : reply)
                    Send(s, m);
            }
        };

        std::atomic<Lobby*> s_lobby{ nullptr };
    }

    bool StartLobby()
    {
        static std::atomic<bool> started{ false };
        if (started.exchange(true))
            return s_lobby.load() != nullptr;
        auto* lobby = new Lobby;
        if (!lobby->Listen())
        {
            delete lobby;
            return false;
        }
        s_lobby = lobby;
        std::thread([lobby] { lobby->Run(); }).detach();
        return true;
    }

    int LobbyPlayers()
    {
        Lobby* l = s_lobby.load();
        return l ? l->Players() : 0;
    }
}
