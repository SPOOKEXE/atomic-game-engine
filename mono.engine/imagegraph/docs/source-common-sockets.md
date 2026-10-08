# Captured source common sockets

Common sockets retain process state per source owner: `Updated`, `Name` and
`Position`. `SourceCommonSockets.hpp` replays caller-supplied phase observations;
it does not execute callbacks or reset authored animators. The bounded native
runtime APIs below execute supported callbacks and resolve getters for audited
profiles. They refuse routes that need unsupported callbacks or wrapper
profiles, or lack required host observations. Batch52 verified the native step
and PXC host bridge for audited profiles. Full catalogue coverage, licensed
reference parity and live Studio remain open.

## Captured phases

`InitializeSourceCommonSockets` constructs `Updated=false`, an empty `Name`
and zero `Position`. A changed identity or source type requires explicit
reconstruction. Reopening a source project starts these outputs cold.

`BeginSourceCommonStep` validates one capture per active owner in source order.
`ShowUpdateTrigger` guards reading Update and clearing Updated. A truthy Update
requires a captured completed direct callback. The returned reset receipt
identifies the local setter action the later bridge must perform; this API does
not change an authored animator. `OutMeta` independently guards assignment of
captured runtime display name and position. Inactive owners retain their state.

`CompleteSourceCommonFullUpdates` separately replays completion of an actual
full-update wrapper. It sets Updated even when trigger visibility is disabled;
a safe-mode return preserves the prior value. Readiness, held outputs and
direct callback completion do not prove this assignment.

Collection overrides `stepBegin` without invoking NodeData's common phase and
selects `doUpdateLite`, which does not pulse Updated. Number also selects the
lightweight wrapper. Callers must attest the actual dispatched full wrapper;
they cannot apply full completion to every successful native callback.

## Bounded native runtime lifecycle

Format 11's authored `SourceCommonOwners` registry records source identity,
native owner, flags and source order. It is document data. `Updated`, `Name` and
`Position` outputs live in `GroupRenderSession`; outputs reopen cold. Common
animator writes also live in that session, while authored animator data remains
in the document.

`InitializeNativeSourceCommonRuntime` explicitly reconstructs cold common
outputs, resets common animator writes and readiness, and retains the five
canonical temporal journals supplied by the request: simulation, surface,
random, data and rigid replay. `ReconcileNativeSourceCommonRuntime` handles
explicit structural edits: matching owners survive, new owners receive the
requested initial state, and durable registry order is adopted. Named writer
resets replace retained writer payloads with current authored storage.

`NativeSourceStepBounded` visits active owners in durable source order. It reads
the Update getter only when `ShowUpdateTrigger` is set. A true value invokes the
supported direct callback, then resets that owner's local Update animator. The
step clears `Updated` when the trigger is visible. When `OutMeta` is set, it
writes authored position and display name. If the archive lacks a saved display
name, the host must provide a runtime-name observation; the runtime does not
guess from a catalogue label. Missing required observations cause refusal.

The step works on one bounded candidate session, including all five temporal
journals, and publishes only after the full step succeeds. A refusal preserves
the prior session. Direct callbacks do not set readiness or pulse `Updated`.
The native step does not execute full or Lite wrapper completion. Callback and
wrapper profiles without audited support refuse when that callback is needed.
Collection overrides the base common phase and requires its pending-flag
observations; pending display refresh also requires the host to report it
handled.

`ReadNativeSourceCommonGetter` observes the current common getter without
processing its owner or changing readiness. It uses the session's retained
journals where the request does not supply them. Display refresh and dummy-input
presentation remain host duties, and their completion is not inferred. Hidden
constructor or global helper registration order is not inferred; routes that
need those observations remain unsupported.

## Source proof

The pinned source is commit `b69eca232217360cf1502ef0223523d818606652`:

- `scripts/node_data/node_data.gml`: constructor lines 180-181 and 213-215;
  common step lines 615-628; lightweight/full wrappers lines 1397-1481.
- `scripts/project_data/project_data.gml`: active owner traversal lines 418-434.
- `scripts/node_collection/node_collection.gml`: lightweight dispatch line 2;
  overriding step lines 189-200; child traversal lines 272-277.
- `scripts/node_number/node_number.gml`: lightweight dispatch line 4.

## Native checks

Batch52 passed all 20 joined checks: 3,650 core cases, 376 source IO cases,
1,159 Studio checks and 33 offscreen GPU cases. The native step and PXC host
bridge passed for audited supported profiles. Full catalogue coverage remains
open where callbacks, wrappers or required host observations are unsupported.

All 26 CPU workload profiles passed with 338 calls and 50,492 spans. The
benchmark binary is `41474cedeb94a5c413d57db173430b68f71ceaed8fc267aa5e655341eaca6b6d`.
The common lifecycle workload metadata is unchanged: 13 calls and 468 spans for
64 owners and 16 steps per call, with 85 reset receipts and two bounded
refusals per call. Its retained payload is 11,112 bytes and state FNV is
`3059338748087447014`. Timing output was checked in RAM and not saved, so this
is not a performance claim. Licensed reference parity and live Studio remain
open.
