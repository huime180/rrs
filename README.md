# RRS — Adaptive Refresh Rate (KernelSU / Magisk)

Adaptive refresh-rate module for Xiaomi HyperOS: it selects refresh-rate tiers for the current app and scene, supports per-app frame caps, touch sampling rate and 1 Hz AOD, and includes a KernelSU WebUI.

## Features

- Dynamic refresh-rate tiers (e.g. `185 → 120 → 60`) with per-tier dwell time
- Per-app and per-game frame caps, including dynamic tiers
- Low-brightness refresh-rate limit
- Touch sampling rate control (device-dependent)
- 1 Hz AOD
- WebUI for all settings

## Install

1. Download `rrs-<version>-ksu.zip` from [Releases](https://github.com/huime180/rrs/releases).
2. Flash it with KernelSU or Magisk.
3. Reboot.

## Update

Update checks use `update.json`; update through your root manager or the WebUI.

## Compatibility

Tested on Xiaomi Pad, Redmi K70 and Redmi K100 Pro Max. Unsupported devices fall back to the stock refresh-rate policy.
