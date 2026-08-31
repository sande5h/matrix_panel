-- Finds the browser window marked as producing audio and reads its active
-- tab's Media Session metadata -- the same source that fills the browser's own
-- media popover.
--
-- Only the marked window is touched. Executing JavaScript across every tab
-- takes minutes on a busy browser because suspended tabs must be woken first,
-- so the speaker in the window title is what keeps this fast.
--
-- Only Helium is handled. Terms like "active tab" are resolved against the
-- app's dictionary when this script is compiled, so a block for a browser that
-- is not installed is a parse error even if it would never run. To add one,
-- install the browser first, then copy the tell block.
--
-- Needs View > Developer > Allow JavaScript from Apple Events in Helium.

set jsPath to (POSIX path of (path to home folder)) & ".hammerspoon/nowplaying.js"
set js to read POSIX file jsPath as «class utf8»
set mark to "🔊"

tell application "System Events"
    if not (exists process "Helium") then return ""
end tell

tell application "Helium"
    -- The window actively producing audio, if there is one.
    repeat with w in windows
        if (name of w) contains mark then
            with timeout of 5 seconds
                try
                    set r to execute (active tab of w) javascript js
                    if r is not "" and r is not missing value then return r
                end try
            end timeout
        end if
    end repeat

    -- Nothing is making noise, but a paused track is still worth showing and
    -- carries no marker. One call per window rather than per tab: the active
    -- tabs are already awake, so this stays fast.
    repeat with w in windows
        with timeout of 5 seconds
            try
                set r to execute (active tab of w) javascript js
                if r is not "" and r is not missing value then return r
            end try
        end timeout
    end repeat
end tell

return ""
