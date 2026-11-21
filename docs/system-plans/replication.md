# Replication performance plan

Open work to make replication scale with moving parts and clients. Built so
far: the static index patches moved anchored parts in place
(`spatial::HashGrid::Refit`), and `BulkMoveTo` lists the few `ClientLocal`
rows once per call.

## Measured problems

`ReplicationStress` (20,000 anchored parts moved every tick, Debug Studio
Play, share of the process):

| Where | Cost |
|---|---|
| `Authority::PublishMany` | about 27%; `BuildComponents` half, `DetectRows` 18 ms a tick |
| Script `BulkMoveTo` | about 16%, 18 ms a beat |
| Static index rebuild | 17 ms a tick (every part moves, so the patch path does not apply) |
| `collect-replicated` (client) | 12 ms, rebuilds the replica draw list |

- A moved part is one generic `Transform` delta row: 8-byte handle plus the
  10-byte `TransformWire` (`scene/Wire.hpp`). About 21 MB/s for one client at
  60 Hz before framing.
- With 8 clients against 1, `PublishMany` goes 28 to 200 ms and
  `play links.deliver` 29 to 223 ms, linear per client, while
  `Authority::Steady` stays at 23 ms.
- `StressChurn.luau` (about 15,000 live, constant spawn and destroy): every
  structural change invalidates the client visibility cache, so
  `Authority::Interest` re-selects the whole tree each tick (51 ms), plus
  `replica.structure` 67 ms and static index rebuilds from added and removed
  pillars.

## Pose lane

Specialise only part pose (position, rotation) and velocity when present.
Colour, size, attributes, names and tree changes stay on the generic path.

1. **Authority dirty set.** Walk `EachChanged<Transform>` once a tick into a
   dense `(slot, pose)` array instead of per-component row detection.
2. **Per-client slots.** A part entering a client's interest is sent once with
   its handle and given a 2-byte slot (3 past 65,536); later poses name the
   slot.
3. **Delta from the acknowledged pose** (`Protocol.hpp` baselines): 1 byte an
   axis for small moves, 2 otherwise, full code on teleport; rotation only when
   its packed code changes. Target about 5 to 6 bytes a part a tick.
4. **Priority and budget.** Order by eye distance and time since last send,
   stop at a per-tick byte budget. Far parts update less often but are never starved.
5. **Client apply by slot.** Poses go straight into snapshot interpolation
   entries and update replica draw rows by slot, targeting `collect-replicated`.
6. **Predicted motion.** Send velocity (`MotionWire`, 12 bytes) or derive it
   from the last two poses; the authority mirrors each client's extrapolation
   and resends only past an error threshold (for example 1 cm or 0.5 degrees)
   or a keep-alive.
7. **Kinematic index.** Only if a measured scene still rebuilds the static
   index every tick after 1 to 6: movers go to an in-place index and return to
   static after a quiet period. Needs the pair encoding and `BroadPhase`,
   `NarrowPhase`, `Continuous`, `Characters` and `Query` to accept a third
   index.

## Other work

- Invalidate interest per subtree on structural change instead of the whole
  client visibility cache.
- Share per-client publish work: the cost that scales linearly with clients
  should be the per-client selection and encoding, not shared row building.

## Rules

- Each lane step is a protocol change: bump the protocol version and keep the
  generic path for peers that cannot take it.
- Parity is the received transforms, bit for bit, against the generic path.
- Every experiment records profile, hypothesis, candidate, matched-build
  before and after, exact parity and a keep-or-reject decision. A faster
  result is not assumed. Benchmarks run through existing `just` jobs and write
  only under the build directory.
- Measure bytes a tick, `PublishMany` and `collect-replicated` in
  `ReplicationStress`, a few-movers scene and multi-client Play
  (`--play-clients N`).
