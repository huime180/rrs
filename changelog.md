# RRS 1.4.9

## Installation
- Restrict installation to Xiaomi / Redmi / POCO devices
- Warn when no KernelSU metamodule is present and recommend hybrid_mount VFS mode; installation still continues
- Remove the brand check from the boot-time overlay builder

## Refresh rate
- Remove the "do not intervene" refresh-rate option from game and per-app settings
- Default game refresh policy is now the highest tier; legacy `off` entries follow the global policy
- Show the adaptive sampling explanation under the global refresh-rate tiers when adaptive is selected

## Touch sampling
- Per-app sampling follows global is labeled daily or game according to the entry type
- Sampling options are limited by the overall maximum and the current value stays visible
- Screen-on only re-applies per-app sampling; a dedicated screen logcat runs only for entries with per-app sampling
- Re-apply the current app when AppList changes
- Remove sampling command write tests and readback checks; a writable node is accepted as supported

## Other
- Default low-brightness threshold is 4%
- Force `support_aod_fullscreen` to true; leave `support_max_fps` unchanged
