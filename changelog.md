# RRS 1.4.6

## Adaptive refresh rate
- Content sampling now runs every 0.5s and detects 45fps video (1.5x) for 90Hz output
- Video switches no longer poison the sampler with 0fps or lock the display at 60Hz
- Per-app dynamic/auto frame rates survive WebUI reloads and daemon restarts
- Per-app delays now align to the tail of the global delay list when the app has fewer tiers

## Animation boost
- Animation boost now applies to both fixed and dynamic tiers
- Dynamic mode skips animation boost for 1s after touch; fixed mode ignores touch
- Boost and restore share the same animation window; touch cancels an active boost without restoring

## WebUI and diagnostics
- Home page tier display includes `auto`
- `--probe` prints `auto` instead of `-1`
- Low-brightness detection reads the backlight sysfs first, avoiding a settings process every 2s

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
