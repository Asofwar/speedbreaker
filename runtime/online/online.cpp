// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
#include "online.h"
#include "steam.h"
#include <user/paths.h>
#include <user/settings.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <pwd.h>
#include <sys/socket.h>
#include <unistd.h>

#if __has_include(<toml++/toml.hpp>)
#include <toml++/toml.hpp>
#else
#include "../../tools/XenonRecomp/thirdparty/tomlplusplus/include/toml++/toml.hpp"
#endif

namespace online
{
    namespace
    {
        constexpr uint16_t kSearchPort = 10902;  // the local network's search (UDP), as nfsmw-server-lan's
        constexpr const char* kSearch = "NFSMW_DISCOVER";
        constexpr const char* kAnswer = "NFSMW_SERVER";

        std::mutex s_mutex;
        std::condition_variable s_found;
        std::string s_configGamertag, s_configServer;
        bool s_configRead = false;
        uint32_t s_discovered = 0;  // network order; 0: none yet
        std::string s_discoveredName;
        bool s_started = false;

        std::filesystem::path ConfigPath() { return GetUserPath() / "online.toml"; }

        // A gamertag as the 360 has them: letters, digits and spaces, 15 at most.
        std::string Clean(std::string name)
        {
            std::string out;
            for (char c : name)
                if (std::isalnum(uint8_t(c)) || (c == ' ' && !out.empty()))
                    out += c;
            while (!out.empty() && out.back() == ' ')
                out.pop_back();
            return out.substr(0, 15);
        }

        std::string SystemUser()
        {
            const char* v = std::getenv("USER");
            if (!v || !*v)
                if (const passwd* p = getpwuid(getuid()))
                    v = p->pw_name;
            std::string name = Clean(v ? v : "");
            // the Steam Deck's own user is "deck": not much of a gamertag
            if (name.empty() || name == "deck" || name == "root")
                return "";
            return name;
        }

        std::string Quoted(const std::string& v)
        {
            std::string out = "\"";
            for (char c : v)
                if (c == '"' || c == '\\')
                    (out += '\\') += c;
                else if (uint8_t(c) >= 0x20)
                    out += c;
            return out + "\"";
        }

        void ReadConfig()
        {
            s_configRead = true;
            const auto path = ConfigPath();
            std::error_code ec;
            if (!std::filesystem::exists(path, ec))
            {
                // a starting point to edit, with the defaults spelled out
                std::ofstream f(path);
                if (f)
                    f << "# SpeedBreaker online play (Settings > Online picks Host or Join).\n"
                         "# gamertag: your name in Xbox Live's lobby (15 letters, digits or spaces).\n"
                         "# server: a host's address to join; empty: find one on the local network.\n"
                         "# (Settings > Online edits both; Connection there picks Direct or Steam.)\n"
                      << "gamertag = " << Quoted(SystemUser()) << "\n"
                      << "server = \"\"\n";
                return;
            }
            try
            {
                toml::table t = toml::parse_file(path.string());
                s_configGamertag = Clean(t["gamertag"].value_or(std::string()));
                s_configServer = t["server"].value_or(std::string());
            }
            catch (const toml::parse_error& e)
            {
                fprintf(stderr, "[online] %s doesn't parse (%s): its defaults\n", path.c_str(), std::string(e.description()).c_str());
            }
        }

        // An address or a host name, in network order; 0 when it doesn't resolve.
        uint32_t Resolve(const std::string& text)
        {
            if (text.empty())
                return 0;
            in_addr a{};
            if (inet_pton(AF_INET, text.c_str(), &a) == 1)
                return a.s_addr;
            addrinfo hints{}, *list = nullptr;
            hints.ai_family = AF_INET;
            if (getaddrinfo(text.c_str(), nullptr, &hints, &list) != 0 || !list)
            {
                fprintf(stderr, "[online] the server \"%s\" doesn't resolve\n", text.c_str());
                return 0;
            }
            const uint32_t v = reinterpret_cast<sockaddr_in*>(list->ai_addr)->sin_addr.s_addr;
            freeaddrinfo(list);
            return v;
        }

        // Join: ask the local network for a host until one answers. The search
        // goes to the broadcast address and to this machine (a host here).
        void SearchLoop()
        {
            const int fd = socket(AF_INET, SOCK_DGRAM, 0);
            if (fd < 0)
                return;
            int one = 1;
            setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
            if (std::getenv("NFSMW_NET_ADDR"))  // (two copies on one machine: each from its own address)
            {
                sockaddr_in me{};
                me.sin_family = AF_INET;
                me.sin_addr.s_addr = htonl(NetLocalAddressHost());
                bind(fd, reinterpret_cast<sockaddr*>(&me), sizeof(me));
            }
            fprintf(stderr, "[online] looking for a host on the local network\n");
            for (int round = 0;; round++)
            {
                // 255.255.255.255 leaves by the default route only: also every
                // network's own broadcast address (a VPN's, such as ZeroTier's)
                std::vector<in_addr_t> targets = { htonl(INADDR_BROADCAST), htonl(INADDR_LOOPBACK) };
                ifaddrs* list = nullptr;
                if (getifaddrs(&list) == 0)
                {
                    for (ifaddrs* i = list; i; i = i->ifa_next)
                        if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET && (i->ifa_flags & IFF_BROADCAST) && i->ifa_broadaddr)
                            targets.push_back(reinterpret_cast<sockaddr_in*>(i->ifa_broadaddr)->sin_addr.s_addr);
                    freeifaddrs(list);
                }
                for (in_addr_t to : targets)
                {
                    sockaddr_in a{};
                    a.sin_family = AF_INET;
                    a.sin_port = htons(kSearchPort);
                    a.sin_addr.s_addr = to;
                    sendto(fd, kSearch, strlen(kSearch), 0, reinterpret_cast<sockaddr*>(&a), sizeof(a));
                }
                pollfd p{ fd, POLLIN, 0 };
                while (poll(&p, 1, round < 10 ? 500 : 3000) > 0)
                {
                    char buffer[512];
                    sockaddr_in from{};
                    socklen_t l = sizeof(from);
                    const ssize_t n = recvfrom(fd, buffer, sizeof(buffer) - 1, 0, reinterpret_cast<sockaddr*>(&from), &l);
                    if (n <= 0)
                        continue;
                    buffer[n] = 0;
                    if (strncmp(buffer, kAnswer, strlen(kAnswer)) != 0)
                        continue;
                    std::string name;
                    if (const char* k = strstr(buffer, "NAME="))
                        name.assign(k + 5, strcspn(k + 5, "\r\n"));
                    {
                        std::lock_guard lock(s_mutex);
                        s_discovered = from.sin_addr.s_addr;
                        s_discoveredName = name;
                    }
                    s_found.notify_all();
                    fprintf(stderr, "[online] found %s's lobby at %s\n", name.c_str(), inet_ntoa(from.sin_addr));
                    close(fd);
                    return;
                }
            }
        }
    }

    std::string CleanGamertag(const std::string& name) { return Clean(name); }

    std::string ConfiguredGamertag()
    {
        std::lock_guard lock(s_mutex);
        if (!s_configRead)
            ReadConfig();
        return s_configGamertag;
    }

    std::string ConfiguredServer()
    {
        std::lock_guard lock(s_mutex);
        if (!s_configRead)
            ReadConfig();
        return s_configServer;
    }

    bool Configure(const std::string& gamertag, const std::string& server)
    {
        std::string name = Clean(gamertag), host = server;
        while (!host.empty() && std::isspace(uint8_t(host.back())))
            host.pop_back();
        while (!host.empty() && std::isspace(uint8_t(host.front())))
            host.erase(0, 1);
        const auto path = ConfigPath();
        const auto temporary = path.string() + ".new";
        {
            std::ofstream f(temporary, std::ios::trunc);
            if (!f)
                return false;
            f << "# SpeedBreaker online play (Settings > Online).\n"
                 "# gamertag: your name in Xbox Live's lobby (15 letters, digits or spaces).\n"
                 "# server: a host's address to join; empty: find one on the local network.\n"
              << "gamertag = " << Quoted(name) << "\n"
              << "server = " << Quoted(host) << "\n";
            if (!f.flush())
                return false;
        }
        std::error_code ec;
        std::filesystem::rename(temporary, path, ec);
        if (ec)
            return false;
        std::lock_guard lock(s_mutex);
        s_configRead = true;
        s_configGamertag = name;
        s_configServer = host;
        fprintf(stderr, "[online] online.toml: gamertag \"%s\", server \"%s\"\n", name.c_str(), host.c_str());
        return true;
    }

    Mode GetMode()
    {
        const int32_t v = settings::GetInt(settings::Id::OnlineMode);
        return v == 1 ? Mode::Host : v == 2 ? Mode::Join : Mode::Off;
    }

    bool Enabled()
    {
        static const bool enabled = GetMode() != Mode::Off || std::getenv("NFSMW_FAKE_LIVE") != nullptr;
        return enabled;
    }

    const std::string& Gamertag()
    {
        static const std::string name = [] {
            if (!Enabled())
                return std::string("Player");
            if (const char* v = std::getenv("NFSMW_GAMERTAG"); v && *v)
                if (std::string c = Clean(v); !c.empty())
                    return c;
            {
                std::lock_guard lock(s_mutex);
                if (!s_configGamertag.empty())
                    return s_configGamertag;
            }
            if (steam::Active())
                if (std::string c = Clean(steam::PersonaName()); !c.empty())
                    return c;
            std::string user = SystemUser();
            if (!user.empty())
                return user;
            // No name to go by (a Steam Deck's user is "deck"): one of this
            // machine's own, so two of them in a lobby (and their XUIDs,
            // made from it) differ.
            std::string id;
            for (const char* path : { "/etc/machine-id", "/var/lib/dbus/machine-id" })
                if (std::ifstream f(path); f && std::getline(f, id) && !id.empty())
                    break;
            uint32_t h = 2166136261u;
            for (char c : id.empty() ? std::to_string(getpid()) : id)
                h = (h ^ uint8_t(c)) * 16777619u;
            char name[16];
            snprintf(name, sizeof(name), "Racer%04X", h & 0xFFFF);
            return std::string(name);
        }();
        return name;
    }

    uint32_t ServerAddress()
    {
        if (const char* v = std::getenv("NFSMW_NET_HOST"); v && *v)
            if (uint32_t a = Resolve(v))
                return a;
        std::string server;
        {
            std::lock_guard lock(s_mutex);
            server = s_configServer;
        }
        if (steam::Active() && GetMode() == Mode::Join)
        {
            if (uint32_t host = steam::HostAddress(20))
                return htonl(host);
            fprintf(stderr, "[online] no Steam lobby found (yet)\n");
        }
        else if (GetMode() == Mode::Join)
        {
            if (uint32_t a = Resolve(server))
                return a;
            std::unique_lock lock(s_mutex);
            s_found.wait_for(lock, std::chrono::seconds(5), [] { return s_discovered != 0; });
            if (s_discovered)
                return s_discovered;
            fprintf(stderr, "[online] no host found on the local network (yet)\n");
        }
        in_addr a{};
        inet_pton(AF_INET, "127.0.0.1", &a);  // Host: our own lobby
        return a.s_addr;
    }

    std::vector<Address> LocalAddresses()
    {
        std::vector<Address> out;
        ifaddrs* list = nullptr;
        if (getifaddrs(&list) != 0)
            return out;
        for (ifaddrs* i = list; i; i = i->ifa_next)
        {
            if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !(i->ifa_flags & IFF_UP) || (i->ifa_flags & IFF_LOOPBACK))
                continue;
            const std::string name = i->ifa_name ? i->ifa_name : "";
            // a container's or a VM's own network: no player reaches it
            if (name.rfind("docker", 0) == 0 || name.rfind("br-", 0) == 0 || name.rfind("veth", 0) == 0 || name.rfind("virbr", 0) == 0 ||
                name.rfind("lxc", 0) == 0 || name.rfind("podman", 0) == 0)
                continue;
            const in_addr a = reinterpret_cast<sockaddr_in*>(i->ifa_addr)->sin_addr;
            const uint32_t h = ntohl(a.s_addr);
            std::string kind = "LAN";
            if (name.rfind("tailscale", 0) == 0 || (h & 0xFFC00000u) == 0x64400000u)  // 100.64.0.0/10
                kind = "Tailscale";
            else if (name.rfind("zt", 0) == 0)
                kind = "ZeroTier";
            out.push_back({ kind, inet_ntoa(a) });
        }
        freeifaddrs(list);
        return out;
    }

    uint32_t KnownServerAddress()
    {
        if (const char* v = std::getenv("NFSMW_NET_HOST"); v && *v)
            return Resolve(v);
        if (GetMode() != Mode::Join)
            return 0;
        std::lock_guard lock(s_mutex);
        if (!s_configServer.empty())
        {
            in_addr a{};
            if (inet_pton(AF_INET, s_configServer.c_str(), &a) == 1)
                return a.s_addr;
        }
        return s_discovered;
    }

    void Start()
    {
        {
            std::lock_guard lock(s_mutex);
            if (std::exchange(s_started, true))
                return;
        }
        const Mode mode = GetMode();
        if (mode == Mode::Off && !std::getenv("NFSMW_FAKE_LIVE"))
            return;
        {
            std::lock_guard lock(s_mutex);
            ReadConfig();
        }
        const bool overSteam = settings::GetInt(settings::Id::OnlineTransport) == 1;
        if (mode != Mode::Off && overSteam && !steam::Start(mode == Mode::Host))
            fprintf(stderr, "[online] no Steam: direct addresses instead\n");
        fprintf(stderr, "[online] %s%s, as \"%s\"\n", mode == Mode::Host ? "hosting" : mode == Mode::Join ? "joining" : "a lobby run apart (NFSMW_FAKE_LIVE)",
            steam::Active() ? " over Steam" : "", Gamertag().c_str());
        if (mode == Mode::Host)
            StartLobby();
        else if (mode == Mode::Join && !steam::Active() && !std::getenv("NFSMW_NET_HOST") && Resolve(s_configServer) == 0)
            std::thread(SearchLoop).detach();
    }
}
