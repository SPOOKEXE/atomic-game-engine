# Cache Results native CPU contract

`pc.cache_results` executes the defined ordinary-surface branches of pinned
`Node_Cache_Results.update`. Exact source files and hashes are in
`native-cache-results-policy.json`. This is an owned CPU replay profile, without
a licensed runner, GPU, source scheduler or source button parity claim.

The source truncates the surface list to Amount before checking the clock. At
exact first-frame equality it drops the first surface only when the list is
full, appends a new cycle slot and makes that slot current. Other observations
keep the source `surfaceIndex`. A shrink can therefore write at the old index
and make the output longer than Amount again. Growth alone creates no cycles.
Signed fractional frames are compared exactly against Timeline.First. Fresh
direct seeks observe only the requested frame; no missing calls are fabricated.

`surface_set_shader` defaults to transparent clear and `BLEND.alpha`.
`BLEND_ALPHA` uses source factor one for RGB and alpha. An ordinary RGBA8 input
therefore copies its stored pixels, including partial alpha, into the cleared
same-size surface. Source `surface_verify` defaults the destination to RGBA8.
Other finite formats use the existing native numeric conversion and swizzle
profile. Exact device sampling, conversion rounding and single-channel swizzle
remain reference gates.

The caller owns DataReplay. Each node stores one complete immutable surface
list and source current index. The entry owns the exact signed observation
clock; its one list record uses storage key zero. Each successful operation
returns independent output images and a new journal. Amount changes and later
non-surface observations preserve the defined retained surfaces. The source
first-frame branch with no valid input exposes a raw unwritten 1x1 allocation;
it receives a diagnostic. A zero Amount first frame reads a nonexistent first
source slot and is invalid. Sparse source slot extension and Atlas-backed draw
semantics receive explicit capability diagnostics. No pixels are guessed.

Source count and byte limits are checked before copying. Validation workspace
is admitted before validating a borrowed ledger. Complete candidate, immutable
journal and output list storage overlap is admitted before construction.
Failures preserve prior journals and output pixels. CapturedFeedbackHost uses
its existing direct DataReplay path, current-frame checkpoint and same-frame
cache. Clearing that native owner supplies a fresh list; this does not claim a
source Cache Clear button adapter or source serialized cache lifecycle.

The compiled graphs exercise completed cycles, full-list rotation, Amount
shrink/growth, signed fractional calls, direct seek, same-frame host reuse,
clear, all seven native image formats and dimension changes. A compiled Cache
Results to Sequence Animation route verifies retained cycle selection and
independent prior ownership. The executor
boundary tests cover retained non-surface observations, explicit unwritten and
Atlas refusals, and measured full-list admission with unrelated retained bytes.
The joined release64 CPU gate passes all nine Cache Results cases. The validation
ledger records the complete core, product and Composer Vulkan checks. Licensed
reference comparisons and representative profiling remain open.
