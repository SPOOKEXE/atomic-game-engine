# source export clock audit, 2026-10-07

## facts grug read

pinned Composer revision: `b69eca232217360cf1502ef0223523d818606652`.

- [animator](https://github.com/Ttanasart-pt/Pixel-Composer/blob/b69eca232217360cf1502ef0223523d818606652/scripts/animation_controller/animation_controller.gml): `setFrame` stores raw `real_frame`, then rounds `current_frame` by default (lines 68-71). `firstFrame` uses that default (line 101). `render` starts there, or at zero when simulating (lines 141-146). each step advances raw time by one before rounding (lines 193-194).
- [export](https://github.com/Ttanasart-pt/Pixel-Composer/blob/b69eca232217360cf1502ef0223523d818606652/scripts/node_export/node_export.gml): sequence and animation exports compare rounded `CURRENT_FRAME` against raw custom endpoints. every export requires zero remainder from `CURRENT_FRAME - range_start` modulo frame step (lines 922-935). default endpoints come from the animator (line 922).
- [modulo](https://github.com/Ttanasart-pt/Pixel-Composer/blob/b69eca232217360cf1502ef0223523d818606652/scripts/safe_operation/safe_operation.gml): `safe_mod` uses remainder directly, without rounding endpoints (lines 3-4).

native `GraphAuthoredExport.cpp` refuses fractional default, custom and selected-region endpoints. `GraphExportSession.cpp` schedules integer ticks. single prepared-surface exports already preserve signed fractional preview requests; sessions explicitly exclude single exports.

## deduction and next work

grug must derive the eligible rounded source cursor before planning files. sampling raw fractional endpoints would change source behavior. for example, a custom range starting at one-based 1.25 has zero-based origin .25; integer cursors never satisfy its step-one modulo filter.

this is source reading, not a licensed desktop capture. exact render callbacks, endpoint shutdown and empty-output publication still need matched observations. grug made no export scheduling change in this audit.
