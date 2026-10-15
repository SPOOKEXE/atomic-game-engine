# Weather, portal and mirror visual validation

Validation on 2026-10-15 used an NVIDIA GeForce RTX 4090, Vulkan and driver
580.173.02. Runtime captures use the `dev` preset unless stated otherwise.
These are real rendered images, including native-window checks, rather than
mock renderer results. Metal execution remains unverified. Capture-file waits
and fixed capture pacing make these runs unsuitable as FPS benchmarks.

Renderer acceptance passes in both presets. Client high-FPS handoff acceptance
remains open: the final rerun fails sample-count gates, and an earlier run
captured an all-black crossing frame. Details and original evidence are below.

Local images, metadata, contact sheets and logs are retained under
`.cache/build/dev/weather-visual/`. Strict CI logs use the corresponding
`ci/weather-visual/` directory; test-owned captures live under each preset's
`tests/` directory. Generated images are build artifacts rather than checked-in
benchmark output.

## Inspected scenes

| Coverage | Evidence |
|---|---|
| Weather recipes | Clear, scattered cumulus, overcast stratus, towering storm, wispy ice and ground fog at 1280x720; GUI status text confirms each selection |
| Weather views and controls | Ground, cloud interior, above clouds, valley fog, roof/rain and storm; stateful checkpoint and pause controls |
| Portal composition | `Portals-1-world`, `CrossWorldSeam` with two worlds and zero spacing, `PortalShadow`, and both near/far lighting scenes |
| Mirror composition | `Mirrors-1-world` surface grades, `Mirrors-4-worlds` with four worlds, `RecursiveMirrors`, `MirrorCorridor` and `MirrorDepth` |
| Surface load | `StressMirrors` with 320 authored facets; this is a visual workload check, not a frame-budget claim |
| Reflection depth | One- and three-bounce variants of the recursive and depth demos |

The initial and extended stills are in `base-contact.png` and
`extended/contact.png`; weather controls are in `controls/contact.png`.
Inside-cloud and storm views contain broad, smooth dense volumes. These checks
establish visible composition and working controls, not photorealistic cloud
detail. The corrected scattered-cumulus and wispy-ice button names were actually
activated; earlier invocations with incorrect names are not positive evidence.

A native-window `Portals-1-world` tour wrote all 1,800 images and metadata
records at 640x360, with 1,800 simulation ticks and no dropped ticks. Frame 999
crosses the seam, mapping the camera by 28.638 metres. Inspected adjacent
frames retain the continuous room view. None of the 1,800 images is entirely
black. See `motion/native-portal-contact.png`,
`motion/native-crossing-contact.png` and `motion/image-audit.json`.
The complete fixed-step tour is also encoded as `motion/native-portal-tour.mp4`.

Two 180-frame recursive-mirror sequences use one and three bounces, fixed alpha
0.5 and 60 Hz capture progression. Compared frames 30, 90 and 179 have identical
camera poses and ticks in both runs. The three-bounce view contains additional
nested reflections; both sequences contain moving reflected objects and no
entirely black frames. See `motion/recursive-contact.png`. This is separate
from the intermittent networked high-FPS observation below.
`motion/recursive-bounce-comparison.mp4` places one bounce on the left and three
on the right, with matching frames.

## Portal seam and lighting measurements

The 1920x1080 seam run captured 90 frames each from side, front and far views,
plus 90 matching continuous-room reference frames. All 360 BMPs and metadata
records were written. The report confirms complete metadata, matching fixed
interpolation alpha 0.5 and pipeline identities.

Across the 90 matched front/reference frames, the body moved 62.906 pixels and
the maximum missing and duplicated body counts were both zero under the
report's one-pixel boundary allowance. This checks body silhouette and aperture
coverage; it does not assert identical whole-image shading. See
`seam-1080p60/portal-seam-report.json` and its contact sheet.

All four 1280x720 lighting stills passed the existing pixel reporter:

| Scene/view | Positive pixels |
|---|---|
| Amber lamp, near | 305,493 amber; 329,658 lit |
| Amber lamp, far | 115,198 amber; 131,328 lit |
| Red/green lamps, near | 83,270 red; 18,520 green; 138,708 mixed |
| Red/green lamps, far | 26,731 red; 39,988 green; 65,032 mixed |

The sun is disabled in these fixtures. The far amber illumination and the
opposite lamp's channel provide positive evidence of light crossing the seam.
The contact sheet is `lighting/contact.png`; raw counts are in
`lighting-helper.log`. The final helper rerun reproduces these counts and
requires clean client exits for all four views. Its images and per-view runtime
logs are in `lighting-final/`, with the reporter output in
`lighting-helper-final.log`.

## Weather in a mirror

A downward camera sees a horizontal mirror. Every primary ray points down,
while the deliberately red cloud slab lies above the camera. The direct view
therefore cannot explain red cloud pixels in the mirror aperture.

Both mixed and separate capture paths pass the new GPU fixture. It requires
positive red cloud pixels inside an analytically bounded mirror aperture,
preserves exterior samples within two display codes, and restores the original
image after clouds are disabled. Each preset passes 25,144 assertions in one
case. Development and strict CI were run separately on the actual device.

An independent runtime on/off pair changes all 153,600 samples in its central
mirror region and zero of 48,640 samples in the exterior top strip. Images and
the difference are in `integration/`. A separate behind-camera red marker
produces 3,136 red reflection pixels and zero with reflection disabled, in
`mirror-marker/contact.png`. A previously black mirror was looking into an
empty black background; that observation did not justify a renderer change.

## Repairs made during validation

The seam and lighting capture helpers now preserve the scene basename in a
per-view fixture directory. Renaming the scene previously broke lookup of its
client camera companion and prevented captures from starting. The actual seam
and lighting helpers now generate their expected images.
The lighting helper also retains per-view logs and rejects failed or timed-out
clients even if a BMP was already written.

`CrossWorldSeam` now authors coincident mouth centres and opposite face normals
in the two worlds. The former destination mouth was displaced and faced the
same way, so the rear view requested an unserved nested return capture.
Paired front/rear images now show the blue destination. A headless test loads
the real scene in both worlds and checks mappings, axes, both camera sides and
crossing round trips: 40 assertions in each preset.

The client prepares an unjoined sequence-capture portal only once per frame.
Previously it prepared the native eye and then appended the same portal claim
again in the fallback path. The reproduced run wrote JSON records but no BMPs.
The new real-client GPU test requires all 24 images and metadata records,
unique portal indices, and a completed destination image. Development and
strict CI each pass 199 assertions.

The disabled-weather graph now forwards the unchanged colour image through
weather reconstruction and fog. The storage-image boundary previously broke
the identity chain and recorded extra full-image copies. The contract permits
the bridge only when the storage name has known sampled-image readers;
writable storage and unknown consumers keep materialized images. Other declared
outputs remain checked for overwrite conflicts.

The allocation plan must retain forwarded sources through their later readers.
At 640x480 this adds one 2,457,600-byte physical target to the plan and extends
two RGBA16F image lifetimes, raising planned peak live bytes by 4,915,200.
These are planning bounds, not measured driver residency. Inactive output
targets need not allocate. The existing device oracles measure two identity
copies with named intermediate inspection and one without inspection, compared
with three in the reproduced regression.
The earlier logical-memory measurements in the implementation report predate
this allocation-plan change; no new full-weather memory benchmark was run here.

The real-process portal fixture now allows 60 seconds for device startup while
retaining five-second image-reply deadlines. One actual child needed 29.708
seconds between timestamp initialization and renderer readiness, exceeding the
former 20-second startup allowance. Both child variants then passed. Early
child exit now reports its reason, code and signal immediately.

The final explicit non-GPU renderer selection passes 822 cases and 414,470
assertions in each of `dev` and strict `ci`. Source checking, architecture and
their fixtures pass. Changed C++ formatting and shell syntax checks pass.
The default renderer selection also passes 844 cases and 415,896 assertions in
`dev`; it includes device work and overlapped a CI run, so it is not an isolated
GPU run or a performance measurement.

The initial broad device run failed six of 206 renderer cases, with 12 failed
assertions. Those checks exposed the startup allowance and disabled-weather
forwarding issues described above. The rebuilt development device run passes
all 206 cases and 2,510,681 assertions without weakening the image or copy-count
oracles. Its log is `render-gpu-accepted.log`.
The final strict CI device run also passes all 206 cases, with 2,510,678
assertions. The final two device selections ran sequentially; their small
assertion-count difference does not change the selected cases or their outcomes.

## Intermittent high-FPS findings

The broader client GPU selection completed 31 cases with 30 passing and one
failing, with three failed assertions out of 1,689,455. The selected cases cover
first/third-person traversal, return crossings, cleared subjects, variable
presentation, late content, image handoff, retirement and fault scenarios. The
24-cell network impairment matrix was explicitly excluded from this visual run.

At 144 FPS with 60 Hz simulation, frame 276 was entirely black. Adjacent frames
contained 7,232 and 7,465 nonblack pixels out of 16,384. The camera was about
1.9 mm beyond the seam; portal metadata still named a ready, current image and
a matching camera revision. That metadata cannot prove the producer's pixels
were valid. The same case also missed two sample-count gates at 240 FPS.

The failed assertions in `mono.client/tests/PortalWalk.cpp` were:

```text
1314: visible for: false                         (144 FPS, frame 276)
1455: adoptedClearSamples >= 3 for: 1 >= 3        (240 FPS)
1621: observed.LookSamples[stage] >= 10: 9 >= 10  (240 FPS, stage 0)
```

An isolated rerun, with no concurrent GPU work, passes all 73,560 assertions at
144/240 FPS. This does not resolve the first observation. Some diagnostic
captures overlapped the initial broad run, so it supplies no isolated timing
claim and does not establish contention as the blackout's cause.

A final isolated rerun against the rebuilt client fails two of 72,337
assertions: `adoptedClearSamples >= 3` reports `1 >= 3` at both 144 and 240 FPS.
Its visible-pixel assertion does not fail. This is still a failed acceptance
check, not a resolved handoff issue. The log is `client-high-fps-final.log`;
all 3,840 final BMPs and metadata records are archived in
`high-fps-final-run/`. The evidence does not establish whether these sample
failures and the earlier blackout share a cause.

The original metadata and images were archived before rerunning under
`high-fps-first-run/`; the full logs are `client-gpu.log` and
`client-high-fps-isolated.log`. No speculative near-plane change was made.
Producer and final consumer image readbacks at the failing camera pose are the
next discriminating diagnostic. This remains an open visual continuity issue.
