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
Actual GPU dependency waits remain unverified; host API durations must not be
relabeled GPU queue waits.

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
