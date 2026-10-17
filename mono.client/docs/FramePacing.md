# Client frame pacing

The client defaults to uncapped presentation and requests immediate mode from
the GPU backend. The fixed simulation rate remains 60 Hz by default and can be
set separately with `--tick-rate`.

- `--max-fps 144` limits presentation to 144 FPS. Zero removes the cap.
- `--vsync` requests display pacing and ignores the presentation cap.
- `--uncapped` overrides configured VSync. Combining it with `--vsync` is an error.

Persistent settings are `client.uncapped` (default `true`) and `client.max-fps`
(default `0`). Config files, environment variables and `--flag` establish their
values; explicit `--uncapped`, `--vsync` and `--max-fps` options override them.

Startup logs report the requested present mode and effective cap. If immediate
mode is unsupported, the renderer warns and retains the supported present mode.
An unchanged image submits no presentation.

Headless data-factory and inherited presentation hosts keep a 60 FPS service
cadence unless `--max-fps` supplies a nonzero rate. This does not change their
fixed simulation rate.
