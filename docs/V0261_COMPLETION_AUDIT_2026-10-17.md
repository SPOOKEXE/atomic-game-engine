# v0.26.1 follow-up correctness audit

This audit follows the strict gate and three native release builds at `630c711c`.
Those earlier results do not validate the changes below. Quiet performance
acceptance remains pending under the user's instruction to keep other workloads
running. B0 and all three optimization passes remain incomplete.

## Compute admission limits

`76d49a19` adds actual GPU execution at the stock dispatch ceiling of 4,194,240
inputs, with an exact sparse-output CPU oracle. The compact-and-double package's
full-result ceiling is 3,945,730 elements: output plus diagnostic readback exactly
fills 16 MiB. One more output element refuses without reserving device memory.
This package-specific readback ceiling is separate from dispatch capacity.

`2fa63923` checks the script bridge's separate 8 MiB output limit. Two retained
copies charge the 16 MiB result budget. Exact 64 KiB chunk reads cover the whole
result; another owner refuses until release and then succeeds. A one-element
oversize request preserves its ticket and reservations.

With explicit Khronos and synchronization validation and private queues in serial
mode, all 15 compute GPU cases passed with 979 assertions and zero skips or
Vulkan validation errors. Log:
`.cache/build/ci/v0261-compute-with-bridge-validation.log`.

## Authored town cadence

The original paired Town scripts reproduced pose/effect intervals of 1 instead
of 4/8 and created no beams: 20 failed assertions across both VMs.
`47cf2083` adds optional presentation pairs to the existing Configure method.
Omitted pairs retain existing values; incomplete or invalid pairs refuse before
changing policy or pending admissions. Bindings were regenerated. Schema TOMLs
were regenerated and had no content changes.

The paired scripts now author all four domain tiers and their own beams. The
Town benchmark adopts those policies and sources. Only its full-fidelity
reference overrides intervals. The real animation/ribbon consumer fixture passed
18,614 assertions; native and paired VM configuration checks passed 46 and 26.
Existing paired Town and crowded Town checks passed 693,390 assertions in three
cases. Relevant logs are `v0261-town-authored-validation.log` and
`v0261-town-existing-validation.log` under `.cache/build/ci/`.

`e93cd553` carries the authored examples and consumer fixture. All 12 optimized
Town benchmark cohorts passed 600 measured ticks after 120 internal warmup ticks,
with zero harness warmup samples. Each cohort compares against the opposite VM
under the same policy. Simple, multi-world complex and single-world crowded
variants passed both full and authored modes. Final semantic track time was
11.999988556 seconds at tick 720. Timing acceptance is withheld.

A fresh temporary game embedded the exact updated Town sources, with SHA-256
`d7a814a05c1b04f474fa13fcd8b7fcfc36b638cabd7e6185458ee613d987ddd1`
(Luau) and `6442da8c0ebddabfef7115ae6f4ae5d482682e82adb361fd96a0118489b9721d`
(JavaScript). Both ran in live Studio, produced two enabled beams with live
attachments, advanced animation tracks, and restored the original scene on stop.
Run mode settled at 48 instances in each world. The live log has zero engine
errors or Vulkan validation messages, and Studio exited 0:
`.cache/build/ci/v0261-live-studio/studio-authored-cadence-final-validation.log`.
Viewport captures do not establish a beam pixel oracle. Dropped simulation ticks
occurred while the host was shared; this is functional evidence only.

## Baked-reference persistence

The initial reproduction lost `BakedProceduralReference` in all three ordinary
paths: world text, `.aworld`, and `.agame`. A first adapter then exposed a second
failure: a restored collider had mask `4294967295`, Trigger false and CanQuery
false. World properties retain CanCollide through Trigger, while the empty mask
belongs to the procedural binding and is derived state.

`1bd49a60` adds the bounded owner codec: at most 64 records and 36,872 bytes.
Game supplies document-local IDs; native handles do not enter the file. Import
validates the entire packet before publishing references, fresh editable-mesh
names and the derived empty contact mask. Trigger and CanQuery remain authored.
Malformed packets preserve accepted demands, visuals and masks.

`d953b075` embeds the owner bytes in ordinary world files, resolves references
after all items load, omits transient subtrees and inert rows, and preflights
baked demands before replacing universe world files. Both generator-free and
authority-bearing worlds use this path.

Owner checks passed 287 assertions in eight cases. Ordinary game/procedural
checks passed 312 assertions in ten cases, including all three roundtrips,
recoverable malformed blocks and failed-save preservation. The earlier failure
log is `.cache/build/ci/v0261-baked-mask-repro.log`; the corrected focused results
are retained in the task's tool output.

## Host waits and busy cohorts

`ac166654` records owner submission-worker waits separately from recording joins.
The benchmark also samples the existing host `SDL_WaitForGPUIdle` call outside
Render, with a bounded sample count and tails. Both measures include host/API
overhead and explicitly leave GPU dependency waits unmeasured.

Serial and parallel optimized runs passed exact full-buffer compute oracles for
one, two and eight views. Each row reported 16 host-idle-wait samples; parallel
submission-worker wait counts were 64, 80 and 176. All six rows had zero dropped
queue queries and zero Vulkan validation errors. Sixteen samples are marked
insufficient for tail acceptance. No speedup is claimed.

The real X11 parallel queue fixture also passed seven cases and 651 assertions
with zero skips and zero Vulkan validation messages. Its injected submission
failure intentionally emits an engine error. Log:
`.cache/build/ci/v0261-host-wait-windowed-parallel-validation.log`.

The new separate weather busy cohort ran at 65x65 with 17 opaque receivers,
two 32x32 shadow cascades and 32 cloud steps. Cold, 96-frame bounded maintenance,
16-frame strict reuse and 16-frame advancing phases passed the harness checks,
with zero Vulkan validation errors. Steady reuse and advancing phases created
no GPU resources. This is diagnostic correctness evidence, not a quiet B0 row.

## Remaining acceptance

Commit `0d207e61` aligns all three trace shader pass prefixes with the CPU ABI:
`Eye`, `CameraDepth`, and `RenderFeatures` are at offsets 160, 176, and 192.
The staged named-member SPIR-V offset test passed in CI's `[render][hard-render]`
selection (6 cases, 166 assertions); log:
`.cache/build/ci/v0261-trace-uniform-test.log`.

Commit `237268fe` fixes gravitational-lens ray direction for oblique
projections by using the finite forward point from `WorldAtLinearDepth`,
removing the far-plane 0.999 ray flip. The existing GPU gradient test now covers
three projection modes and two spin phases. The old shader failed both left
oblique assertions: the baseline stayed at 0.3828125 instead of exceeding
0.39281249. The fixed shader passed all 101,592 assertions in one case, with
zero VUIDs and skips. Logs:
`.cache/build/ci/v0261-lens-projection-{repro,fixed}-validation.log`.

Commit `1767c0bd` fixes a separate benchmark-fixture clipping issue: the old
0.08 lens skew clipped the bent six-metre background. The source probe was
0.422852 while its bent sample was about zero. Lens-only skew 0.04 preserves
the gradient; other oblique families retain 0.08. Benchmark rows report the
actual skew. The focused GPU test keeps the stronger 0.08 skew. Directional
thresholds and six-metre depth remain unchanged.

The final ray matrix passed all 135 oracles: 45 each for perspective,
orthographic, and oblique projection across five scenes and nine cohorts at
65x65 for 16 frames, with one measured sample and zero harness warmup samples.
Explicit Khronos and synchronization validation reported zero VUIDs. Timing is
rejected because workloads were shared. The headless server CTest passed 46/46
with zero failures in 446.14 seconds; log:
`.cache/build/server/v0261-followup-ctest.log`. The documentation check passed
with every public entity documented and zero malformed comments; log:
`.cache/build/ci/v0261-followup-docs-check.log`. The headless server full build
passed 1,711 build edges with the Debug line-tables and unity override, without
staged shaders or the client; `--version` reported 0.26.1. Consult final-revision
gate and native workflow evidence in CI logs and GitHub Actions before claiming
completion. The B0 matrix remains incomplete and zero of three optimization
passes is complete. These correctness checks establish no optimization gain.
At the time of this earlier audit section, actual GPU dependency waits remained
unverified; host API durations must not be relabeled GPU queue waits. The saved
Nsight Graphics capture evidence is recorded below.

## Partial CPU heap baseline

Commit `d0ce2609` added v2 heap rows to the five existing graph-pipeline
benchmark suites when `MONO_BENCH_MEMORY=1`. These measurements predate the
audio repair in `48dbe261`. The rows report process allocation byte and block deltas,
retained-byte delta, live bytes and blocks, exclusive named-tag self peak,
process lifetime peak, modeled header overhead (`LiveBlocks * 32` on this
build), per-block overhead, and dropped-scope and foreign-free deltas. Zero
dropped scopes and foreign frees were observed. The probe counts tracked C++
requested payload; it excludes `malloc` and GPU allocations, as well as the
total profiler and allocator footprint. A tag peak is its maximum simultaneous
attributed bytes, not a whole-path interval peak.

Each run set produced 26 heap rows across the five suites. Three fresh
processes per suite produced 15 process runs; rows were byte-identical across
the repeats. Each process took one measured body sample with zero warmups. Each
set also ran 30 existing benchmark bodies, and all semantic oracles passed.
Output remained on stdout and was not saved. The `bench` preset used
RelWithDebInfo, `MONO_HEAP_PROFILE=ON`, unity builds, and GCC 13.3 on a Ryzen
9 9900X with 24 logical CPUs. Compile commands show effective `-O3`: first-party
and benchmark flags override the preset's `-O2`. This establishes no speed
claim. The run was CPU-only, so GPU resolution was not applicable.

The pre-fix graphcore empty case allocated 976 bytes in 14 blocks. Its prepared
64-node order case allocated zero bytes and blocks in this fixture; this does
not establish that all graph compilation is allocation-free. Audio's
deadline probe allocated 2,568 bytes in nine blocks and retained 1,348 bytes on
its cold invocation, then allocated 136 bytes in one block and retained zero
bytes on the immediately following warm invocation. Source review attributed
the warm allocation to the GNU 13 `std::stable_sort` temporary. The cold
invocation also grew `Taken` and `Schedule` and rebuilt scratch storage. This
was the pre-fix evidence that the mixer callback's no-allocation requirement
was unmet, not profiler initialization.

All five benchmark targets built successfully across ten build edges, and
`format-check` passed with pinned clang-format 21. These are diagnostic memory
measurements only. Shared-host timings are rejected, and this is not full B0
or pass completion. No speed claim is made.

## Audio deadline callback repair

Commit `48dbe261` repairs the mixer callback allocation path found above. The
mixer preallocates bounded flat scratch and queue, schedule, and ID lookup
storage at construction. Sorted IDs avoid map-node allocation, and ordinal
sorting preserves command posting order. The scratch payload is 1 MiB for
stereo at 128 frames and 4 MiB at the default 512 frames, plus other reserved
storage. This is an explicit resident-memory tradeoff, with no timing claim.

On the same `bench` preset and GNU 13.3 environment, the updated audio suite
emitted seven heap rows, including mixer construction. Three fresh-process
runs were byte-identical. Construction requested 14,213,136 bytes across 23
allocations and retained zero bytes after destruction. Cold and warm
`RenderScheduled` each allocated and retained zero bytes; dropped scopes and
foreign frees remained zero. After the warm fixture, process live bytes were
14,251,624, compared with 12,906,488 in the pre-fix run. The actual sample
oracle is unchanged.

Three new allocation tests fail against the old `9cabd481` mixer with 18 heap
assertion failures; 5,276 of 5,294 assertions passed. The fixed audio suite
passed 105 cases with one SDL dummy-driver skip, 150,506 assertions total. The
skipped case passed separately with `SDL_AUDIODRIVER=dummy` and 44 assertions,
covering all 106 cases across both runs. Logs are
`.cache/build/ci/v0261-audio-repro/old-tests.log`,
`.cache/build/ci/v0261-audio-fixed-tests.log`, and
`.cache/build/ci/v0261-audio-dummy-tests.log`.

The AddressSanitizer and UndefinedBehaviorSanitizer audio run passed 42 cases
and 22,062 assertions with leak detection and halt-on-error enabled. The audio
module unity build and mixer tests were instrumented; dependencies were not.
Log: `.cache/build/ci/v0261-audio-sanitize/tests.log`. New cases cover cold and
warm rendering, 1,024 nodes, a full 1,023-command queue, tie order, sparse high
IDs, replacement and topology deadlines, overflow, and zero output. Existing
format mismatch and float summation order checks pass. `SoundRef` ownership
remains with the host; no retirement system was added.

This repair establishes allocation correctness for the checked callback paths,
not an accepted optimization pass. Full B0 and all three optimization passes
remain incomplete.

At `5116b884`, strict pre-push passed all 771 suites with zero skips or failures
and all standard checks passing. Log: `.cache/build/ci/v0261-audio-pre-push.log`.
The native workflow also passed Build, Process Isolation, Package, and Upload
on all three platforms, with six artifact records:
[workflow run](https://github.com/SPOOKEXE/atomic-game-engine/actions/runs/38984298078).
The publish job was skipped in this manual dry run; no release publication is claimed.

## Additional CPU heap baselines at 5116b884

CPU-only stdout diagnostics used `bench`, effective `-O3`, heap profiling on,
and GCC 13.3. Shared-host timings are rejected; no speed or GPU-traffic claim.

Renderer startup registration matched in three fresh processes: 314,665
requested bytes, 2,592 allocations, 162,364 retained bytes in 1,252 blocks.
Its 99-kind canonical byte sequence is 13,121 bytes (SHA-256
`bc0cedee5fccfa4c0bf8712383e55a22d45e58f478ddabd051a47eaa45be6e35`). Three
warm processes matched across 20 heap, four work, and five canonical rows for
empty/simple/default-PBR/stress-256. Repeated registration used 204,462 bytes
and 2,215 allocations with no retained-byte delta. This candidate awaits full
B0; planned logical bytes are not actual GPU traffic.

Portal fixtures passed analytic owner and distance oracles within 0.003 m:
five cases, three processes, 2,048 rays per case per process. Empty allocated
zero; simple, blocked, and short each allocated 32,768 bytes in 8,192 blocks;
the 32 linked portal pairs with 4,192 colliders allocated 1,048,576 bytes in 262,144
blocks. Retained-byte delta was zero for every case.

C06 passed eight cohorts (empty/simple/busy/complex in authoritative-serial and
threaded-preview modes), three processes, one sample, zero warmups, one trial:
192 phase rows, 16 fixtures each. Four artifact hashes matched across modes
and processes; the last two also matched pinned expected hashes. Cancellation
completion varies by schedule, and process peak includes the full trial and
oracles, so this proves neither stable cancellation cost nor leak freedom. Max
retained requests were 2; logical admission reservation was 64 MiB, not heap.
Full B0 and all three optimization passes remain incomplete.

## C02 paired Town CPU diagnostic

C02 ran seven native and 12 paired Town workloads, one process per suite. Each
row covered six windows: 600 measured ticks after 120 internal warm ticks, one
harness sample, and zero outer warmups. All 42 native and 72 Town windows
passed semantic checks. Maximum promotion latency was zero; authored maximum
age was seven ticks. Animated fixtures ended at tick 720 with track time
11.999988556 s. Town owners used the opposite VM under the same policy as a
reference outside the measured owner. Shared-host timings are rejected.

C++ heap counters and VM allocator counters remain separate. Luau reports
requested bytes and sums of individual VM peaks; those peaks are not a
simultaneous process peak. QuickJS reports charged malloc bytes without peaks.
Cross-basis VM comparisons are invalid.

## C02 fresh-process RSS diagnostic

The benchmark binary used engine code at `5116b884` plus the selector-only
patch; no engine-code changes were included.

Collected 2026-10-17 with `/usr/bin/time -f PROCESS_RSS_MAX_KIB=%M`,
`ATOMIC_CADENCE_TICKS=600`, one harness sample, zero outer warmups, 120 internal
warmup ticks and 600 measured ticks. Each of the 19 selected rows emitted one
benchmark row, and all semantic oracles passed. The following are whole-process
resident high-water marks in KiB, including fixture setup, internal warmup,
reference work and oracles. They are not owner or VM heap peaks, and do not
measure physical GPU memory.

| Cohort | Maximum RSS (KiB, full fidelity / authored cadence) |
|---|---:|
| Native empty, single policy | 29,936 |
| Native simple | 31,756 / 32,200 |
| Native busy | 35,216 / 35,436 |
| Native complex | 48,584 / 48,396 |
| Town Luau simple | 34,912 / 35,524 |
| Town Luau complex | 77,360 / 78,320 |
| Town Luau crowded | 37,252 / 37,556 |
| Town JavaScript simple | 35,560 / 34,716 |
| Town JavaScript complex | 77,092 / 77,188 |
| Town JavaScript crowded | 35,988 / 36,080 |

The build was `bench`, effective `-O3`, with heap profiling and unity builds
enabled, using GNU 13.3 on a Ryzen 9 9900X. This CPU-only diagnostic makes no
RSS improvement claim. Shared-host clock and latency gains remain pending, and
full B0 has not passed.

## Server host lifetime repair

The original docs-only pre-push at `ac0dac6e` passed 770 of 771 suites, then
crashed in `server.host` with SIGSEGV (`.cache/build/ci/v0261-baseline-notes-pre-push.log`).
The VM destructor called `Store::ClearDescendantRemoving` through a borrowed
`Store` after its world was destroyed; host `Runtimes` still retained the VM.
Production create/reset/retire also
reproduced exit 139 with allocator perturbation. An older partially instrumented
ASan run confirmed heap-use-after-free in lifecycle and package fixtures. Logs:
`.cache/build/ci/v0261-server-core-backtrace.log`,
`.cache/build/ci/v0261-server-lifecycle-perturb.log`, and
`.cache/build/ci/v0261-server-sanitize/old-{lifecycle,package}.log`.

Commit `3997b2a3` stages a candidate VM through a weak reference while the
candidate scheduler owns it strongly, clears old host VM references at swap
commit and before retirement, and updates the primary world only after commit. The
private `BuildWorld` output remains private. The package fixture now exercises
`RuntimeOf` and `DiscardRuntime` in a standalone universe with host callbacks.
Six focused data factory cases passed 262 assertions with allocator
perturbation and ASan plus UBSan with leak detection. Sanitizers instrumented
the LuauRuntime and Store unity translation units; other dependencies and test
code were uninstrumented. Logs:
`.cache/build/ci/v0261-server-lifetime-perturb-fixed-targeted.log` and
`.cache/build/ci/v0261-server-sanitize/fixed-targeted-tests.log`. Selector commit
`1894f6af` passed 249 assertions across 24 tool cases
(`.cache/build/ci/v0261-benchmark-selector-{build,tests}.log`). At code head
`9784ad17`, the strict pre-push gate passed 772 suites, 0 skipped, 0 failed;
log: `.cache/build/ci/v0261-selector-lifetime-pre-push.log`. The server full-file
test passed all 36 cases and 2,011 assertions; log:
`.cache/build/server/v0261-server-lifetime-tests.log`. Native workflow
[38987915616](https://github.com/SPOOKEXE/atomic-game-engine/actions/runs/38987915616)
passed Linux x86_64, Windows x86_64, macOS ARM64, Build, Process Isolation,
Package, and Upload. It recorded 6 artifact records; publish was skipped as a
manual dry run.

## C04 saved Nsight Graphics capture

A privileged Nsight Graphics 2026.3.1 capture was saved from code head
`9784ad17` in `.cache/build/bench/c04-sudo-trace-wbmiB5`. The trace
`bench_render_2026_10_17_15_24_22.ngfx-gputrace` is 8,727,374 bytes, and capture
status was 0. `capture.log` records successful save, load, and table exports
before target termination and a later connection error. The profiler terminated
the target after capture, so this is not a completed 512-frame benchmark oracle
or accepted benchmark-row set.

The run used the bench build at effective `-O3`, heap profiling enabled, GNU
13.3.0, Ryzen 9 9900X, RTX 4090 (AD102), driver 580.173.02, Vulkan, and Nsight
Graphics 2026.3.1. It rendered 8 moving views of 1,024 resident meshes at
128x128 per view, with 16 internal warm frames and 0 harness warmups. It used a
524,288-element `f32x4` compute input and output (8 MiB each) and was configured
for 512 frames. Submit delay was 200, submit limit 256, and maximum capture
duration 1,000 ms. Clocks were unaltered, HES and screenshots were off, and
metric set was `AsyncComputeTriage`; real-time shader profiling was off.

The exported `FRAME.xls` duration of 225.215 ms is the entire trace duration,
not an engine frame. The `.xls` exports are ASCII TSV counters and include no
per-queue wait export. The capture UI was reviewed offline under private Xvfb;
queue labels resolved to graphics Q0, compute Q1, and transfer Q2. AT-SPI export
recorded 256 submit rows in
`.cache/build/bench/c04-offline-review/submit-events.json`.

The compute queue has an observed 203.818 us dependency wait at 152.74 to
152.94 ms on a transfer-upload semaphore. Compute events 188481 to 188495 wait
on semaphore `0x61965c7b7090`, signaled by transfer events 188612 to 188615. A
subsequent transfer readback wait lasted 45.139 us at 152.95 to 153.00 ms on
compute signal `0x61965c23e120`. These are GPU timeline dependency waits, not
CPU wait-call durations. Nsight Graphics documents these synchronization
objects in its [GPU Trace UI guide](https://docs.nvidia.com/nsight-graphics/UserGuide/gpu-trace-ui.html#queue-synchronization-objects). The offline review screenshots are
`compute-wait-152ms.png` and `readback-selected.png` under
`.cache/build/bench/c04-offline-review/`.

The frame-active value was 46.89 ms of 225.22 ms, rounded; other contexts show
red hatches. External Ollama and RustDesk processes were resident, so the trace
does not establish quiet timing or an optimization gain. Shader metric names
`main_0x...` were missing and removed after a UI warning; no shader occupancy
conclusion can be drawn from those metrics. This capture advances C04 GPU-wait
evidence only. C04 and B0 remain incomplete, with 0 of 3 optimization passes
complete.

## 1080p ray and weather GPU diagnostics

At engine code head `9784ad17` (documentation-only head `1f13c5fb`), the existing
ray benchmark ran eight families, with retained-static and moving-camera mirror
workloads: 9 cohorts x 5 scenes x 3 projections, 135 scenarios at
1920x1080. Each used 16 measured frames after 8 internal warm frames, one
harness sample, and zero outer warmups. Bench used effective `-O3`, heap
profiling, GNU 13.3, RTX 4090, and Vulkan. Perspective completed with 45 oracle
rows, 5 moving-camera oracle rows, and 45 work rows; all drops were zero.
Orthographic and oblique each completed 45 work rows and 50 oracle output rows,
all with zero drops. All three exited 0 without errors. Every process enabled
Khronos and synchronization validation. Full-stream scans reported zero Vulkan
validation errors and zero engine errors.

The opt-in weather device scenario CPU harness ran five fresh processes for
inactive, empty, simple, busy, and complex scenes at 1920x1080 with shadow
resolution 256. Each had one sample, zero outer warmups, and 16 phase frames.
All passed oracles, exited 0, and had zero scanned validation errors. Each
completed four phases with 1 cold, 96 maintenance, 16 strict-reuse, and 16
advancing frames, 20 phases total across five processes. In the complex cohort, strict-reuse and advancing each reported zero new or released logical
GPU resources; dropped events were zero in all four complex phases. These
resource and drop observations are limited to that cohort.

Benchmark output remained on stdout. Ollama processes remained resident, so
quiet timing and optimization gains are not claimed. B0 remains incomplete, with
0 of 3 optimization passes complete.

## C04 empty recording row

`RunRecordingBatch` defaults to 1,024 meshes. Its recipe discovers a one-view,
zero-mesh row without authored compute even with `MONO_RENDER_RECORDING_COMPUTE=1`.
Populated 1, 2, and 8 view rows are unchanged. The old binary rejected the
selection with “no benchmark matches the requested selection”; the updated
build passed. `clang-format-21` corrected the declaration in
`RecordingBatch.cpp` only. The full format check and em-dash check passed.

GPU validation covered ordinary, serial, and parallel modes in four cohorts
at 128x128 per view, with 16 measured frames after 16 warm frames. All 12
functional oracles passed. All three empty rows had zero `FrameResult`
instance rows, dirty rows, and triangles. They also had zero instance-upload
bytes and zero logical GPU allocation/release bytes during measured frames; no
authored compute ran. All nine populated rows passed exact
full-buffer 8 MiB checks. Eight private rows each completed 16 batches with zero
GPU query drops. Full-stream scans found zero VUIDs or engine errors with
Khronos and synchronization validation enabled. The ordinary 8-view row
had 296 dropped GPU timing marks; the other 11 had zero, so that row's GPU
profile is partial. Shared-host timings and tails are not accepted. B0 remains
incomplete, with 0 of 3 optimization passes complete.

## Ordinary multi-view timestamp capacity

At `d69975cd`, GDB sampled eight frames of the ordinary eight-view recording
benchmark (`-O3`, 128x128 views, 1,024 shared resident meshes, 8 MiB compute).
It recorded 1,616 timestamp attempts, 202 per frame: 1,024 writes and 592
capacity refusals with the old 128-mark pool. Null commands, inactive slots,
and native-handle failures were zero. The benchmark's 16 measured frames
reported 296 dropped GPU timing marks.

The limit is now 256 marks per pool, with four pools per collector: 512 extra
query slots. Driver allocation bytes are unknown. CPU result arrays grew from
1 to 2 KiB at eight bytes per entry. Wait, lifecycle, and public APIs did not
change. The old real Vulkan test failed at mark 128 (262 assertions passed,
one failed; `.cache/build/ci/v0261-timestamp-capacity-red-tests.log`). The
fixed test passed 742 assertions in one case in each of ordinary, serial, and
parallel modes, with zero VUIDs.

All 12 recording rows passed their oracles, including nine exact 8 MiB checks,
with zero dropped GPU timing marks or VUIDs. Ordinary eight-view timing had
four completed sequences. Private serial had 16 completed frames, 176 graphics
samples, and intentionally zero compute and transfer samples. Private parallel
had 16 completed frames, 128 graphics, 16 compute, and 32 transfer samples.
Both had zero query drops and VUIDs. Private per-pass timing is unavailable by
design; per-queue timing was verified. C01 serial passed 15 cases and 972
assertions; C04 parallel passed 6 cases and 1,087 assertions. Shared-host
timings remain unaccepted. B0 remains incomplete, with 0 of 3 optimization
passes complete.

## Native timestamp gate and Cargo cache candidate

Exact code at `50a9c492` passed strict pre-push: 774 suites, 0 skipped, 0
failed. Log: `.cache/build/ci/v0261-profiler-cold-pre-push.log`. Cold native
workflow [39014980981](https://github.com/SPOOKEXE/atomic-game-engine/actions/runs/39014980981)
completed successfully for Linux x86_64, Windows x86_64, and macOS ARM64. Each
platform passed Identify, Restore (cold miss), Build, SaveCargo, ProcessIsolation,
Package, and both uploads. Six artifacts were produced; publish was skipped as a
manual dry run. Windows Identify is now validated natively.

The cold run saved three actual caches: Linux 137,645,276 bytes, Windows
45,348,531 bytes, and macOS 26,087,821 bytes. Fresh release Ninja spans were
Linux 431.090 s (AccessKit edge 143.499 s), Windows 654.638 s (111.140 s), and
macOS 360.945 s (54.595 s). These edges overlap; their sum is not wall-clock
critical path time.

The workflow tests a Cargo cache keyed by toolchain, image, preset, vendor,
configuration, and platform. It restores the AccessKit target and Cargo registry
index/cache, while removing top-level archive outputs forces Cargo freshness.
Rebuilding the existing local bench archive gave Cargo 0.270 s, AccessKit Ninja
edge 0.301 s, 0.366 s wall time, and identical archive SHA-256
`dc5ceae1b1e7d486e5ed073fc255bcb223275773fdfbbbb96a7df679bf208e56`. This local
artifact check does not establish fresh-checkout or native CI gains. `actionlint`
1.7.12 passed with external ShellCheck disabled because the tool was unavailable.
Inline Python parse, key stability, sensitivity to Rust flags, preset, image and
platform, and archive-cleanup checks passed. A local Windows-branch simulation
failed with 127 because Linux lacks `python`.
Cold run [39014980981](https://github.com/SPOOKEXE/atomic-game-engine/actions/runs/39014980981)
and warm run
[39016026769](https://github.com/SPOOKEXE/atomic-game-engine/actions/runs/39016026769)
both completed successfully at exact head `50a9c492`. On each platform, warm
Identify, Restore, Build, ProcessIsolation, Package, and both uploads passed, with
six artifacts; publish was skipped. Cargo had an exact cache hit, freshness
recheck ran, and SaveCargo was skipped. Warm Build compiled only `accesskit-c`.

| Platform | AccessKit edge, cold/warm (s) | Ninja span, cold/warm (s) | Build step, cold/warm (s) | Whole job, cold/warm (s) | Rust crate compilations, cold/warm |
|---|---:|---:|---:|---:|---:|
| Linux x86_64 | 143.499 / 31.419 s | 431.090 / 53.558 s | 469 / 83 s | 637 / 287 s | 90 / 1 |
| Windows x86_64 | 111.140 / 30.643 s | 654.638 / 100.025 s | 707 / 157 s | 976 / 489 s | 23 / 1 |
| macOS ARM64 | 54.595 / 19.305 s | 360.945 / 53.975 s | 407 / 108 s | 638 / 345 s | 13 / 1 |

Warm Identify/Restore/Recheck took 1/4/0 s on Linux, 6/2/0 s on Windows, and
1/1/1 s on macOS. Cold Cache Save took 3, 3, and 2 s. Toolchains were Rust/Cargo
1.99.0 on Linux and Windows, and 1.98.0 on macOS. Warm C/C++ job ccache totals
were Linux 1,498 hits / 1,498 calls / 0 misses, Windows 1,488 / 1,488 / 0, and macOS
1,229 / 1,435 / 206. Cold C/C++ misses were 87 / 87 / 293, and warm misses were
0 / 0 / 206, ordered Linux, Windows, macOS. The macOS misses were 200 SDL units (C PCH and 199 C files)
and six process-check test runner units (C++ PCH and five C++ cases). C/C++ counts
include Release Build and process-check, not product compilation alone. At this
checkpoint, the root cause had not yet been established; the following PCH
investigation reproduced timestamp-driven misses locally. Because the source
changes include a public core header and many C/C++ misses, whole-build
differences cannot be attributed solely to Cargo. Shared-host timing remains
unaccepted. B0 is incomplete, with 0 of 3 optimization passes complete. No engine
optimization pass or broad engine speed claim is made.

## Cold profiling in the audio callback

The push at `df1831cc` was refused by the mixer suite: 41 of 42 cases and
22,058 of 22,062 assertions passed. The empty-output call allocated four blocks,
1,152 bytes. GDB traced allocations of 64, 512, 64, and 512 bytes through
`FrameGraph::Get`, its two empty name deques, and `AudioMixer::Render`, with
recording disabled. Logs: `.cache/build/ci/v0261-cargo-cache-pre-push.log` and
`.cache/build/ci/v0261-audio-cold-profile.log`.

`157dd173` keeps initialization on FrameGraph control calls; cold scopes and
reports use only published state. `35e2e8a7` prepares the SDL callback's heap
tag path before opening the stream. Its thread-local guard prevents tag creation
throughout the callback. Missing tags retain their ancestor and count drops;
profiler capacity cannot refuse audio. Existing scope placements are retained.

The new isolated cold test failed four allocation assertions against the old
FrameGraph, with five assertions passing. The fixed test passed all nine.
Active FrameGraph passed 63 cases and 324 assertions; heap profiling passed
14 cases and 76 assertions. The original mixer seed, 3182939086, passed all
42 cases and 22,062 assertions. With SDL's dummy driver, all 106 audio cases
and 150,550 assertions passed. The release test preset built in 164 edges and
passed 14 heap cases and 41 assertions with heap hooks compiled out.

GDB observed two positive SDL Feed calls through their return instructions:
both completed with zero C++ allocations, tag registrations, or static
initialization guard calls; the playback fixture passed 44 assertions. This
does not cover SDK or driver C allocations. Log:
`.cache/build/ci/v0261-audio-feed-profile.log`. Format and documentation checks
passed, with every public entity documented. Exact code at `50a9c492` passed the
strict pre-push gate: 774 suites, 0 skipped, 0 failed. Log:
`.cache/build/ci/v0261-profiler-cold-pre-push.log`. No speed gain or completed
engine optimization pass is claimed; B0 remains incomplete, with 0 of 3 passes
complete.

## Clang PCH cache candidate

A local Clang 21 launcher using CMake-shaped PCH and consumer commands reproduced
timestamp-only PCH churn: after touching only the header and deleting outputs,
the default used four cache misses, zero hits, and changed PCH bytes. With
`-Xclang -fno-pch-timestamp`, it had two cold misses, then two direct hits with
identical PCH bytes. A same-size content edit with the old mtime correctly
failed the consumer static assertion under both variants. The production fix
scopes the flag to active ccache C/C++ Clang or AppleClang PCH creation, excluding
MSVC frontends. The rationale is described in
[ccache discussion 1218](https://github.com/ccache/ccache/discussions/1218).

The isolated C/C++ Clang fixture checks cold misses, mtime-only hits and identical
PCH bytes, then an edited-header third miss and consumer error. Its first version
incorrectly required the edited-header PCH hash to change and failed; the corrected
fixture passed for Clang C/C++ and existing GCC C/C++ argument-boundary and error
checks. Logs: `.cache/build/ci/v0261-clang-pch-fixture.log` and
`.cache/build/ci/v0261-clang-pch-fixture-fixed.log`. `actionlint` 1.7.12 passed
with ShellCheck disabled. Linux strict pre-push runs the Clang and GCC checks.
Exact code at `da132daf75bc920a01a196145414ef3df405819e` passed full strict
pre-push: 774 suites, 0 skipped, 0 failed, all checks passed. Log:
`.cache/build/ci/v0261-clang-pch-pre-push.log`. Native runs
[39017676705](https://github.com/SPOOKEXE/atomic-game-engine/actions/runs/39017676705)
and [39018193934](https://github.com/SPOOKEXE/atomic-game-engine/actions/runs/39018193934)
both completed successfully on Linux x86_64, Windows x86_64, and macOS ARM64.
Build, ProcessIsolation, Package, and both uploads passed on all platforms, with
six artifacts per run; publish was skipped as a manual dry run. Both runs had
Cargo exact hits and freshness rechecks on all platforms. macOS used AppleClang
15.0.0.15000309. Its first run populated the cache with 248 misses; the second
had 1,435 hits / 1,435 calls / 0 misses. The previous warm run at `50a9c492`
had 206 repeated macOS C/C++ misses, so reuse now reproduces on a fresh checkout.

This verifies cache work reuse, not whole-build speedup. The first fixed macOS
Ninja span was 80.131 s, with a 15.313 s Cargo edge. The second was 58.288 s,
with a 20.181 s Cargo edge; the earlier `50a9c492` warm Ninja span was 53.975 s.
SDL compile edges summed to 77.682 s and then 54.658 s, but overlap and are not
CPU time or critical-path time. The fixture took 13/15 s on macOS and 15/16 s on
Linux across the two runs. Actionlint 1.7.12 validated the later macOS-only
condition. No further native run was performed after that workflow-only edit.
Product build flags were unchanged. Linux already runs the fixture in strict
pre-push. Shared-host timing remains unaccepted; B0 remains incomplete, with 0
of 3 optimization passes complete.


## Refreshed domain heap diagnostics

At engine revision `32069a3d`, the optimized heap-enabled `bench` preset rebuilt
`bench_graphcore`, `bench_imagegraph`, `bench_bake`, `bench_audio`, and
`bench_graph` in 41 edges. Configure/build logs are
`.cache/build/bench/v0261-current-heap-config.log` and
`.cache/build/bench/v0261-current-heap-build.log`. GNU C++ 13.3 used effective
`-O3`; `MONO_HEAP_PROFILE=ON` and `MONO_OPTIMISE=ON` were verified. Each of the
five compile/routing suites ran in three fresh processes with
`MONO_BENCH_MEMORY=1`, one sample, and no harness warmups. All 15 invocations
passed. Across each combined run, all 27 heap rows repeated their allocation
bytes, blocks, retained-byte delta, dropped-scope delta, and foreign-free delta.
Every row reported zero retained-byte change, dropped scopes, and foreign frees.
Both prepared audio deadline calls allocated zero C++ bytes and blocks.
Benchmark output stayed on stdout. The diagnostic wrapper mistakenly printed
`rows=0` by checking a literal escaped tab; the actual tab-separated rows were
printed and separately compared correctly.

These counters measure requested C++ payload. They exclude C allocator and GPU
memory, and the header overhead is a model. Scope peaks are exclusive; process
peaks include fixture setup and the process lifetime. The image compile suite
does not measure full evaluation, and the audio routing suite does not measure
full mixing. Runtime probes below address those separate gaps. A running game
and two model servers shared this host; no workload was stopped. Clock and
latency acceptance remains pending. B0 is incomplete, with 0 of 3 optimization
passes complete.


## Whole-domain runtime heap probes

`a3d082eb` adds opt-in full CPU image evaluation and prepared audio mixing
probes, plus `just domain-runtime-memory-bench`. Setup and expected-output
construction are outside each measured call. The v3 rows reset process counters
before one synchronous call, report entry live bytes and additional peak payload,
and keep the output live at the final snapshot. The exclusive tag peak remains
separate. The process total includes prepared fixtures and reference data;
additional payload is not RSS, allocator commitment, or GPU memory.

The command built both changed targets in four edges and passed both complete
existing benchmark suites. Three further fresh processes per domain passed all
14 heap rows and full-output checks per combined run. Allocation bytes, blocks,
retained-byte deltas, per-call additional peaks, and output hashes matched across
all three runs. All rows had zero dropped scopes and foreign frees. Normal mode
with `MONO_BENCH_MEMORY=0` passed without emitting heap probes. Source and compiler
settings match the preceding diagnostic refresh; no engine algorithm changed.

| Prepared evaluation | Allocated C++ bytes / blocks | Retained bytes at snapshot | Additional peak payload bytes |
|---|---:|---:|---:|
| Empty document refusal | 42 / 1 | 42 | 42 |
| Simple 1x1 solid | 11,806 / 185 | 4 | 3,701 |
| Busy 256x256 source/transform/blend | 802,094 / 242 | 262,144 | 786,827 |
| Complex 1024x1024 source/transform/blend | 12,598,574 / 242 | 4,194,304 | 12,583,307 |

The refusal retains its diagnostic string; successful calls retain the expected
RGBA8 output. Every output byte is compared. The existing image reference uses
production pixel sampling/composition helpers, so its pixel arithmetic is not an
independent oracle. Prepared image fixtures have already evaluated during
preflight; these rows do not claim a cold-process measurement.

Audio covers 0, 1, 64, and 511 flat voices plus 64 spatial voices. Each prepared
mixer renders its first and next 512-frame stereo block at 48 kHz. All ten calls
have zero tracked C++ allocations, retained-byte change, and additional peak
payload. Every sample is checked against a scalar authored-order mix, including
stage rounding and final clipping; peak, clipping flag, report fields, and sample
clock are checked too. Spatial gains use the independently tested public spatial
helpers. Zero callback churn does not mean zero residency: the prepared process
held about 18.2 MB of tracked C++ payload, including bounded mixer storage, source,
output, and other benchmark state. Dedicated diagnostic mixers do not advance the
normal timing fixtures.

`76147194` adds ray CPU live bytes/blocks, modeled header overhead, and profiler
error deltas. Its peak is explicitly process lifetime including preflight, not a
per-frame peak. Five lens scenarios, inactive through complex, passed their
existing oracles at 65x65 with eight measured frames on Vulkan, with all added
fields present and zero dropped marks, dropped scopes, and foreign frees. This
checks reporting only, not 1080p performance. The optimized render refresh built
in 148 edges; logs are `.cache/build/bench/v0261-runtime-heap-build.log` and
`.cache/build/bench/v0261-runtime-heap-final-build.log`. Benchmarks remained on
stdout. Publication checks are recorded separately in
`.cache/build/ci/v0261-runtime-heap-pre-push.log`; their final result must be read
before claiming the strict gate passed.

## Native CPU domain verification

`just domain-runtime-verify` builds and runs the three real image, audio, and
procedural delivery suites with heap diagnostics enabled. The verifier requires
the exact four image and ten audio cohorts with their full-output checks, and
all eight C06 cache phases with the four authored fixture sizes. It rejects
missing rows, invalid heap relationships, dropped scopes, foreign frees, delivery
failures, pending installs, and warm-cache source reads. Output stays on stdout;
all timings remain diagnostic and invalid for performance acceptance.

The local Linux job and the final verifier rerun passed. Ruff, strict mypy with
unreachable-code checks, actionlint, and whitespace checks passed. Review caught
and closed an omitted authored C06 fixture-size check. Actionlint ran without
ShellCheck, which is unavailable locally. The manual build workflow's optional
`domain_benchmarks` input defaults to false and uses a separate `bench` build on
each native platform. Windows and macOS verification remains pending until the
actual job steps are inspected; existing optional-job handling means overall
workflow success alone is insufficient. Shipped preset flags are unchanged.
At `6bb3ce63`, the full strict pre-push gate completed: 774 suites, zero skipped,
zero failed, and all remaining checks passed. The push completed successfully;
the remote branch matched the exact local revision.

The procedural harness still states `generator_cache=absent`. Its unchanged
request is regenerated; it is not a cache hit. Its baked artifact fixture uses
local asset publication, reads, and generator-free installation. This does not
measure network delivery cache hits or network bytes. Those dimensions, complete
quiet clock/latency acceptance, and the full B0 matrix remain pending. No engine
optimization pass is accepted; 0 of 3 passes are complete.


## Procedural signed delivery cache baseline

`bd6ded44` adds `just procedural-delivery-bench bench 0`; `bench 1` enables
per-phase heap counters. It runs the real signed directory publication through
`delivery::AssetClient` and `client::ProceduralContent::PumpWorld`. Each of the
four existing C06 authored shapes uses its first two regions and seed 91.
Generation, cooking, signing, recipient construction, and full-output checks
stay outside the measured phase. The phase includes a fresh client's complete
lifetime, manifest verification, pumping, scene installation, and destruction.
The warm phase creates another client against the populated disk cache.

The initial build failed on missing declarations for `SetProceduralSpline` and
`SetProceduralOverride`; their direct header resolved it. A real empty-case run
then reproduced `procedural delivery target capture refused` with child status
-6. The unbound fixture now sets the required collider trigger, matching the
existing integration test. That failure also exposed an owned scratch directory
left behind by an unhandled exception. A local function exception handler forces
unwinding before rethrow. Reintroducing the bad trigger reproduced the refusal
with zero added scratch directories; the valid trigger was then restored. No
engine behavior was changed. Build logs are
`.cache/build/bench/v0261-procedural-delivery-*-build.log`; the initial configuration
and build logs omit the extra suffix.

The final heap-enabled job passed. Three additional fresh processes each passed
all eight phases, with identical counters, heap deltas/additional peaks, asset
hashes, and scene hashes. All source failures, verification failures, refusals,
pending requests, dropped scopes, and foreign frees were zero. Each phase
installed two regions. The heap-disabled mode passed with the same asset/scene
hashes, and the disabled opt-in emitted no diagnostic rows. Successful and
injected-failure runs left no new scratch directories. Output stayed on stdout.

| Scene | Cold cache hits / misses | Cold bundle payload bytes | Warm cache hits / misses | C++ allocation bytes, cold / warm |
|---|---:|---:|---:|---:|
| Empty | 1 / 1 | 16 | 2 / 0 | 125,356 / 59,762 |
| Simple | 42 / 1 | 248,160 | 43 / 0 | 5,077,107 / 1,507,525 |
| Busy | 103 / 1 | 461,426 | 104 / 0 | 9,989,723 / 3,817,084 |
| Complex | 136 / 1 | 817,672 | 137 / 0 | 17,318,568 / 9,177,450 |

Every warm phase fetched zero bundles and zero bundle payload bytes. Cold hits
are legitimate: fetching one requested asset populates its bundle peers. These
are existing cold/warm cache behaviors, not before/after optimization gains.
Every cached byte is checked against both the authored bake and signed manifest.
All mesh buffers and all six object fields are compared against generated scene
output; scopes, PRD1 digests, world placement, fitted bindings, and contact masks
are checked too. Canonical proofs serialize scalars and lengths with a 128 MiB
bound, never raw structure padding.

The source is a directory. Its transferred and expanded counters measure raw
bundle payload, excluding manifest reads. A fresh warm client still reads and
verifies its manifest. Socket bytes are not applicable to this cohort, and OS
page-cache state is unspecified. Heap figures cover requested C++ payload, not
C allocations, RSS, or GPU commitment. The per-call process peak starts from
prepared resident live bytes; retained bytes include the installed recipient.

A concrete work-removal candidate is now visible: every cold cohort reports two
bundle reads for one published bundle, transferring twice its signed bundle
payload. The empty fixture aliases identical assets, so this is not twice the
sum of its authored asset sizes. `AssetClient::Pump` drives bundles before delivering completions.
`Advance` can retain a synchronous directory job while its request still appears
pending; the next pump can start it again. This source explanation is consistent
with the measured duplicate reads. It is recorded for the first optimization
pass, without changing the baseline engine. Quiet clock/latency acceptance and
the complete B0 matrix remain pending, with 0 of 3 optimization passes complete.
Publication checks for this change are recorded in
`.cache/build/ci/v0261-procedural-delivery-pre-push.log`; read their final result
before claiming the strict gate passed.
