# WebSocket — StormFabric mod

Full WebSocket client (`ws://` and `wss://`, any URL) for Stormworks Lua scripts:
**addons/missions** (`script.lua`) and **microcontrollers**. Networking is done by WinHTTP
(Windows 8+) in separate threads: a Lua call never blocks the game.

| | Addons (`script.lua`) | Microcontrollers |
|---|---|---|
| Build 1.15.23 | ✅ | ✅ |
| Build 1.15.18 | — | ✅ |

Tested in game on 2026-11-15 (v0.3.0): local `ws://` echo, `wss://echo.websocket.org`,
microcontroller with a screen.

Requires the [StormFabric](https://github.com/0k0k4/StormFabric) mod loader installed in
the game.

## Installation

From a [release](https://github.com/0k0k4/stormfabric-websocket/releases):

1. Close Stormworks.
2. Extract the zip into `<game>\mods\` (you get `mods\websocket\mod.json` and
   `sf_websocket.dll`).
3. Enable **WebSocket** in the in-game **MODS** menu (it is disabled by default), or add a
   `websocket=1` line to `mods\_modloader.ini`.

From source:

1. Close Stormworks.
2. `build.bat` (MSVC x64), then `install.bat <game_folder>`: copies `mod.json` and
   `sf_websocket.dll` to `<game>\mods\websocket\` and adds `websocket=1` to
   `mods\_modloader.ini`.

Launch through `Launcher.exe`. The `StormworksModLoader.log` file should contain
`websocket 0.3.0 ready` and `libraries: addon=yes microcontroller=yes`.

> The DLL must **never** be named `websocket.dll`: that is the name of a Windows DLL that
> WinHTTP loads for the WebSocket protocol. The mod refuses to start if its name exists in
> `System32` (see StormFabric's
> [`AGENTS.md`](https://github.com/0k0k4/StormFabric/blob/main/AGENTS.md), rule 7).

## Lua API

Always test the table so the script keeps working without the mod: `if websocket then … end`.

| Function | Returns |
|---|---|
| `websocket.connect(url [, options])` | `id`, or `nil, error` |
| `websocket.send(id, data [, binary])` | `true` / `false` |
| `websocket.poll([id])` | `id, type, data, extra`, or nothing |
| `websocket.receive(id)` | `data, binary`, or nothing |
| `websocket.state(id)` | `"connecting"`, `"open"`, `"closing"` or `"closed"` |
| `websocket.close(id [, code [, reason]])` | `true` / `false` |
| `websocket.pending(id)` | number of pending events |
| `websocket.version()` | `"0.3.0"` |

**Events** (`poll`):

| `type` | `data` | `extra` |
|---|---|---|
| `"open"` | — | — |
| `"message"` | content (byte string, NUL included) | `true` if binary |
| `"error"` | readable message | code (Win32/WinHTTP or HTTP status) |
| `"close"` | reason | close code (1006 = dropped without a clean close) |

An error is always followed by a `close`. A connection is released when `poll` returns its
`close`; `state` then returns `"closed"`.

**Options** for `connect`, as a `"key=value;key=value"` string:

| Key | Value | Effect |
|---|---|---|
| `protocol` | text | `Sec-WebSocket-Protocol` header |
| `header` | `Name: value` | extra HTTP header (repeatable; no `;` in the value) |
| `insecure` | `0` / `1` | `wss`: accept an invalid or self-signed certificate |
| `timeout` | 1–120 s | DNS/connect/handshake timeout (10 s by default) |
| `proxy` | `auto` / `direct` / `system` | `auto`: automatic proxy, then direct on a non-network failure |

### Example — addon

```lua
local sock
function onCreate()
    if websocket then sock = websocket.connect("wss://echo.websocket.org/") end
end

function onTick()
    if not websocket then return end
    while true do
        local id, kind, data, extra = websocket.poll()
        if not id then break end
        if kind == "open" then websocket.send(id, "hello")
        elseif kind == "message" then server.announce("ws", data)
        elseif kind == "close" then sock = nil end
    end
end
```

### Example — microcontroller

See `addon_test/mc_test.lua` (state and last message on a screen, sends every 2 s).

## Isolation and lifetime

- Each VM (an addon, a microcontroller) only sees **its own** connections: on the first
  call, the mod stores a numeric token `websocket.__owner` in the VM's `websocket` table.
  Do not change this field. If a script removes the global `websocket` table, its
  connections move to a shared owner.
- A connection nobody queries (`poll`, `send`, `state`…) for `idle_timeout` seconds is
  closed: this is what cleans up after a destroyed VM (despawned vehicle,
  `?reload_scripts`). **Call `poll` every tick.**
- When the world closes (no addon VM left), all connections are closed.
- Unloading the mod (MODS menu) closes everything and waits for its threads to end.

## Configuration

File created on first launch: `<game>\mods\websocket\config\websocket.cfg.bin`
(`key=value` text, edit it with the game closed; an invalid value makes the whole file
ignored, with the reason in the log).

| Key | Default | Effect |
|---|---|---|
| `allow_hosts` | `*` | allowed hosts, comma-separated: `*`, `host`, `*.domain` |
| `max_connections` | 32 | simultaneous connections (all scripts combined) |
| `idle_timeout` | 30 | seconds without a call before closing |
| `max_message_bytes` | 1048576 | maximum size of a received message (above: close 1009) |
| `max_queued_events` | 256 | pending events per connection (above: messages dropped + `error`) |
| `max_pending_send_bytes` | 4194304 | bytes waiting to be sent per connection |
| `log_connections` | 1 | write each connection to the log |
| `microcontrollers` | 1 | `0`: library for addons only |

## Security

Any script that receives the table can open an outbound connection, including the Lua of
a **Workshop vehicle**. The mod is disabled by default (`mod.json`), logs every connection
(host and port only, never the path or headers) and lets you restrict hosts
(`allow_hosts`) or turn microcontrollers off (`microcontrollers=0`). In multiplayer,
addons run on the host; microcontrollers run on every player who has the mod.

## Troubleshooting

| Symptom | Cause |
|---|---|
| `websocket` is `nil` | mod disabled, or script created before the mod loaded (reload the world) |
| `error 8 … not enough memory` | a DLL named `websocket.dll` is loaded in the game (old version of the mod) |
| `error 12007` / `12029` | name not found / server unreachable |
| `error 12175` | certificate rejected (`insecure=1` for a test server) |
| `error 403` (or another status) | the server refused the WebSocket upgrade |
| `host not allowed` | host missing from `allow_hosts` |
| connection closes by itself after ~30 s | the script no longer calls `poll` (`idle_timeout`) |

## Development

- `include/`: StormFabric SDK headers (MIT license, `include/LICENSE-StormFabric`). To build
  against another version: `set SF_SDK=<StormFabric>\include` before `build.bat`.
- `src/ws_client.*`: WinHTTP client, independent of the SDK. `src/ws_registry.*`: ids,
  owners, limits, config. `src/ws_mod.cpp`: entry point and Lua API.
- Offline tests (start `python tests/echo_server.py` first, without `-I`):
  `build\ws_selftest.exe [--online]` (client) and `build\mod_test.exe` (Lua API through a
  fake host that follows the loader's rules).
- In-game test: `addon_test/script.lua` (`?ws` commands) and `addon_test/mc_test.lua`.
