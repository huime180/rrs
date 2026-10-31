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
- Low-brightness threshold follows the system brightness slider instead of the raw backlight percentage
