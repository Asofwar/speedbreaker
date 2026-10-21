// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// XAM message apps (XMsg*). Only XMP (0xFA, the system music player, which
// games ask about custom soundtracks) is implemented, following Xenia's
// kernel/xam/apps/xmp_app.cc message by message. XGI (0xFB: achievements,
// sessions, stats) and XLiveBase (0xFC) messages fail with X_E_FAIL and a log
// line, as Xenia's unknown messages do, unless NFSMW_FAKE_LIVE=1 (below).
#include <stdafx.h>
#include <online/online.h>
#include "function.h"
#include "overlapped.h"
#include "xam.h"

#include <cpu/guest_thread.h>
#include <kernel/memory.h>

#include <mutex>
#include <set>

void NetOwnXnAddr(uint8_t* xnaddr);  // kernel/imports_net.cpp
uint32_t NetServiceAddress();

namespace
{
    constexpr uint32_t X_E_SUCCESS = 0;
    constexpr uint32_t X_E_FAIL = 0x80004005;
    constexpr uint32_t X_E_INVALIDARG = 0x80070057;
    constexpr uint32_t X_E_NOTFOUND = 0x80070490;
    constexpr uint32_t X_ERROR_IO_PENDING = 997;
    constexpr uint32_t X_ERROR_NOT_FOUND = 1168;

    // XMP notifications (XN_XMP_*).
    constexpr uint32_t kMsgStateChanged = 0x0A000001;
    constexpr uint32_t kMsgPlaybackBehaviorChanged = 0x0A000002;
    constexpr uint32_t kMsgPlaybackControllerChanged = 0x0A000003;

    struct Xmp
    {
        std::mutex mutex;
        uint32_t state = 0;             // 0 idle/stopped, 1 playing, 2 paused
        uint32_t playbackClient = 1;    // the title controls playback
        uint32_t playbackMode = 0;
        uint32_t repeatMode = 0;
        uint32_t unknownFlags = 0;
        float volume = 1.0f;
        std::unordered_map<uint32_t, uint32_t> playlists;  // handle -> song count
    } s_xmp;

    template<typename T>
    T* Guest(uint32_t address) { return static_cast<T*>(g_memory.Translate(address)); }

    uint32_t XmpDispatch(uint32_t message, uint32_t buffer, uint32_t length)
    {
        std::lock_guard lock(s_xmp.mutex);
        auto* args = Guest<be<uint32_t>>(buffer);
        switch (message)
        {
        case 0x00070002: // XMPPlayTitlePlaylist(client, storage, song)
        {
            uint32_t handle = *Guest<be<uint32_t>>(args[1]);
            if (!s_xmp.playlists.count(handle))
                return X_E_NOTFOUND;
            // Custom soundtracks aren't played; the title keeps its own music.
            fprintf(stderr, "[xmp] PlayTitlePlaylist %08X (not played)\n", handle);
            s_xmp.state = 1;
            XamNotifyEnqueueEvent(kMsgStateChanged, s_xmp.state);
            return X_E_SUCCESS;
        }
        case 0x00070003: // XMPContinue
            if (s_xmp.state == 2) s_xmp.state = 1;
            XamNotifyEnqueueEvent(kMsgStateChanged, s_xmp.state);
            return X_E_SUCCESS;
        case 0x00070004: // XMPStop
            s_xmp.state = 0;
            XamNotifyEnqueueEvent(kMsgStateChanged, s_xmp.state);
            return X_E_SUCCESS;
        case 0x00070005: // XMPPause
            if (s_xmp.state == 1) s_xmp.state = 2;
            XamNotifyEnqueueEvent(kMsgStateChanged, s_xmp.state);
            return X_E_SUCCESS;
        case 0x00070006: // XMPNext
        case 0x00070007: // XMPPrevious
            return X_E_NOTFOUND;
        case 0x00070008: // XMPSetPlaybackBehavior(client, mode, repeat, flags)
            s_xmp.playbackMode = args[1];
            s_xmp.repeatMode = args[2];
            s_xmp.unknownFlags = args[3];
            XamNotifyEnqueueEvent(kMsgPlaybackBehaviorChanged, 0);
            return X_E_SUCCESS;
        case 0x00070009: // XMPGetStatus(client, state*)
            *Guest<be<uint32_t>>(args[1]) = s_xmp.state;
            return X_E_SUCCESS;
        case 0x0007000B: // XMPGetVolume(client, float*)
            *Guest<be<float>>(args[1]) = s_xmp.volume;
            return X_E_SUCCESS;
        case 0x0007000C: // XMPSetVolume(client, float)
            s_xmp.volume = reinterpret_cast<be<float>*>(args)[1];
            return X_E_SUCCESS;
        case 0x0007000D: // XMPCreateTitlePlaylist(client, storage, size, songs, count, name, flags, songHandles*, handle*)
        {
            uint32_t storage = args[1];
            *Guest<be<uint32_t>>(args[8]) = storage;  // the handle is the storage pointer (Xenia)
            *Guest<be<uint32_t>>(storage) = storage;
            s_xmp.playlists[storage] = args[4];
            return X_E_SUCCESS;
        }
        case 0x0007000E: // XMPGetInfo: nothing is ever playing
            return X_E_FAIL;
        case 0x00070013: // XMPDeleteTitlePlaylist(client, storage)
            return s_xmp.playlists.erase(*Guest<be<uint32_t>>(args[1])) ? X_E_SUCCESS : X_E_NOTFOUND;
        case 0x0007001A: // XMPSetPlaybackController(client, controller, playbackClient)
            s_xmp.playbackClient = args[2];
            XamNotifyEnqueueEvent(kMsgPlaybackControllerChanged, !uint32_t(args[2]));
            return X_E_SUCCESS;
        case 0x0007001B: // XMPGetPlaybackController(client, controller*, locked*)
            *Guest<be<uint32_t>>(args[1]) = 0;
            *Guest<be<uint32_t>>(args[2]) = 0;
            return X_E_SUCCESS;
        case 0x00070029: // XMPGetPlaybackBehavior(client, mode*, repeat*, flags*)
            if (args[1]) *Guest<be<uint32_t>>(args[1]) = s_xmp.playbackMode;
            if (args[2]) *Guest<be<uint32_t>>(args[2]) = s_xmp.repeatMode;
            if (args[3]) *Guest<be<uint32_t>>(args[3]) = s_xmp.unknownFlags;
            return X_E_SUCCESS;
        case 0x0007002E: // storage size for a playlist of N songs
            *Guest<be<uint32_t>>(args[2]) = 4 + uint32_t(args[1]) * 128;
            return X_E_SUCCESS;
        }
        fprintf(stderr, "[xmp] unimplemented message %08X (buffer %08X, %u bytes)\n", message, buffer, length);
        return X_E_FAIL;
    }

    // NFSMW_FAKE_LIVE=1 (the game's own Xbox Live race against a stand-in EA
    // lobby server, NFSMW_NET_HOST; kernel/imports_net.cpp): the XAM apps'
    // Live messages answered as a signed-in console's are. Their buffers'
    // layouts are Xenia's (xgi_app.cc, xlivebase_app.cc); what NFS Most
    // Wanted needs of them is what victorgbd's NFSMW Recompiled found
    // (github.com/victorgbd/NFSMW-Recompiled-Mobile, docs/diario).
    bool FakeLive() { return online::Enabled(); }

    void LiveLogOnce(uint32_t app, uint32_t message, const char* what)
    {
        static std::mutex mutex;
        static std::set<uint64_t> seen;
        std::lock_guard lock(mutex);
        if (seen.insert(uint64_t(app) << 32 | message).second)
            fprintf(stderr, "[live] app %02X message %08X: %s\n", app, message, what);
    }

    // XGI (sessions, contexts, properties, stats). The game's matchmaking is
    // EA's lobby: the session here only has to exist. A host's XSessionCreate
    // gets its XSESSION_INFO (XNKID 8 bytes, the host's XNADDR 36, XNKEY 16)
    // and a nonce, which the game sends to the others through the lobby.
    uint32_t XgiDispatch(uint32_t message, uint32_t buffer, uint32_t length)
    {
        uint8_t* base = g_memory.base;
        if (message == 0x000B0010 && buffer)  // XSessionCreate
        {
            const uint32_t flags = PPC_LOAD_U32(buffer + 4), info = PPC_LOAD_U32(buffer + 20), nonce = PPC_LOAD_U32(buffer + 24);
            if ((flags & 1) && info)  // XSESSION_CREATE_HOST
            {
                static uint32_t count = 0;
                count++;
                uint8_t* p = base + info;
                uint8_t own[36];
                NetOwnXnAddr(own);
                const uint8_t kid[8] = { 0x80, own[0], own[1], own[2], own[3], 0x00, uint8_t(count >> 8), uint8_t(count) };
                memcpy(p, kid, 8);
                memcpy(p + 8, own, 36);
                for (int i = 0; i < 16; i++)
                    p[44 + i] = uint8_t(kid[i % 8] ^ (0x5A + i));
                if (nonce)
                    for (int i = 0; i < 8; i++)
                        base[nonce + i] = uint8_t(kid[i] ^ 0xA5);
                fprintf(stderr, "[live] XSessionCreate (host): XNKID %02X%02X%02X%02X%02X%02X%02X%02X\n", kid[0], kid[1], kid[2], kid[3], kid[4], kid[5],
                    kid[6], kid[7]);
            }
        }
        LiveLogOnce(0xFB, message, "success");
        return X_E_SUCCESS;
    }

    // XLiveBase: logon, NAT type, service addresses, presence. The game asks
    // for EA's service (0x45410004) and opens its gateway to the address it
    // gets: the stand-in server's. Without it its NetConn stays at "-svc"
    // and the game says the connection to Xbox Live was lost.
    uint32_t XLiveBaseDispatch(uint32_t message, uint32_t buffer, uint32_t length)
    {
        uint8_t* base = g_memory.base;
        switch (message)
        {
        case 0x00058004:  // the logon id (a bool)
        case 0x00058006:  // the NAT type: XONLINE_NAT_OPEN
            if (buffer)
                PPC_STORE_U32(buffer, 1);
            break;
        case 0x00058007:  // XOnlineGetServiceInfo(service id, XONLINE_SERVICE_INFO*): { id, IN_ADDR, port, reserved }
            if (length)
            {
                const uint32_t ip = NetServiceAddress();
                PPC_STORE_U32(length, buffer);
                memcpy(base + length + 4, &ip, 4);  // (network order, as an IN_ADDR)
                PPC_STORE_U16(length + 8, 0);
                PPC_STORE_U16(length + 10, 0);
                fprintf(stderr, "[live] XOnlineGetServiceInfo(%08X): the stand-in server\n", buffer);
            }
            break;
        case 0x0005800E:  // XUserMuteListQuery({ user, pad, XUID, result }, BOOL* muted): nobody is muted
            if (length)
                PPC_STORE_U32(length, 0);
            if (buffer)
                PPC_STORE_U32(buffer + 16, 0);
            break;
        case 0x00058020:  // the friends list's enumerator (CXLiveFriends::Enumerate): none, as Xenia
            LiveLogOnce(0xFC, message, "failed (no friends list)");
            return X_E_FAIL;
        default:
            break;
        }
        LiveLogOnce(0xFC, message, "success");
        return X_E_SUCCESS;
    }

    uint32_t Dispatch(uint32_t app, uint32_t message, uint32_t buffer, uint32_t length)
    {
        if (app == 0xFA)
            return XmpDispatch(message, buffer, length);
        if (FakeLive() && app == 0xFB)
            return XgiDispatch(message, buffer, length);
        if (FakeLive() && app == 0xFC)
            return XLiveBaseDispatch(message, buffer, length);
        if (app == 0xFB || app == 0xFC)
        {
            fprintf(stderr, "[xmsg] app %02X message %08X not implemented (returning X_E_FAIL)\n", app, message);
            return X_E_FAIL;
        }
        return X_E_NOTFOUND;
    }
}

uint32_t XMsgInProcessCall(uint32_t app, uint32_t message, uint32_t arg1, uint32_t arg2)
{
    return Dispatch(app, message, arg1, arg2);
}

uint32_t XMsgSystemProcessCall(uint32_t app, uint32_t message, uint32_t buffer, uint32_t length)
{
    return Dispatch(app, message, buffer, length);
}

uint32_t XMsgStartIORequest(uint32_t app, uint32_t message, XXOVERLAPPED* overlapped, uint32_t buffer, uint32_t length)
{
    uint32_t result = Dispatch(app, message, buffer, length);
    if (result == X_E_NOTFOUND)
    {
        fprintf(stderr, "[xmsg] XMsgStartIORequest: app %08X undefined\n", app);
        result = X_E_INVALIDARG;
        GuestThread::SetLastError(X_ERROR_NOT_FOUND);
    }
    if (overlapped != nullptr)
    {
        CompleteOverlapped(overlapped, result);
        result = X_ERROR_IO_PENDING;
    }
    if (result == 0 || result == X_ERROR_IO_PENDING)
        GuestThread::SetLastError(0);
    return result;
}

uint32_t XMsgCancelIORequest(XXOVERLAPPED* overlapped, uint32_t wait)
{
    return 0;  // every request completes immediately
}

GUEST_FUNCTION_HOOK(__imp__XMsgInProcessCall, XMsgInProcessCall);
GUEST_FUNCTION_HOOK(__imp__XMsgSystemProcessCall, XMsgSystemProcessCall);
GUEST_FUNCTION_HOOK(__imp__XMsgStartIORequest, XMsgStartIORequest);
GUEST_FUNCTION_HOOK(__imp__XMsgCancelIORequest, XMsgCancelIORequest);
