# Resumable authored range export

`GraphExportSession` collects authored Animation and Sequence exports one graph
frame per `Resume`. The host reports pending completion through
`GraphExportSessionHost::Pending()`. A diagnostic string never determines
whether work is pending. This engine module neither owns a renderer nor waits
for a device fence.

`BeginAuthored` borrows a prepared input snapshot to plan the exact controls and
surface-array membership. The caller attests its selected node, document and
observations, as with the existing prepared export API. Admission does not
execute the graph. Preparing that snapshot remains a caller responsibility;
a cold stateful preparation must itself advance a bounded warmup cursor when
its providers can return pending. Planning does not imply that this preparation
was side-effect-free.

The admitted session owns a bounded document copy and compiled plan, resolved
export settings, and an exclusive staging directory inside an existing granted
output directory. It retains no provider, evaluation-request pointer or borrowed
observation span. Every resume receives a fresh stack provider and request.
`GraphExportGeneration` attests immutable authoring, input, observation, grant
and capture-owner generations. The observation generation includes audio and
PCX input identity. Changed generations, seeds or rigid playback observations
cancel the session and preserve previously published files.

`NextFrame()` is the actual frame to bind before calling `Resume`, including
stateful warmup. Temporal evaluation advances by one integer tick, rather than
asking the replay owner to cold-seek across multiple host frames in one attempt.
Successful capability captures and ordered PCX notifications are retained in
`PendingHostObservations` until that frame succeeds. Retries reuse these
observations. `CaptureSequenced` binds deterministic evaluation-local callback
ordinals to exact authored controls and input-image hashes. This permits distinct
processor rows of the same host node. Changed order or inputs refuse before
another capability call, including the previously queued incomplete invocation.
The existing unsequenced collector keeps its single-node changed-input refusal.
These ordinals are process-local and never enter graph or recording formats.
A provider refusal without an explicit pending observation is
terminal. Completed frames are never evaluated again during encoding.

The session admits at most 4096 collected frames and 4096 evaluated steps,
including conservative temporal warmup across targets. These limits apply to
this asynchronous API; existing synchronous range APIs keep their existing
limits. Staged frames total at most 512 MiB. Captured data, previous receipts,
vector backing and actual capacities are admitted before growth. The shared
collector reports its conservative retained payload charge, including receipt
clone footprints and message backing.
`MaximumPayloadReservationBytes()` admits that reservation before beginning an
intent. `PayloadReservationBytes()` reports it while the session is admitted.
This is a conservative capture/collection reservation,
not measured process heap use. Compilation, transient CPU evaluation and final encoding retain their
existing independent operation limits. It is not a whole-process memory cap.

Private `.cframe` files have the named `AGE_GRAPH_EXPORT_FRAME` version 1 header,
with surface-format name, dimensions, pixel hash and byte count, followed by
exact source-format pixel bytes. They are transient implementation files, not
an interchange format. Normalized, half-float and float surfaces keep their
native precision. Reads check regular-file type, exact bounded size, header,
pixel hash and finite samples before returning a completed receipt.

After collection, encoding runs the existing transactional batch exporter with
a private read-only frame provider. The original graph, Lua state, renderer,
file writers and other host effects cannot execute in this phase. Existing
native PNG, BMP, EXR, APNG and CPU GIF paths remain available where selected by
the authored planner. JPEG, WebP, ICO, text-image and video formats still require
the existing exact executable grants and their supported encoder profiles.
Final external CPU encoder polling retains its existing bounded timeout; this
API does not make those process calls asynchronous.

Publication keeps the existing batch backup and rollback behavior. A failure
in any target preserves all previous outputs. Cancel, terminal failure and
successful publication remove owned frame staging. Requested retained debug
frames use the existing bounded retained-directory path; their exact paths are
available through `RetainedDirectories()`.

Call `Cancel(host)` before replacing or destroying a pending intent so its
renderer namespace can retire pending jobs. Destruction can remove the owned
staging directory, but cannot cancel an external provider without retaining a
borrowed pointer. A completed session remains complete on later resumes and
never republishes its files.

The focused fixtures cover explicit pending completion with upstream side
effect reuse, decoded PNG frame pixels, float-to-EXR precision, cancellation,
changed generations, terminal refusal, flat image-array member dimensions and
later-target cache failure rollback.
Their callback provides owned completion observations; it is a host-contract
fixture, not source shader or GPU parity evidence.
