# RRS 1.4.7

## Touch sampling
- Touch firmware patch now respects `ic_report_timer_coef` for normal/vsync modes
  - 240 report rate becomes 120/240 on 2x devices
  - 300 report rate becomes 150/300 on 2x devices
  - FTS devices without the coefficient keep direct 240/240
- Game and super modes stay direct: 240/240 and 300/300
- Original `/odm` firmware is read by a read-only mount or `fsck.erofs` extraction; `thp_stock` is refreshed on every install when the original partition is readable
- Removed `thp_rate_max`; the WebUI derives the maximum from `thp_stock`
- WebUI shows only the module firmware overlay, marks it effective, and annotates original values
- Empty firmware sources are skipped instead of writing zero-byte files
