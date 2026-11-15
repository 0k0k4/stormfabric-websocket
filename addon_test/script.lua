-- In-game test for the websocket mod. Paste into the script.lua of an addon (Addon
-- Editor), enable the addon in a new world. Works without the mod: `websocket` is nil.
--
-- Chat commands:
--   ?ws test                    connect to wss://echo.websocket.org and send a message
--   ?ws connect <url> [options] e.g. ?ws connect ws://127.0.0.1:8765 protocol=chat
--   ?ws send <text>             send on the current connection
--   ?ws state                   state + pending events
--   ?ws close                   close the current connection
--   ?ws version

local current = nil
local test_payload = nil

local function say(text, peer)
    server.announce("[ws]", text, peer or -1)
end

function onCreate(is_world_create)
    if websocket then
        say("websocket " .. websocket.version() .. " loaded")
    else
        say("websocket mod not available")
    end
end

function onTick(game_ticks)
    if not websocket then return end
    while true do
        local id, kind, data, extra = websocket.poll()
        if not id then break end
        if kind == "open" then
            say("#" .. id .. " open")
            if test_payload and id == current then websocket.send(id, test_payload) end
        elseif kind == "message" then
            if #data > 200 then data = data:sub(1, 200) .. "... (" .. #data .. " bytes)" end
            say("#" .. id .. " message" .. (extra and " (binary)" or "") .. ": " .. data)
            if test_payload and data == test_payload then
                say("TEST OK: echo received")
                test_payload = nil
                websocket.close(id, 1000, "test done")
            end
        elseif kind == "close" then
            say("#" .. id .. " closed " .. tostring(extra) .. " " .. tostring(data))
            if id == current then current = nil end
        elseif kind == "error" then
            say("#" .. id .. " error " .. tostring(extra) .. ": " .. tostring(data))
        end
    end
end

local function connect(url, options, peer)
    if current then websocket.close(current) end
    local id, err = websocket.connect(url, options)
    if not id then
        say("connect failed: " .. tostring(err), peer)
        return
    end
    current = id
    say("#" .. id .. " connecting to " .. url, peer)
end

function onCustomCommand(full_message, peer_id, is_admin, is_auth, command, sub, ...)
    if command ~= "?ws" then return end
    if not websocket then
        say("websocket mod not available", peer_id)
        return
    end
    local args = {...}
    if sub == "test" then
        test_payload = "stormfabric " .. tostring(server.getTimeMillisec and server.getTimeMillisec() or 0)
        connect("wss://echo.websocket.org/", nil, peer_id)
    elseif sub == "connect" and args[1] then
        test_payload = nil
        connect(args[1], args[2], peer_id)
    elseif sub == "send" and current then
        local text = full_message:match("^%S+%s+%S+%s+(.*)$") or ""
        say("send: " .. tostring(websocket.send(current, text)), peer_id)
    elseif sub == "state" then
        if current then
            say("#" .. current .. " " .. websocket.state(current) .. ", pending " .. websocket.pending(current), peer_id)
        else
            say("no connection", peer_id)
        end
    elseif sub == "close" and current then
        say("close: " .. tostring(websocket.close(current, 1000, "bye")), peer_id)
    elseif sub == "version" then
        say(websocket.version(), peer_id)
    else
        say("usage: ?ws test | connect <url> [options] | send <text> | state | close | version", peer_id)
    end
end
