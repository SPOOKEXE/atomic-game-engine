# VirtualDeviceService plan

## Status and scope

Future work only. None of the APIs or enums below exists yet. This is a general
device service for scripted controllers, accessibility inputs, tracking tools,
and external producers. It does not require VR, a headset, or a particular ML
library. It extends the [input system](input-system.md); the
[VR adapter](vr-system.md) uses the same normalized device and action contracts.

The first acceptance example is an external Python process running MediaPipe or
another hand tracker. Luau receives its output through a host-approved transport,
queues it on a virtual device, optionally preprocesses it, and handles the
committed result through events or polling:

```text
Python tracker -> transport bridge -> Luau receiver -> device:PushData(...)
    -> fixed-tick pump -> optional PreprocessData -> typed validation
    -> atomic sample commit -> device/action state -> OnDataReceived
```

Transport ingestion and inference are separate from device ownership. Current
`HttpService` provides JSON helpers, GUID generation, and URL encoding only;
`GetAsync`, `PostAsync`, and `RequestAsync` are absent. An HTTP example therefore
also needs an asynchronous ingress bridge. Do not document it as runnable today.
See [HttpService.cpp](../../mono.engine/script/src/HttpService.cpp).

## Definitions and device instances

A definition declares a stable name, one immutable schema, and multiplicity.
Instances share that definition but have independent queues, state, calibration,
callbacks, and connection status.

| Proposed enum | Meaning |
|---|---|
| `Enum.VirtualDeviceDefinitions.Singleton` | At most one live instance per definition within a world and local-player scope |
| `Enum.VirtualDeviceDefinitions.Multiple` | Multiple named instances of the definition within the same scope |

Singleton is a count constraint, not a global device shared across clients.
Scripts in the same scope resolve the same registry, so creating another VM or
calling from another script cannot bypass the constraint. A second creation
fails with `AlreadyExists`; callers explicitly retrieve the existing device.
An instance continues to occupy its slot while disconnected. Only destruction
frees it. Multiple instances require unique names within their definition and
scope. Both modes obey the separate overall live-device quota.

Definitions and fields use stable string names in assets, profiles, and traces.
Runtime device handles carry generations; destroying and recreating an alias
cannot make a stale handle reach the new device. A conflicting redefinition is
refused. Replacing a schema requires a new revision and a controlled migration.

## Typed values and limits

Each definition declares between 1 and 32 named values. Every instance and
committed sample has that same maximum of 32 values. This is independent of the
input plan's 32 live devices and eight compatibility gamepad slots.

Reuse the input action kinds through a proposed `Enum.VirtualDeviceValueType`:

| Kind | Examples |
|---|---|
| `Boolean` | button, tracked flag, recognized gesture |
| `Scalar` | trigger, confidence, grip strength, throttle |
| `Vector2` | stick, touchpad, normalized pointer |
| `Vector3` | landmark, velocity, spatial direction |
| `Pose` | transform with position/orientation validity |

A vector or pose occupies one declared value. Arbitrary nested arrays, strings,
and buffers are not typed controls and cannot hide unlimited values inside one
slot. Bounded raw transport data may contain records and arrays before
preprocessing. Text input remains on the input system's text path.

Field descriptors specify kind, neutral/default value, optionality, and relevant
range, units, or coordinate-space metadata. Initial values are neutral. Every
commit replaces a complete sample: all required fields must be present and
missing optional fields reset to their declared defaults. Unknown fields,
wrong types, NaN, infinity, invalid transforms, and range violations refuse the
whole sample. There is no silent coercion or partial update.
Canonical field order is lexicographic by stable field name, independent of
Luau table traversal order.

Provisional ceilings for implementation and burst testing:

| Limit | Policy |
|---|---|
| declared values per definition/instance | hard maximum 32 |
| live instances | share the input runtime's 32-device ceiling, including disconnected instances |
| registered definitions | at most 32 per world/local-player scope, including unused definitions |
| pending samples | at most 32 per device and 256 per owning runtime |
| owned ingress data | at most 64 KiB per raw sample and 1 MiB pending per owning runtime |
| raw nesting | at most 8 levels and 1,024 entries per sample |
| names | at most 64 UTF-8 bytes, no empty names or control characters |

Bound before copying or allocating. Define exact counting and encoded-size rules
in the neutral schema before implementation. Quota refusal returns a stable
reason and increments diagnostics; it never evicts another producer's device.

## Proposed Luau surface

Signatures are design contracts, not existing declarations:

```text
VirtualDeviceService:DefineDevice(name, multiplicity, schema, options?) -> definition
VirtualDeviceService:GetDefinitions() -> ordered definition descriptors
VirtualDeviceService:GetDefinition(name) -> definition descriptor | nil
VirtualDeviceService:GetDevices(filter?) -> ordered observer handles
VirtualDeviceService:GetDevice(definitionName, instanceName?, localPlayer?) -> observer | nil
VirtualDeviceService.OnDeviceConnected(device, metadata) -> event
VirtualDeviceService.OnDeviceDisconnected(device, reason, metadata) -> event

definition:CreateDevice(instanceName?, options?) -> producer handle
definition:Destroy() -> refuses while instances exist

device:PushData(rawData, sourceMetadata?) -> queued, reason?
device.PreprocessData -> callback | nil
device:GetLatestSample() -> immutable sample snapshot | nil
device:GetValue(fieldName) -> typed value
device:GetStatus() -> status snapshot
device:GetDiagnostics() -> bounded counters
device:Disconnect(reason?)
device:Destroy()
device.OnDataReceived(sample, metadata) -> event
device.OnValueChanged(fieldName, newValue, oldValue, metadata) -> event
device.OnStatusChanged(status, reason, metadata) -> event
device.OnError(errorInfo) -> event
```

`PreprocessData(rawData, sourceMetadata)` returns a complete typed record or nil
to deliberately discard a sample. Nil callback means validate the submitted
record directly. It runs once per dequeued sample, on the owning VM thread,
under the script step budget. It cannot yield, perform I/O, or recursively pump
devices. Callback replacement takes effect at the next pump boundary. Errors,
budget exhaustion, and invalid output leave the last committed sample unchanged
and produce a bounded diagnostic and `OnError`; nil output is an intentional
drop, not an error.

`PushData` copies and queues. A true return means queued, not processed. It
never calls preprocessing or subscribers inline. A handler that enqueues more
data schedules it for the next pump. Producers cannot modify committed samples
through a retained table; each event receives an immutable snapshot. Event
handlers may not yield and run within the same bounded delivery budget. A
failing subscriber does not prevent other subscribers or devices from running;
an error handler failure is logged without recursively firing `OnError`.

The creator owns the write handle and callback. Enumeration returns observers
that can read and subscribe but cannot submit, replace callbacks, disconnect,
or destroy a device. Read access remains local to the permitted world/player
scope. `GetDevices` filters by definition, scope, and status and returns stable
creation order; `GetDefinitions` returns stable name order. Explicitly sharing
a producer capability is a later host decision, not an alias-based permission.
Definition destruction requires its creator's authority too.

## Example: two hand trackers

Proposed usage, after a transport has already delivered decoded data to Luau:

```luau
local Devices = game:GetService("VirtualDeviceService")
local handDefinition = Devices:DefineDevice(
    "TrackedHand",
    Enum.VirtualDeviceDefinitions.Multiple,
    {
        Tracked = { Type = Enum.VirtualDeviceValueType.Boolean, Default = false },
        IndexTip = { Type = Enum.VirtualDeviceValueType.Vector3, Default = Vector3.zero },
        Pinch = { Type = Enum.VirtualDeviceValueType.Scalar, Default = 0, Min = 0, Max = 1 },
    }
)
local left = handDefinition:CreateDevice("Left", { StaleAfterTicks = 12 })
local right = handDefinition:CreateDevice("Right", { StaleAfterTicks = 12 })

left.PreprocessData = function(raw, metadata)
    return {
        Tracked = raw.tracked,
        IndexTip = Vector3.new(raw.index_tip.x, raw.index_tip.y, raw.index_tip.z),
        Pinch = raw.pinch,
    }
end

left.OnDataReceived:Connect(function(sample, metadata)
    if sample.Tracked then
        -- Game code handles the accepted input here.
    end
end)

Devices.OnDeviceDisconnected:Connect(function(device, reason, metadata)
    -- Game code releases gestures and updates its connection UI here.
end)

local function onLeftHandPacket(decoded, sourceSequence)
    return left:PushData(decoded, { SourceSequence = sourceSequence })
end
```

The right-hand instance may install its own processor or accept already typed
records with a nil callback. An HTTP, WebSocket, local IPC, plugin, or replay
receiver can call the same function; these are adapter candidates, not claims
that those transports are script-accessible today.

MediaPipe documents 21 landmarks per hand and distinguishes normalized image
coordinates from world landmarks. One device per hand can expose all 21 as
`Vector3` fields plus up to 11 derived controls. Two full hands do not fit in one
32-field device. The example uses only one landmark and expects the producer
to provide pinch strength; it does not claim MediaPipe returns that field.
Coordinate conversion, handedness/mirroring, origin, and scale must be explicit.
World landmarks alone do not establish a calibrated position in engine world
space. See the [Hand Landmarker guide](https://developers.google.com/edge/mediapipe/solutions/vision/hand_landmarker).

## Pump, events, and lifecycle

The host drains a bounded ingress batch at the start of each fixed-tick input
phase. It freezes that batch before any callbacks run and processes samples by
host admission sequence. Source timestamps are diagnostic, not scheduling
authority. Optional source sequence numbers reject duplicate or older packets
within an explicit source session; reconnect starts a new session generation.

For each accepted sample, preprocess, validate, and commit all fields together.
Normalize its controls and evaluate affected action state before dispatching
events. The order is `OnDeviceConnected` on first valid data or recovery, then
`OnStatusChanged` if needed, `OnDataReceived`, and `OnValueChanged` for changed
fields in schema order. Subscribers run in connection order and observe the
committed snapshot. Equal samples still fire `OnDataReceived` because receipt
is meaningful; `OnValueChanged` fires only on changes.

States are `Created`, `Connected`, `Stale`, `Disconnected`, and `Destroyed`.
Creation registers the instance but does not claim a working producer. The
first valid sample connects it. `StaleAfterTicks` is a finite positive option
with a documented host default; only a successfully committed sample resets
it. Rejected or intentionally dropped packets cannot keep an input alive.
Tracking validity is field/pose data, separate from transport connectivity.

On timeout, explicit disconnect, focus loss, or source-session loss, atomically
neutralize controls, invalidate poses, cancel action contributions and output,
then emit `OnDeviceDisconnected` once for a previously connected device and
`OnStatusChanged`. Changed fields emit `OnValueChanged` with cancellation
metadata. Lifecycle cancellation does not fire `OnDataReceived` as if a packet
arrived. Focus recovery requires fresh data; old queued packets are discarded.
Pose validity loss likewise cancels its dependent actions even if the producer
remains connected. Reconnection needs a new valid sample and never replays old
button edges.

`Disconnect` clears pending samples but retains the registered instance.
`Destroy` also removes it from enumeration, disconnects subscriptions, releases
callback refs and the quota slot, and invalidates its handles. VM teardown,
script-owner destruction, Studio Stop, and world exit perform the same cleanup.
Repeated disconnect or destruction is harmless. Destroying a device during a
callback cancels remaining delivery to it without invalidating the pump's walk.

Keep ordered FIFO delivery initially. A full queue refuses the newest sample
and reports `QueueFull`. A later explicit latest-only mode may coalesce
continuous tracking frames, but must preserve digital transitions or declare
that those fields cannot use that mode. Do not silently lose button taps.

## Integration and ownership

The device layer can deliver events and snapshots before named actions ship;
it must not depend on completing the entire input roadmap. Action integration
then maps stable `virtual/<definition>/<instance>/<field>` controls through the
normal processors, contexts, local-player assignment, and fixed-tick sampler.
Devices never write `ControllerState` to impersonate a physical gamepad. They
do not acquire a fake Player or bypass semantic character intent validation.

Shared typed definitions, current samples, and status belong in ECS-owned
world state, separate from the pinned `InputState`. A client-only adapter owns
transport handles and opening devices. `script` declares neutral service and
event facts; Luau and JavaScript adapters own their callback references. Do not
widen the cross-world value codec to carry VM callbacks or device pointers.
Headless tests use copied ingress fixtures without SDL, Python, a model, or a
network. Synthetic server-side devices are possible only when explicitly
created in a server-local scope, never by writing a remote client's registry.

Ordinary game scripts may create clearly marked virtual devices in their own
scope. This does not authorize hardware impersonation, OS input injection,
camera access, a listening socket, or arbitrary outbound requests. Transport
access is granted separately by the host. The first external bridge should be
local and explicit, with bounded requests/completions, source-session identity,
endpoint policy, cancellation, and reconnect. Polling Python over HTTP and
accepting pushed HTTP are different adapter designs; choose one before coding.
Use asynchronous completions admitted at the tick boundary, never blocking
network work inside `PreprocessData` or an event subscriber.

Device definitions may be project content. Runtime handles, queues, callbacks,
and raw tracking frames are local and not saved or replicated automatically.
Only validated semantic gameplay intent uses the existing network path. Record
accepted typed samples with schema revision, source session, admission order,
and tick for input replay; replay injects committed samples without rerunning a
model, transport, or preprocessor. Record cancellation transitions too. Raw
ingress traces are a separate opt-in diagnostic and are not required for replay.

## Useful extensions and validation

Include these with the first device slice: status reason and last-data age,
per-device sequence and accepted/rejected/dropped counters, range diagnostics,
schema inspection, and a Studio monitor that performs no work while closed.
Use named profile settings for coordinate conversion, calibration, deadzones,
gesture hysteresis, and debounce; processed digital edges must remain tick
deterministic. Device output such as rumble stays on the separate bounded
capability-checked output path. Script-created devices do not claim physical
output capabilities.

Deliver in this order:

1. Neutral schemas, quotas, Singleton/Multiple registry, generation checks,
   snapshots, lifecycle, and device enumeration with headless tests.
2. Luau and JavaScript pumps, preprocessing, signals, generated declarations,
   budget/error isolation, and teardown tests using synthetic samples.
3. One approved external bridge and a Python hand-tracking example, with a
   recorded fixture for automated checks and a live demo for manual validation.
4. Named-action binding, calibration profiles, diagnostics, and replay through
   the input plan's existing seams. Physical controller coverage remains in
   the input and VR plans, not inside the Python bridge.

Acceptance tests cover exactly 32 values and refusal of 33; every kind and
range; neutral defaults; both multiplicities across two scripts/VMs; independent
left/right instances; stable enumeration and stale handles; nil and failing
preprocessors; immutable snapshots; FIFO order and callback-enqueued deferral;
event order; repeated equal samples; queue/byte limits; failed-sample atomicity;
duplicate and reordered source packets; stale/tracking/focus cancellation;
reconnect; destruction during delivery; owner/world teardown; language parity;
and deterministic replay without the external producer. Profile the pump and
callbacks separately under burst loads before fixing provisional budgets.
