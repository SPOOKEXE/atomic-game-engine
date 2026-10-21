# Weather performance audit

## Setup

Captured 2026-10-15 on an RTX 4090 with Vulkan driver 580.173.02. The native
client used the `dev` preset, a CPU build at `-O0`, uncapped at 1280 x 720, with
profiling enabled. Each run used one view, with no portals or mirrors. The
FrameGraph summary covers the final approximately five seconds of a
`--profile-seconds 15` run. All runs used `--capture <build artifact>` and
`--frames 100000` to keep the same diagnostic scene-target path as the
exploratory headless runs. Profiling ended before the capture frame, so no
capture file was written. The native presentation path still used an offscreen
scene texture and an extra composition step, as described in
[`Client.cpp`](../mono.client/src/Client.cpp#L4647). On and off runs used the
same path, which makes their paired delta illustrative, but these figures are
not ordinary client FPS measurements.

Each case has one weather-off and one weather-on run. The scene copies of
[`Weather.luau`](../mono.engine/examples/assets/scripts/Weather.luau) selected
the preset and camera at startup and initialized authored light, cloud and fog
values immediately. Off disabled `weather.WeatherEnabled`, `weather.Enabled`,
`lowBank.Enabled` and `ruinMist.Enabled`. On enabled them, including
precipitation and weather responses. Geometry, authored sky, atmosphere, light
and post effects were otherwise the same. Standard output was captured in RAM;
no raw benchmark output files were kept. Desktop GPU activity was present, so
these single paired runs are illustrative development-build measurements, not
optimized release guarantees. All six runs exited successfully. The weather
work guard showed no active weather GPU work when off and an integration pass
when on.

## Frame timings

Values are FrameGraph wall-clock frame durations in milliseconds. The on-run
window and frame count are listed for context.

| Case | Off mean | Off p99 | On mean | On median | On p99 | On max | On frames / window |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| Overcast ground | 4.467 | 7.545 | 9.410 | 9.362 | 15.897 | 18.533 | 518 / 4.991 s |
| Dense stratus interior | 4.600 | 7.578 | 23.201 | 22.224 | 34.897 | 42.357 | 213 / 4.997 s |
| Ground fog valley | 4.712 | 7.644 | 9.520 | 9.200 | 16.921 | 17.671 | 511 / 4.995 s |

At 60 FPS the frame budget is 16.67 ms. The dense interior run exceeded that
budget on average in this capture. Its cloud ray-marching pass averaged
20.248 ms, the dominant measured cost. That result describes this one dev run,
not a general release-performance claim. The final windows exclude startup,
but this does not establish that all first-use costs disappear in longer runs.

## Render passes

| Case | Cloud pass mean | Local fog pass mean | Shadow update mean |
| --- | ---: | ---: | ---: |
| Overcast ground | 6.257 ms | 0.338 ms | 1.847 ms |
| Dense stratus interior | 20.248 ms | 0.011 ms | No update sampled |
| Ground fog valley | 6.672 ms | 0.269 ms | 1.123 ms |

Shadow timings are conditional update costs, not per-frame costs. Field,
summary and clock work measured approximately 0.012-0.013 ms, 0.005 ms and
0.004-0.005 ms respectively. The demo uses 32 authored ray steps, two shadow
cascades at 256 resolution, 512 sparse pages, 64-metre canonical cells and a
20 Hz weather timeline. The cloud integrator takes four samples per authored
step, so this demo runs up to 128 integration samples. The existing optimized
benchmark fixture uses one cascade and 12 steps, for 48 samples, so its numbers
do not represent this dense demo scene.

Headless exploratory runs showed large renderer waits and are excluded from
the native measurements above. Their cause has not been established. The native
client uses an SDL GPU swapchain, where the allowed frames in flight setting
applies; see [`SDL_SetGPUAllowedFramesInFlight`](https://wiki.libsdl.org/SDL3/SDL_SetGPUAllowedFramesInFlight).
The headless wait measurements do not establish that production scenes stutter
or identify first use as their sole cause.

## Visual and lighting findings

The inspected acceptance captures are `stratus.png`, `inside.png`, `storm.png`
and `fog.png` under `.cache/build/dev/weather-acceptance/`. Overcast showed broad,
smooth blue-grey coverage. The storm view was almost uniform dark blue. The
warm horizon remains from the authored Sunset sky in
[`Weather.luau`](../mono.engine/examples/assets/scripts/Weather.luau#L361).
The prototype did not show convincing detailed self-shadowing across the
overcast layer. [`weather-clouds.comp`](../mono.engine/resources/shaders/weather-clouds.comp#L301)
uses one lighting value along each ray.

The default lighting path does not automatically dim ambient light with cloud
cover. [`Sunlight.cpp`](../mono.engine/scene/src/Sunlight.cpp#L56) copies the
authored `Ambient` and `OutdoorAmbient` values, and
[`ambient-lighting.glsl`](../mono.engine/resources/shaders/ambient-lighting.glsl#L2)
uses those values directly. Deferred lighting applies
[`WeatherShadowFactor`](../mono.engine/resources/shaders/deferred-lighting.glsl#L200)
to direct sunlight; ambient lighting is separate. Optional weather density,
haze and glare responses are assembled in
[`WeatherEffects.cpp`](../mono.engine/render/src/WeatherEffects.cpp#L310), but
their strengths default to zero in
[`WeatherCompute.hpp`](../mono.engine/scene/include/engine/scene/WeatherCompute.hpp#L102).
Rain does not act as an ambient dimmer.

The demo supplies an authored presentation response by lerping brightness,
ambient and outdoor ambient in
[`Weather.luau`](../mono.engine/examples/assets/scripts/Weather.luau#L786):
clear uses brightness 3 and outdoor ambient `(110, 132, 170)`, stratus uses
1.35 and `(57, 73, 104)`, and storm uses 0.55 and `(25, 37, 64)`. That scripted
response works in the demo. Automatic engine ambient response to cloud cover
remains absent.
