-- Menu bar control for the HUB75 matrix panel (~/matrix_panel).
--
-- Left click toggles the panel's screen. The dropdown picks one directly.
-- The panel answers to matrix-panel.local via mDNS, so this survives DHCP
-- handing it a new address.

local M = {}

local HOST    = "matrix-panel.local:8088"
local TIMEOUT = 3   -- seconds; the panel is on the LAN, so this is generous

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
local GLYPHS = { clock = "🕒", claude = "📊", video = "🎞", offline = "▪️" }

local function setState(screen)
    lastScreen = screen
    if not menu then return end

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
    hs.http.asyncGet("http://" .. HOST .. path, nil, function(status, body)
        if status ~= 200 then
            setState(nil)
            return
        end
        local ok, data = pcall(hs.json.decode, body)
        if ok and data and data.screen then
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
            item("video", "Video"),
            { title = "-" },
            { title = "Open control page", fn = function()
                hs.urlevent.openURL("http://" .. HOST .. "/")
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

local BROWSERS = { "Helium", "Safari", "Google Chrome", "Arc", "Brave Browser" }
local AUDIO_MARK = "\u{1F50A}"    -- the speaker a playing window carries

local lastPayload, lastPush = nil, 0

-- The panel's font is 5x7 ASCII, so anything outside it renders as a gap.
-- Strip it here rather than shipping blanks to the panel.
local function clean(title)
    if not title then return nil end
    local s = title:gsub("[\128-\255]", "")          -- emoji, ellipsis, accents
    s = s:gsub("%s*%-%s*YouTube%s*$", "")             -- the site name is noise
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

    hs.http.asyncPost("http://" .. HOST .. "/nowplaying", body,
                      { ["Content-Type"] = "application/json" }, function() end)
end

-- Walks the browser list one at a time, asynchronously, stopping at the first
-- window marked as playing. Newline delimiters rather than the default comma,
-- because titles contain commas.
local function pollBrowsers(i)
    i = i or 1
    local name = BROWSERS[i]
    if not name then push(nil) return end
    if not hs.application.get(name) then return pollBrowsers(i + 1) end

    local script = string.format([[tell application %q
        set AppleScript's text item delimiters to linefeed
        return (name of every window) as text
    end tell]], name)

    hs.task.new("/usr/bin/osascript", function(rc, out)
        if rc == 0 and out then
            for line in out:gmatch("[^\n]+") do
                if line:find(AUDIO_MARK, 1, true) then
                    local title = clean(line)
                    if title then
                        push({ title = title, artist = name,
                               position = 0, duration = 0, playing = true })
                        return
                    end
                end
            end
        end
        pollBrowsers(i + 1)
    end, { "-e", script }):start()
end

-- Music.app knows exactly what it is playing, so prefer it when it is running.
-- Guarded by application.get so AppleScript never launches it.
local function pushNowPlaying()
    if hs.application.get("Music") and hs.itunes.isRunning()
       and hs.itunes.getPlaybackState() == hs.itunes.state_playing then
        local title = clean(hs.itunes.getCurrentTrack())
        if title then
            push({ title    = title,
                   artist   = clean(hs.itunes.getCurrentArtist()) or "",
                   position = math.floor(hs.itunes.getPosition() or 0),
                   duration = math.floor(hs.itunes.getDuration() or 0),
                   playing  = true })
            return
        end
    end
    pollBrowsers(1)
end

M.npTimer = hs.timer.doEvery(3, pushNowPlaying)

return M
