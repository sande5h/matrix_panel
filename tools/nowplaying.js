// Read the page's Media Session metadata -- the same source that fills the
// browser's own media popover. Returns "" when the tab is not playing media.
(function () {
  var m = navigator.mediaSession && navigator.mediaSession.metadata;
  if (!m || !m.title) return "";
  var v = document.querySelector("video, audio");
  var art = "";
  if (m.artwork && m.artwork.length) {
    art = m.artwork[m.artwork.length - 1].src;   // last entry is the largest
  }
  return JSON.stringify({
    title: m.title || "",
    artist: m.artist || "",
    art: art,
    position: v ? Math.floor(v.currentTime || 0) : 0,
    duration: v && isFinite(v.duration) ? Math.floor(v.duration) : 0,
    playing: v ? !v.paused : false
  });
})()
