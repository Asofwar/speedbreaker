// SpeedBreaker. GPL-3.0-or-later (see COPYING).
//
// A stand-in for Steam's libsteam_api.so, for testing online play over Steam
// (runtime/online/steam.cpp) with two copies of the game on one machine,
// where one Steam account can't be two players. It exports the flat API
// calls SpeedBreaker uses and fakes them:
//
//   - the SteamID: MOCK_STEAM_ID (a number), else made from NFSMW_GAMERTAG;
//     the persona name: MOCK_STEAM_NAME, else NFSMW_GAMERTAG;
//   - lobbies: files in /tmp/mock-steam (MOCK_STEAM_DIR), one a lobby;
//   - messages: UDP on 127.0.0.1, each SteamID at a port of its own;
//   - callbacks and call results: manual dispatch, as the real one.
//
// Build: cc -O1 -shared -fPIC -o libsteam_api.so mock_steam_api.c
// Use:   NFSMW_STEAM_API=/path/to/libsteam_api.so NFSMW_ONLINE_TRANSPORT=steam
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define EXPORT __attribute__((visibility("default")))

typedef struct { int32_t user; int32_t callback; uint8_t* param; int32_t size; } CallbackMsg;
typedef struct { uint8_t bytes[136]; } Identity;
typedef struct { void* data; int32_t size; uint32_t connection; Identity peer; int64_t pad[8]; } Message;

static uint64_t self_id;
static char persona[64];
static int sock = -1;
static char dir[256];

// pending callbacks (a ring), and finished calls' results
typedef struct { int id; int size; uint8_t data[160]; } Pending;
static Pending queue[256];
static int head, tail;
static Pending current;
typedef struct { uint64_t call; int id; int size; uint8_t data[64]; } Result;
static Result results[64];
static uint64_t next_call = 1;

// received messages, by channel
typedef struct { uint64_t from; int channel; int size; uint8_t* data; } Received;
static Received inbox[1024];
static int inbox_count;
static uint64_t known[64];
static int known_count;

static uint16_t PortOf(uint64_t id) { return (uint16_t)(42000 + (id % 20000)); }

static void Queue(int id, const void* data, int size)
{
    Pending* p = &queue[tail++ % 256];
    p->id = id;
    p->size = size;
    memset(p->data, 0, sizeof(p->data));
    memcpy(p->data, data, (size_t)size);
}

static uint64_t Finish(int id, const void* data, int size)
{
    Result* r = &results[next_call % 64];
    r->call = next_call++;
    r->id = id;
    r->size = size;
    memcpy(r->data, data, (size_t)size);
    uint8_t done[16] = { 0 };  // SteamAPICallCompleted_t { uint64 call; int32 callback; uint32 size }
    memcpy(done, &r->call, 8);
    memcpy(done + 8, &id, 4);
    memcpy(done + 12, &size, 4);
    Queue(703, done, 16);
    return r->call;
}

EXPORT int SteamAPI_InitFlat(char* error)
{
    const char* v = getenv("MOCK_STEAM_ID");
    const char* name = getenv("MOCK_STEAM_NAME");
    if (!name)
        name = getenv("NFSMW_GAMERTAG");
    if (!name)
        name = "MockPlayer";
    snprintf(persona, sizeof(persona), "%s", name);
    if (v)
        self_id = strtoull(v, NULL, 10);
    else
    {
        uint64_t h = 1469598103934665603ull;
        for (const char* c = name; *c; c++)
            h = (h ^ (uint8_t)*c) * 1099511628211ull;
        self_id = 0x0110000100000000ull | (h & 0xFFFFFFull);
    }
    const char* d = getenv("MOCK_STEAM_DIR");
    snprintf(dir, sizeof(dir), "%s", d ? d : "/tmp/mock-steam");
    mkdir(dir, 0777);
    sock = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = { 0 };
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(PortOf(self_id));
    if (bind(sock, (struct sockaddr*)&a, sizeof(a)) != 0)
    {
        snprintf(error, 1024, "mock: port %u taken", PortOf(self_id));
        return 1;
    }
    fprintf(stderr, "[mock-steam] %s, SteamID %llu, port %u\n", persona, (unsigned long long)self_id, PortOf(self_id));
    return 0;
}

EXPORT int32_t SteamAPI_GetHSteamPipe(void) { return 1; }
EXPORT void SteamAPI_ManualDispatch_Init(void) {}

static void Drain(void)
{
    uint8_t buffer[65536];
    for (;;)
    {
        ssize_t n = recv(sock, buffer, sizeof(buffer), MSG_DONTWAIT);
        if (n < 12 || inbox_count >= 1024)
            return;
        Received* r = &inbox[inbox_count++];
        memcpy(&r->from, buffer, 8);
        memcpy(&r->channel, buffer + 8, 4);
        r->size = (int)n - 12;
        r->data = malloc((size_t)r->size + 1);
        memcpy(r->data, buffer + 12, (size_t)r->size);
        bool seen = false;
        for (int i = 0; i < known_count; i++)
            seen |= known[i] == r->from;
        if (!seen && known_count < 64)
        {
            known[known_count++] = r->from;
            Identity id = { 0 };  // SteamNetworkingMessagesSessionRequest_t { identity }
            memcpy(id.bytes, &r->from, 8);
            Queue(1251, &id, sizeof(id) > 160 ? 160 : (int)sizeof(id));
        }
    }
}

EXPORT void SteamAPI_ManualDispatch_RunFrame(int32_t pipe) { Drain(); }

EXPORT bool SteamAPI_ManualDispatch_GetNextCallback(int32_t pipe, CallbackMsg* out)
{
    if (head == tail)
        return false;
    current = queue[head++ % 256];
    out->user = 1;
    out->callback = current.id;
    out->param = current.data;
    out->size = current.size;
    return true;
}

EXPORT void SteamAPI_ManualDispatch_FreeLastCallback(int32_t pipe) {}

EXPORT bool SteamAPI_ManualDispatch_GetAPICallResult(int32_t pipe, uint64_t call, void* out, int size, int expected, bool* failed)
{
    for (int i = 0; i < 64; i++)
        if (results[i].call == call)
        {
            memcpy(out, results[i].data, (size_t)(size < results[i].size ? size : results[i].size));
            *failed = false;
            return true;
        }
    *failed = true;
    return false;
}

static int dummy;
EXPORT void* SteamAPI_SteamUser_v023(void) { return &dummy; }
EXPORT uint64_t SteamAPI_ISteamUser_GetSteamID(void* self) { return self_id; }
EXPORT void* SteamAPI_SteamFriends_v018(void) { return &dummy; }
EXPORT const char* SteamAPI_ISteamFriends_GetPersonaName(void* self) { return persona; }
EXPORT void* SteamAPI_SteamMatchmaking_v009(void) { return &dummy; }
EXPORT void* SteamAPI_SteamNetworkingMessages_SteamAPI_v002(void) { return &dummy; }

// a lobby file: its owner's id, and its data lines
static void LobbyPath(uint64_t lobby, char* out, size_t size) { snprintf(out, size, "%s/lobby-%llu", dir, (unsigned long long)lobby); }

EXPORT uint64_t SteamAPI_ISteamMatchmaking_CreateLobby(void* self, int type, int members)
{
    uint64_t lobby = 0x0186000000000000ull | (self_id & 0xFFFFFFFFull);
    char path[512];
    LobbyPath(lobby, path, sizeof(path));
    FILE* f = fopen(path, "w");
    if (f)
    {
        fprintf(f, "%llu\n", (unsigned long long)self_id);
        fclose(f);
    }
    uint8_t created[12] = { 0 };  // LobbyCreated_t, packed to 4: { int32 result; uint64 lobby }
    int32_t ok = 1;
    memcpy(created, &ok, 4);
    memcpy(created + 4, &lobby, 8);
    return Finish(513, created, 12);
}

EXPORT bool SteamAPI_ISteamMatchmaking_SetLobbyData(void* self, uint64_t lobby, const char* key, const char* value)
{
    char path[512];
    LobbyPath(lobby, path, sizeof(path));
    FILE* f = fopen(path, "a");
    if (!f)
        return false;
    fprintf(f, "%s=%s\n", key, value);
    fclose(f);
    return true;
}

static uint64_t OwnerOf(uint64_t lobby)
{
    char path[512];
    LobbyPath(lobby, path, sizeof(path));
    FILE* f = fopen(path, "r");
    unsigned long long owner = 0;
    if (f)
    {
        if (fscanf(f, "%llu", &owner) != 1)
            owner = 0;
        fclose(f);
    }
    return owner;
}

EXPORT uint64_t SteamAPI_ISteamMatchmaking_GetLobbyOwner(void* self, uint64_t lobby) { return OwnerOf(lobby); }

static uint64_t found[16];
static int found_count;

EXPORT void SteamAPI_ISteamMatchmaking_AddRequestLobbyListStringFilter(void* self, const char* key, const char* value, int cmp) {}
EXPORT void SteamAPI_ISteamMatchmaking_AddRequestLobbyListDistanceFilter(void* self, int distance) {}

EXPORT uint64_t SteamAPI_ISteamMatchmaking_RequestLobbyList(void* self)
{
    found_count = 0;
    DIR* d = opendir(dir);
    if (d)
    {
        struct dirent* e;
        while ((e = readdir(d)) && found_count < 16)
            if (strncmp(e->d_name, "lobby-", 6) == 0)
            {
                uint64_t lobby = strtoull(e->d_name + 6, NULL, 10);
                if (OwnerOf(lobby) != self_id)
                    found[found_count++] = lobby;
            }
        closedir(d);
    }
    uint32_t count = (uint32_t)found_count;  // LobbyMatchList_t { uint32 count }
    return Finish(510, &count, 4);
}

EXPORT uint64_t SteamAPI_ISteamMatchmaking_GetLobbyByIndex(void* self, int i) { return i < found_count ? found[i] : 0; }

EXPORT uint64_t SteamAPI_ISteamMatchmaking_JoinLobby(void* self, uint64_t lobby)
{
    uint8_t enter[20] = { 0 };  // LobbyEnter_t, packed to 4: { uint64 lobby; uint32 perms; bool locked; uint32 response }
    uint32_t response = OwnerOf(lobby) ? 1 : 2;
    memcpy(enter, &lobby, 8);
    memcpy(enter + 16, &response, 4);
    return Finish(504, enter, 20);
}

EXPORT int SteamAPI_ISteamMatchmaking_GetNumLobbyMembers(void* self, uint64_t lobby) { return 2; }
EXPORT uint64_t SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex(void* self, uint64_t lobby, int i) { return i == 0 ? OwnerOf(lobby) : self_id; }

EXPORT void SteamAPI_SteamNetworkingIdentity_SetSteamID64(Identity* i, uint64_t id)
{
    memset(i, 0, sizeof(*i));
    memcpy(i->bytes, &id, 8);
}

EXPORT uint64_t SteamAPI_SteamNetworkingIdentity_GetSteamID64(Identity* i)
{
    uint64_t id;
    memcpy(&id, i->bytes, 8);
    return id;
}

EXPORT int SteamAPI_ISteamNetworkingMessages_SendMessageToUser(void* self, const Identity* to, const void* data, uint32_t size, int flags, int channel)
{
    uint64_t id;
    memcpy(&id, to->bytes, 8);
    uint8_t buffer[65536];
    if (size > sizeof(buffer) - 12)
        return 2;
    memcpy(buffer, &self_id, 8);
    memcpy(buffer + 8, &channel, 4);
    memcpy(buffer + 12, data, size);
    struct sockaddr_in a = { 0 };
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(PortOf(id));
    return sendto(sock, buffer, size + 12, 0, (struct sockaddr*)&a, sizeof(a)) < 0 ? 2 : 1;
}

EXPORT bool SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser(void* self, const Identity* who) { return true; }

EXPORT int SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel(void* self, int channel, Message** out, int max)
{
    Drain();
    int n = 0;
    for (int i = 0; i < inbox_count && n < max;)
        if (inbox[i].channel == channel)
        {
            Message* m = calloc(1, sizeof(Message));
            m->data = inbox[i].data;
            m->size = inbox[i].size;
            memcpy(m->peer.bytes, &inbox[i].from, 8);
            out[n++] = m;
            memmove(&inbox[i], &inbox[i + 1], sizeof(Received) * (size_t)(inbox_count - i - 1));
            inbox_count--;
        }
        else
            i++;
    return n;
}

EXPORT void SteamAPI_SteamNetworkingMessage_t_Release(Message* m)
{
    free(m->data);
    free(m);
}
