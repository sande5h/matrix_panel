-- Menu bar control for the HUB75 matrix panel (~/matrix_panel).
--
-- Left click toggles the panel's screen. The dropdown picks one directly.
-- The panel answers to matrix-panel.local via mDNS, so this survives DHCP
-- handing it a new address.

local M = {}

local MDNS_HOST = "matrix-panel.local:8088"

-- mDNS resolves inconsistently here: ping finds the panel, curl often times
-- out on the same name. Every response carries the panel's own IP, so the
-- first success pins it and later calls skip name resolution entirely. A
-- failure drops back to the name, which is what recovers after DHCP moves it.
local pinnedHost = nil
local function host()
    return pinnedHost or MDNS_HOST
end

local menu = hs.menubar.new()
local lastScreen = nil

-- The icon carries the state: one glance at the menu bar says what the panel
-- is showing, without opening anything.
local ICON_DIR  = os.getenv("HOME") .. "/.hammerspoon/icons/"
local ICON_SIZE = 18   -- the menu bar is 22pt tall; 18 leaves it room to breathe

-- Template images let macOS tint the artwork to match the menu bar, so they
-- invert correctly in dark mode instead of disappearing into the background.
-- The source PNGs are three different sizes, hence the explicit setSize.
local function icon(file)
    local img = hs.image.imageFromPath(ICON_DIR .. file)
    if not img then return nil end
    return img:setSize({ w = ICON_SIZE, h = ICON_SIZE }):template(true)
end

local ICONS = {
    clock  = icon("clock.png"),
    claude = icon("claude.png"),
    video  = icon("video.png"),
}

-- Text stands in if an image is missing, so a bad path degrades to a working
-- menu bar item rather than an invisible one.
local GLYPHS = { clock = "🕒", claude = "📊", nowplaying = "🎵",
                 video = "🎞", offline = "▪️" }

local function setState(screen)
    lastScreen = screen
    if not menu or M.uploading then return end   -- an upload owns the title

    local img = screen and ICONS[screen]
    if img then
        menu:setIcon(img)
        menu:setTitle("")
    else
        menu:setIcon(nil)
        menu:setTitle(GLYPHS[screen or "offline"] or GLYPHS.offline)
    end

    menu:setTooltip(screen and ("matrix panel: " .. screen) or "matrix panel: unreachable")
end

-- Every request lands here. `path` is the endpoint, and the panel answers with
-- the same JSON for all of them, so one handler keeps the menu bar in step
-- whatever we asked for.
local function call(path, andThen)
    -- The panel's HTTP server handles one request at a time, so a status poll
    -- during an upload would just sit in the queue behind several megabytes.
    if M.uploading then return end
    hs.http.asyncGet("http://" .. host() .. path, nil, function(status, body)
        if status ~= 200 then
            pinnedHost = nil          -- fall back to the name next time
            setState(nil)
            return
        end
        local ok, data = pcall(hs.json.decode, body)
        if ok and data and data.screen then
            if data.ip and data.ip ~= "0.0.0.0" then
                pinnedHost = data.ip .. ":8088"
            end
            setState(data.screen)
            if andThen then andThen(data) end
        end
    end)
end

local function toggle()
    call("/toggle")
end

local function select(name)
    call("/screen?s=" .. name)
end

local function refresh()
    call("/status")
end

-- ---------------------------------------------------------------- uploading
--
-- The panel takes both firmware and clips over HTTP, so neither needs a cable
-- or a terminal. curl does the transfer -- it streams from disk rather than
-- reading the file into Lua, which matters for a 3 MB image -- and its
-- progress meter is parsed back out of stderr to drive the menu bar title.
--
-- The panel draws its own progress bar at the same time, so the two agree.

local CURL    = "/usr/bin/curl"
local REPO    = os.getenv("HOME") .. "/matrix_panel"
local ENCODER = REPO .. "/tools/encode_video.py"
local FIRMWARE_DEFAULT = REPO .. "/build/matrix_panel.bin"

-- Anything else is assumed to be an already encoded blob.
local VIDEO_EXT = { mp4 = true, mov = true, mkv = true, webm = true,
                    m4v = true, avi = true, gif = true }

local uploadTask = nil

local function notify(title, text)
    hs.notify.new({ title = title, informativeText = text,
                    withdrawAfter = 6 }):send()
end

-- Title instead of icon while something is in flight: the icon means "this is
-- what the panel is showing", and during an upload that is not what it means.
local function showProgress(label, pct)
    if not menu then return end
    menu:setIcon(nil)
    menu:setTitle(pct and string.format("%s %d%%", label, pct) or label)
    menu:setTooltip("matrix panel: " .. label)
end

local function uploadDone(ok, what, detail)
    M.uploading = nil
    uploadTask = nil
    notify(ok and (what .. " uploaded") or (what .. " failed"), detail or "")
    -- An OTA reboots the panel, so give it time to come back before asking.
    hs.timer.doAfter(ok and what == "firmware" and 12 or 1, refresh)
end

local function transfer(what, path, url)
    M.uploading = what
    showProgress(what:upper(), 0)

    uploadTask = hs.task.new(CURL,
        function(rc, out, err)
            if rc == 0 then
                uploadDone(true, what, out and out:sub(1, 200) or "")
            else
                -- --fail-with-body keeps the panel's own error text on stdout.
                local why = (out and out:match("%S") and out)
                         or (err and err:match("[^\r\n]+$")) or ("curl exit " .. rc)
                uploadDone(false, what, why:sub(1, 200))
            end
        end,
        function(_, _, stderr)
            -- curl -# writes "####   45.0%" repeatedly, separated by \r.
            local pct = stderr and stderr:match("([%d%.]+)%%[^%%]*$")
            if pct then showProgress(what:upper(), math.floor(tonumber(pct))) end
            return true
        end,
        { "-#", "--fail-with-body", "--max-time", "600",
          "-H", "Content-Type: application/octet-stream",
          "--data-binary", "@" .. path, url })

    uploadTask:start()
end

-- ffmpeg first if the file is not already a blob. The defaults here match what
-- the panel wants; --rotate, --focus and the rest are worth the command line.
local function encodeThenUpload(src)
    local out = os.tmpname() .. ".bin"
    M.uploading = "video"
    showProgress("ENCODING")

    hs.task.new(ENCODER,
        function(rc, stdout, stderr)
            if rc ~= 0 then
                M.uploading = nil
                uploadDone(false, "encode",
                           (stderr or stdout or ""):match("[^\r\n]+$") or "")
                return
            end
            transfer("video", out, "http://" .. host() .. "/video?play=1")
        end,
        { src, out, "--fps", "30", "--mjpeg", "--quality", "9", "--fit", "crop" })
        :start()
end

local function uploadVideo()
    if M.uploading then return end
    local pick = hs.dialog.chooseFileOrFolder(
        "Choose a clip: a .bin from encode_video.py, or any video to encode",
        os.getenv("HOME") .. "/Downloads", true, false, false,
        { "bin", "mp4", "mov", "mkv", "webm", "m4v", "avi", "gif" })
    if not pick or not pick["1"] then return end

    local path = pick["1"]
    local ext = (path:match("%.([^.]+)$") or ""):lower()
    if VIDEO_EXT[ext] then
        encodeThenUpload(path)
    else
        transfer("video", path, "http://" .. host() .. "/video?play=1")
    end
end

local function flashFirmware()
    if M.uploading then return end

    local path = FIRMWARE_DEFAULT
    if not hs.fs.attributes(path) then
        local pick = hs.dialog.chooseFileOrFolder(
            "Choose a firmware image", REPO, true, false, false, { "bin" })
        if not pick or not pick["1"] then return end
        path = pick["1"]
    end

    -- Worth a confirmation: this one reboots the panel. It cannot brick it --
    -- the image lands in the spare slot and the bootloader rolls back if it
    -- cannot get its server up -- but it does interrupt whatever is on screen.
    local size = hs.fs.attributes(path).size
    if hs.dialog.blockAlert("Flash the panel?",
            string.format("%s\n%.1f MB -- the panel reboots when it lands.",
                          path, size / 1048576),
            "Flash", "Cancel") ~= "Flash" then
        return
    end
    transfer("firmware", path, "http://" .. host() .. "/ota")
end


if menu then
    setState(nil)
    menu:setClickCallback(toggle)
    menu:setMenu(function()
        local function item(name, label)
            return {
                title = label,
                checked = (lastScreen == name),
                fn = function() select(name) end,
            }
        end
        return {
            { title = "Toggle", fn = toggle },
            { title = "-" },
            item("clock", "Clock"),
            item("claude", "Claude quota"),
            item("nowplaying", "Now playing"),
            item("video", "Video"),
            { title = "-" },
            { title = "-" },
            { title = M.uploading and "Uploading..." or "Upload video...",
              disabled = M.uploading ~= nil, fn = uploadVideo },
            { title = "Flash firmware...",
              disabled = M.uploading ~= nil, fn = flashFirmware },
            { title = "-" },
            { title = "Open control page", fn = function()
                hs.urlevent.openURL("http://" .. host() .. "/")
            end },
            { title = "Refresh", fn = refresh },
        }
    end)

    -- Poll slowly so the icon still tracks changes made from the web page or
    -- by a reboot, without chattering at the panel.
    M.timer = hs.timer.doEvery(30, refresh)
    refresh()
end

-- ---------------------------------------------------------------- now playing
--
-- macOS 26 gates MediaRemote behind an entitlement, so nowplaying-cli and
-- friends read nothing. Two sources are left, and neither needs private APIs:
--
--   Music.app  -- scriptable, so title, artist and position all come for free.
--   A browser  -- not scriptable for media, but Chromium-family browsers mark
--                 the window producing audio with a speaker in its title, so
--                 that string doubles as "this is the one that is playing".
--
-- Reading a window title never disturbs anything on screen, which the Control
-- Center accessibility route cannot promise: those labels only exist while the
-- popover is open, so polling them would flash it open every few seconds.
--
-- Every AppleScript call goes through hs.task, off the main thread. A
-- synchronous osascript that blocks -- an app busy, a consent prompt waiting --
-- would otherwise freeze all of Hammerspoon, App Lock included.

local PROBE  = os.getenv("HOME") .. "/.hammerspoon/nowplaying.applescript"
local FFMPEG = "/opt/homebrew/bin/ffmpeg"

local lastPayload, lastPush = nil, 0
local lastArt, lastArtPush = nil, 0

-- The panel's font is 5x7 ASCII, so anything outside it renders as a gap.
-- Strip it here rather than shipping blanks to the panel.
local function clean(title)
    if not title then return nil end
    local s = title:gsub("[\128-\255]", "")          -- emoji, ellipsis, non-latin
    s = s:gsub("%s*%-%s*YouTube%s*$", "")
    s = s:gsub("^%s+", ""):gsub("%s+$", ""):gsub("%s%s+", " ")
    return (s ~= "" and s) or nil
end

local function push(np)
    local body = np and hs.json.encode(np) or hs.json.encode({ stopped = true })

    -- On change, and otherwise every 10 s: the panel forgets a track it has not
    -- heard about for 30 s, so a keepalive stops it blanking something that is
    -- still playing.
    local now = os.time()
    if body == lastPayload and (now - lastPush) < 10 then return end
    lastPayload, lastPush = body, now

    hs.http.asyncPost("http://" .. host() .. "/nowplaying", body,
                      { ["Content-Type"] = "application/json" }, function() end)
end

-- Artwork goes over its own endpoint as raw RGB565: ffmpeg fetches, crops to
-- square, scales and converts, and curl streams the 4.6 KiB straight to the
-- panel. Piping through sh keeps the binary out of Lua entirely.
--
-- Re-sent every two minutes even when unchanged, because a panel that rebooted
-- has forgotten it and nothing else would tell us.
local function pushArt(url)
    if not url or url == "" then return end
    local now = os.time()
    if url == lastArt and (now - lastArtPush) < 120 then return end
    lastArt, lastArtPush = url, now

    local cmd = string.format(
        "%s -v error -y -i %q -frames:v 1 " ..
        "-vf 'scale=48:48:force_original_aspect_ratio=increase,crop=48:48' " ..
        "-f rawvideo -pix_fmt rgb565le - | " ..
        "%s -s -X POST --data-binary @- " ..
        "-H 'Content-Type: application/octet-stream' %q",
        FFMPEG, url, CURL, "http://" .. host() .. "/nowplaying/art")

    hs.task.new("/bin/sh", nil, { "-c", cmd }):start()
end

-- Music.app knows exactly what it is playing, so prefer it when it is running.
-- Guarded by application.get so AppleScript never launches it.
local function fromMusic()
    if not (hs.application.get("Music") and hs.itunes.isRunning()) then return nil end
    if hs.itunes.getPlaybackState() ~= hs.itunes.state_playing then return nil end
    local title = clean(hs.itunes.getCurrentTrack())
    if not title then return nil end
    return {
        title    = title,
        artist   = clean(hs.itunes.getCurrentArtist()) or "",
        position = math.floor(hs.itunes.getPosition() or 0),
        duration = math.floor(hs.itunes.getDuration() or 0),
        playing  = true,
    }
end

local function pushNowPlaying()
    if M.uploading then return end   -- do not queue behind a multi-MB POST
    local m = fromMusic()
    if m then push(m) return end

    hs.task.new("/usr/bin/osascript", function(rc, out)
        if rc == 0 and out and out:match("%S") then
            local ok, d = pcall(hs.json.decode, out)
            if ok and d and d.title then
                local title = clean(d.title)
                if title then
                    push({ title    = title,
                           artist   = clean(d.artist) or "",
                           position = d.position or 0,
                           duration = d.duration or 0,
                           playing  = d.playing and true or false })
                    pushArt(d.art)
                    return
                end
            end
        end
        push(nil)
    end, { PROBE }):start()
end

M.npTimer = hs.timer.doEvery(3, pushNowPlaying)


return M
