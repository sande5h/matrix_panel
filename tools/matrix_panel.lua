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

-- The icons carry the state: one glance at the menu bar says what the panel
-- is showing, without opening anything.
local ICONS = { clock = "🕒", claude = "📊", video = "🎞", offline = "▪️" }

local function setState(screen)
    lastScreen = screen
    if not menu then return end
    menu:setTitle(ICONS[screen] or ICONS.offline)
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

return M
