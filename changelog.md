# RRS 1.4.8

## Install and boot
- Installation no longer reads or patches stock firmware; overlays are generated during `post-fs-data` before modules are mounted
- Stock file hashes are cached so unchanged firmware skips regeneration; system updates rebuild it automatically
- Touch firmware respects `ic_report_timer_coef`; game and super modes stay direct
- `fpsList` keeps the stock tiers, adds the maximum rate and 1 Hz
- `device_features` injection applies only to Xiaomi/Redmi/POCO devices

## WebUI
- Restored the maximum touch sampling rate on the home panel
- Sampling options are limited to the detected maximum
- "Merge" system-default import now appends only and never overwrites existing app settings
