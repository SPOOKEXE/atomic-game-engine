# Part pose replication

Status: design (2026-11-20), not built. Items 1 and 4 of the proposal it
came from are built: the static index patches moved anchored parts in place
(`HashGrid::Refit`), and `BulkMoveTo` lists the few `ClientLocal` rows once
per call instead of probing per part.

Related: [PHYSICS_REPLICATION_OPTIMISATIONS.md](PHYSICS_REPLICATION_OPTIMISATIONS.md),
the measured campaign this would continue.

## What it costs today

`ReplicationStress` moves 20,000 anchored parts every tick. Debug Studio
Play, CPU profile, share of the whole process:

| Where | Share | Notes |
|---|---|---|
| `Authority::PublishMany` | about 27% | `BuildComponents` is half of it; `DetectRows` 18 ms a tick |
| Script `BulkMoveTo` | about 16% | ownership checks were more than half; now 18 ms a beat (was 21) |
| Static index rebuild | 17 ms a tick | unchanged here: every part moves, so the patch path does not apply |
| `collect-replicated` (client) | 12 ms | rebuilds the replica's draw list |

On the wire, a moved part is one row of the generic `Transform` delta:
the entity handle (8 bytes) and the compact transform (`TransformWire`,
10 bytes: three 16-bit positions over plus or minus 2,048 m, and a 32-bit
smallest-three rotation). About 18 bytes a part a tick, against the
acknowledged baseline the delta already carries. 20,000 parts at 60 Hz is
about 21 MB/s for one client, before framing.

## What to specialise

Only the pose of a part (position and rotation), and its velocity when it
has one. Colour, size, attributes, names and tree changes stay on the
generic component path, which is right for data that changes rarely.

### 1. A pose lane on the authority

- **Dirty set from the change list.** Walk `EachChanged<Transform>` once a
  tick into a dense array of `(slot, pose)`, instead of the per-component
  row detection the generic path runs for every replicated component.
- **Stable slots, not handles.** Each client keeps a slot table: the first
  time a part enters its interest set it is sent once with its handle and
  given a small slot number (2 bytes, or 3 past 65,536). Every later pose
  names the slot. This removes 6 of the 18 bytes.
- **Delta from the acknowledged pose.** The baseline mechanism already
  exists (`Delta::Baseline`). For each slot, send the position change since
  the pose that client acknowledged, in a variable width: 1 byte an axis for
  a few centimetres, 2 bytes otherwise, a full 16-bit absolute code on a
  teleport. Rotation is resent only when its packed code changed.
- **Per-client priority and budget.** Order slots by distance to the
  client's eye and by time since the last send, and stop at a byte budget a
  tick. Far parts update less often, never not at all.

Expected size for `ReplicationStress`: slot (2) plus a small position delta
(about 3) plus, rarely, rotation. About 5 to 6 bytes a part a tick, roughly
a third of today, before the budget.

### 2. Predicted motion

- Send linear and angular velocity with a pose when a part has them
  (`MotionWire` already packs them in 12 bytes), or derive it on the
  authority from the last two poses for scripted motion.
- The client extrapolates from the last pose and velocity. The authority
  keeps the same prediction for each client and sends a pose only when the
  error passes a threshold (for example 1 cm or 0.5 degrees) or a keep-alive
  interval passes.
- Steady motion such as this demo's sliding parts, doors and platforms then
  costs a few bytes a second, not a tick.

### 3. Client application

- Poses go straight into the snapshot buffer's interpolation entries, not
  through the generic component apply and its per-component bookkeeping.
- The replica's draw list updates rows by slot. This is the
  per-row change tracking the Magic row of `ZERO_V0261_PROGRESS.md` stopped
  at, and it targets `collect-replicated`.

### 4. Physics on both ends

- A part that moves every tick for a while is a kinematic body, not static
  geometry. When the patch path cannot keep up (many movers, or movers
  crossing cells), move such parts into an index that updates in place, and
  return them to the static index after a quiet period. This needs the pair
  encoding and five consumers (`BroadPhase`, `NarrowPhase`, `Continuous`,
  `Characters`, `Query`) to accept a third index, so it is its own step.

## Order and measures

1. Pose lane with slots and baseline deltas (1). Measure bytes a tick and
   `PublishMany` in `ReplicationStress` and in a few-movers scene; parity is
   the received transforms, bit for bit, against the generic path.
2. Client application by slot (3). Measure `collect-replicated`.
3. Predicted motion (2). Measure bytes a second for steady and erratic
   motion, and the largest visible error.
4. Kinematic index (4), only if a measured scene still rebuilds the static
   index every tick after 1 to 3.

Each step is a protocol change: bump the protocol version, keep the generic
path for parts a peer cannot take, and add a round to the campaign ledger
with its profile, before and after, parity and decision.
