# Typed shared scenarios

`ScenarioSession<Sample>` owns one set of checks and a bounded immutable timeline.
The native adapter prepares and advances its existing runtime. After its native
barrier it publishes copied facts with a stable hook and original logical stamp.
Every matching check sees the same const observation object. The blocking check
batch joins before `Publish` returns, so the next native mutation cannot overlap
observation reads. The host starts and stops the existing job pool.

A sample must own its nested buffers and strings. Do not put a borrowed span,
world pointer, device handle, reference wrapper or shared mutable alias inside
it. `RetainedBytes` counts the actual copied payload, including nested buffers.
The runner adds the actual hook and failure string lengths to that payload
count, then checks the publication cap and both timeline bounds before retaining
it. Bounds describe logical owned payload, not allocator spare capacity or heap
commitment. The adapter must bound construction and native readback allocations
too.
Streaming checks leave `RetainTimeline` false. Temporal checks declare it true;
retention loss fails only checks depending on the affected hook.

Each check declares its original suite and stable name. Required hooks have a
positive minimum sample count. Missing, unavailable, incomplete or lost evidence
fails dependent checks. Record value findings with `CheckResult::Record`; report
those findings through the assertion framework on the calling thread afterward.
A throwing check is disabled while other checks continue receiving observations.
Completion callbacks receive only matching retained hooks. Checks which do not
request retention receive an empty span. These callbacks still run when evidence
failed, allowing useful diagnostics; they must inspect their inputs rather than
assume requirements held.

`Record` bounds each check's finding count and total property/message bytes.
Overflow sets `Passed` false and increments exact dropped-finding counters, even
when the rejected finding would have passed. Failed properties remain failures
when their text cannot fit. Use `Record` for output; treat the published Findings
vector as read-only. Key/attribution/hook text and owner failure diagnostics have
separate budgets. Oversized identities are refused without retaining them in a
report. Exception text is admitted through the same bounded paths.

`ScenarioDriver::Finish` runs exactly once, including preparation, timeout and
cancellation failures. Deadlines and stop requests are cooperative: call
`Checkpoint` in native loops which do not publish regularly. A blocked native
operation cannot be safely preempted. Cleanup finishes before check completion,
which proves those checks cannot rely on a live runtime.

`ScenarioKey` describes all relevant initial state, schedules, actions, transport,
faults, capture and schema choices. Explicit canonical strings can represent
unused features. `CheckSet` identifies callback semantics and must be nonempty
for cached runs. `ScenarioCache<Sample>`
is bound to its creating owner thread and selected execution group. It stores
successful reports under both entry-count and aggregate logical byte bounds,
never a live world or captured payload. Same-key structural changes to checks or
bounds fail clearly. Every lookup validates the request before reusing a report.
The cache moves call-owned keys and checks into one session, validates without
copying callback captures, then reuses that session on a miss. Move check vectors
into the call when their captures cannot be copied.
Cache bytes include keys, check declarations, findings and owned text. FIFO
eviction is deterministic; reports larger than the byte budget are not cached.
Change the key when callback behavior changes. Failed runs
are not cached. Clear the cache when the group finishes. Cache hits report zero
new execution counters and set `Reused`, so they cannot masquerade as a faster
execution measurement.
