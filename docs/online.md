# Online play

Most Wanted on the Xbox 360 has online races but no System Link: players find each other
through EA's lobby servers, which were switched off long ago. This fork brings the mode back.
One player hosts: their copy of SpeedBreaker runs a small stand-in for EA's lobby, and the
others join it. The race itself goes directly between the players, as it did on the 360.

Up to four players. Quick Match and creating a game work. Leaderboards stay empty.

## Setting it up

1. Settings > Online > Xbox Live: **Host** on one copy, **Join** on the others. Restart the
   game after changing it.
2. Main menu > Xbox Live. The host creates a game (Race, then *New game* when the search finds
   nothing). The others pick Race with the same mode and race type, and Quick Match finds the
   host's game.

Your name in the lobby comes from `online.toml` in the user folder
(`~/.local/share/speedbreaker/online.toml` on Linux and the Deck). The file is created on the
first online start:

```toml
gamertag = "Racer"   # up to 15 letters, digits or spaces
server = ""          # a host's address; empty: look for one on the local network
transport = "direct" # or "steam"
```

Offline the profile is still called "Player", the same as before, so your saves don't change.

## On one network

Nothing to set up: a joining copy finds the host by a broadcast on the local network (UDP
10902). If your router blocks broadcasts, put the host's address in `server`.

## Over the internet

There are two ways.

**A virtual network (Tailscale, ZeroTier).** Every player installs it and joins the same
network. Then it works as on one network: ZeroTier passes broadcasts, so the host is found by
itself; with Tailscale, put the host's Tailscale address in `server`. Both relay the traffic
when a direct connection isn't possible.

**Steam (`transport = "steam"`).** No addresses and nothing to install besides Steam. The host
creates a lobby on Steam, the others find it there, and all traffic goes through Valve's relay
network. You need:

- Steam running on every computer (on the Deck it always is);
- Steam's library `libsteam_api.so`. SpeedBreaker doesn't ship it, it takes it from any Steam
  game you have installed (most native Linux games have one). If none is found, point
  `NFSMW_STEAM_API` at the file.

Over Steam the game shows up as Spacewar (AppID 480), Valve's free sample app that every Steam
account has. That's the price of not needing a store page of our own. The lobby is public and
marked as SpeedBreaker's, so Quick Match finds games of strangers who play the same way.
`NFSMW_STEAM_LOBBY=friends` makes the host's lobby friends-only.

Without Steam or a virtual network the host must forward its ports: TCP 30960 and 10901, UDP
3658 and 6000. The other players need UDP 3658 and 6000 reachable as well.

## Environment variables

| Variable | What it does |
|---|---|
| `NFSMW_ONLINE=off\|host\|join` | the mode for this run |
| `NFSMW_GAMERTAG=<name>` | the gamertag |
| `NFSMW_NET_HOST=<address>` | the host to join |
| `NFSMW_ONLINE_TRANSPORT=direct\|steam` | the transport |
| `NFSMW_STEAM_API=<path>` | Steam's `libsteam_api.so` |
| `NFSMW_STEAM_LOBBY=friends` | a friends-only Steam lobby |
| `NFSMW_FAKE_LIVE=1` | sign in to Xbox Live without a lobby of our own, for a lobby server run separately (such as [nfsmw-server-lan](https://github.com/victorgbd/nfsmw-server-lan)) |
| `NFSMW_NET_LOG=1` | log the network calls |

## How it works

- The game talks to EA's lobby (the "Aries" protocol of EA's DirtySock library) over TCP.
  `runtime/online/lobby.cpp` answers it in the copy that hosts.
- Signing in to Xbox Live, sessions, QoS and the rest of the console's online services are
  answered in `runtime/kernel/xmsg.cpp` and `imports_net.cpp`. The console's network calls go
  to ordinary sockets.
- DirtySock starts as with `-nosecure`. In the 360's secure mode it sends all traffic through
  the Xbox Live gateway, which we don't have.
- The Steam transport (`runtime/online/steam.cpp`) gives each player a virtual address
  10.88.x.y. It sends UDP packets as Steam messages and tunnels the lobby's TCP connection.

Thanks to [victorgbd](https://github.com/victorgbd): their
[NFSMW Recompiled](https://github.com/victorgbd/NFSMW-Recompiled-Mobile) and
[nfsmw-server-lan](https://github.com/victorgbd/nfsmw-server-lan) were the first to get this
game racing again without EA, and their notes showed the way. The network layer on host
sockets started from [AndhikaFW's fork](https://github.com/AndhikaFW/speedbreaker).

## По-русски

На Xbox 360 в Most Wanted есть онлайн-гонки, но нет System Link: соперники ищутся через лобби
EA, а эти серверы давно выключены. В форке режим вернули. Один игрок хостит: его копия
SpeedBreaker держит у себя небольшое лобби вместо EA, остальные подключаются к нему. Сама гонка
идёт напрямую между игроками, как на консоли. До четырёх человек. Работают быстрый поиск и
создание игры. Таблицы рекордов пустые.

Как играть:

1. Настройки > Online > Xbox Live: на одной копии **Host**, на остальных **Join**. После
   смены перезапустите игру.
2. Главное меню > Xbox Live. Хост создаёт игру: «Заезд», затем «Новая игра», когда поиск ничего
   не найдёт. Остальные выбирают тот же режим и тип гонки, и быстрый поиск находит игру хоста.

Имя в лобби задаётся в `online.toml` в папке пользователя (`~/.local/share/speedbreaker/` на
Linux и Deck), там же адрес сервера и транспорт. Без онлайна профиль по-прежнему называется
«Player», сохранения не меняются.

- **Одна сеть.** Ничего настраивать не нужно: подключающийся находит хоста сам (широковещание,
  UDP 10902). Если роутер его режет, впишите адрес хоста в `server`.
- **Через интернет, вариант 1.** Tailscale или ZeroTier у всех игроков. С ZeroTier хост
  находится сам. С Tailscale впишите в `server` его адрес в Tailscale.
- **Через интернет, вариант 2.** `transport = "steam"`. Нужен запущенный Steam (на Deck он
  всегда есть) и любая установленная игра со Steam-библиотекой `libsteam_api.so`. Хост создаёт
  лобби в Steam, остальные находят его там, трафик идёт через серверы Valve. Адресов и портов
  не нужно. В Steam игра при этом отображается как Spacewar (AppID 480).
- **Без Steam и VPN** хосту нужно пробросить порты TCP 30960 и 10901, UDP 3658 и 6000.
