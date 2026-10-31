# RRS 1.4.5

## Low-brightness limit
- Low-brightness threshold is now a percentage of the brightness slider (0–100%), default 8%, 0 = off
- Low-brightness max refresh rate is now an inline Lock / Dynamic switch with a frame-rate picker, default Lock
  - Lock: only the cap tier is used at low brightness, minimizing switches
  - Dynamic: cap tier plus the existing lower tiers
- Low-brightness tiers never exceed the top tier and never add extra steps down
- Per-app frame caps are not affected by the low-brightness threshold
- Fixed hot reload for the low-brightness threshold, cap and mode

## Game detection
- Game import now prefers the Game Turbo database (`gamebooster.db`), avoiding false positives for video-toolbox apps (e.g. 红果免费短剧, 网易云) that share the `gamebooster_perf_level` mark
- Falls back to the settings list when the database is unavailable

## Refresh rate
- Animation boost target can be the device top refresh rate (default) or the top tier
- Animation monitoring now subscribes only to SurfaceFlinger logs and filters by pid; single-core usage drops from ~0.6% to ~0.2%

## WebUI
- Options use bottom sheets; UI scale, low-brightness cap and animation boost are picker sheets
- Wallpaper images are compressed automatically; no more "image too large" errors

## Distribution
- updateJson / zipUrl / changelog are routed through gh-proxy.com
