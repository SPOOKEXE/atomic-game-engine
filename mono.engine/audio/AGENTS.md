# audio — module invariants

L12 `client`. The mixer graph, spatialisation and the device.
`core-features.md` asks for a DAW-like node pipeline and
`DATATYPES_LIBRARIES.md` §11.2 puts one hard requirement on it; this file is
what a reviewer should refuse.

## Sample-accurate scheduling is the requirement, not a refinement

`DATATYPES_LIBRARIES.md` marks it `!` and names this module:

> *A game ticks at frame rate and audio runs at sample rate. Events scheduled on
> tick boundaries jitter audibly.* … *the tick **queues** an event and the audio
> graph schedules it against the sample clock. This is the one place where
> "close enough to the frame" is wrong, it is audible immediately to anyone who
> notices it, and it cannot be fixed from above.*

So a `Command` carries a **sample deadline** and `AudioMixer::Render` splits its
block at every deadline inside it. At 60 Hz and 48 kHz a frame is 800 samples; a
footstep applied at the top of whichever block comes next lands up to a block
early or late, and a run of them is audibly uneven.

**Do not add a "play now" that bypasses the deadline.** A convenience that
applies at the top of the block is the exact quantisation this exists to remove,
and it will be reached for because it is shorter to write.

`ApplyPending` exists and is *not* that: it is for building a routing before the
clock is running, and using it on the device thread is the bug.

**A deadline in the past is applied, never dropped.** A tick that ran late still
meant its command to happen, and dropping it turns a frame hitch into a missing
sound.

## The device thread has a hard deadline and a missed buffer is audible

Everything in the render path is shaped by that one sentence. The callback
renders a block and copies it out; nothing on that path may allocate, block or
take a lock:

- **The mixer owns the graph and only the callback touches it.** There is no
  lock in this module, and the reason is not that locks were avoided but that
  there is nothing to lock.
- **Commands cross on a lock-free single-producer, single-consumer ring.** One
  tick posts, one device drains. Two of either corrupts it silently.
- **Scratch buffers are sized when the graph changes**, never during a render.
- **A `SoundRef` is copied on the tick side**, so the callback neither allocates
  one nor drops the last reference to one — dropping it would free a buffer
  inside the callback.

A full queue **drops and counts** rather than blocking. The producer is a tick
and the consumer has a deadline: waiting would stall the world to keep a sound,
which is the wrong way round.

## One id space, and the output's is reserved

`AudioGraph` mints ids and so does `CommandQueue::Allocate`, and they name nodes
in the same graph. So `AudioGraph::OUTPUT_ID` is fixed and the queue starts
above it at `FIRST_FREE_ID`.

**This was got wrong once and it failed silently**: the queue's first allocation
collided with the output, the player was adopted as the output, and the wire
from it to itself was refused. No crash, no error, no sound. If a third id
source ever appears it goes through the same constants.

## Cycles are refused at the wire, not detected at the mix

A feedback loop is either infinite recursion inside a callback with a hard
deadline, or unbounded gain — the loudest possible failure. `Connect` walks the
graph and refuses; connecting is not on the audio path, so the walk is free
where it matters.

**The direction of that walk is easy to invert and reads plausibly either way.**
Written backwards it refuses every shortcut across a chain and admits every
loop. `CanReach(start, target)` is named for what the walk does rather than for
the wire, and the suite pins both directions.

## Everything above the device is data, which is why it is all tested

CI has no sound card and a developer's is in use, so `NullDevice` is not a
convenience — it is what makes this module testable at all.
`Renderer::Initialise(nullptr)` made the same choice for graphics, and
`AGENTS.md`'s rule that a header needing a GPU has no unit suite would otherwise
apply here too.

The only part left uncovered is the handover to SDL, and it is small on purpose.

**A machine with no audio output is not an error.** `OpenDevice` answers null
and the caller runs quietly. A game that refused to start because it could not
make a noise would be worse than one that is silent.

## Float, interleaved, and clipped exactly once

**Float because a mixer sums.** Sixteen-bit integers clip the moment two loud
sounds add, so a mixer using them attenuates defensively at every stage and
loses headroom it cannot get back. Exceeding ±1.0 *inside* the graph is legal
and expected; the clamp happens once, at the output stage, and `MixReport::Peak`
reports what the graph produced rather than what survived — measured after
clipping it reads exactly 1.0 for ever, which is the number that hides the
problem.

Gains are **linear**, because the mixer multiplies. A decibel is what a user
interface shows, and converting there keeps the conversion out of the audio
path.

Panning is **equal power**, not linear. A linear pan drops about 3 dB in the
middle, so a sound swept across the front sags as it passes the centre.

## The decoder refuses rather than guessing

`DecodeWav` takes bytes an origin served — `repo_layout.md` §1 says anyone can
run one — and RIFF is a chain of length-prefixed chunks, which is to say a list
of numbers telling a parser how far to jump.

**No length is acted on before it is checked against what actually arrived**,
and the RIFF size field is deliberately not trusted to bound the walk: it is a
number in the file and the buffer's length is a fact.

**A chunk running past the end is a refusal, not a clamp.** Clamping turns a
truncated file into a shorter sound that plays, and the corruption stays
inaudible until somebody wonders why a footstep got quieter.

**A codec this engine does not have is refused, never guessed at.** A decoder
that guessed would produce noise at full volume, which is the single worst
failure this subsystem has. `.ogg`, `.flac` and `.mp3` are classified by the
manifest and are not decoded here — each is a vendored codec and a licence
decision.

## Why this graph is not `engine::graph`

`repo_layout.md` §9 plans one graph runtime with five consumers and lists audio
among them. §16 decision 12 states the ordering: *build `mono.engine/graph/`
against render only, and do not claim it is general until the physics graph is
the second user.* Today `graph` holds the description of a frame and none of the
execution, so routing audio through it would mean building that runtime against
a second consumer before it exists for the first.

This graph is small and deliberately shaped like the eventual one — nodes,
ports, a topological order — so folding it in later is a move rather than a
rewrite. **Do not "unify" them until `graph` has an executor and physics uses
it.**

## Not here yet

Named so nobody adds half of one:

- **Filters of any kind.** No low-pass, no reverb, no occlusion. `Emitter`
  produces two gains and nothing else; a distance-dependent filter is the first
  thing this will want and it needs a filter node to live in.
- **Binaural or surround output.** `CHANNELS` is 2 and `PanGain` is a stereo
  operation. Surround is a change to the output stage, not to the format.
- **A polyphase resampler.** `SampleBuffer::ConvertTo` is linear interpolation
  and is audibly poor at large ratios. It runs at load time and never on the
  device thread, so replacing it later touches a decode path rather than a
  mixer. `DATATYPES_LIBRARIES.md` lists a resampler as a dependency this engine
  does not have.
- **Streaming from disk or from an origin.** A `SoundRef` is a whole decoded
  buffer. Music-length content wants a ring fed by a worker, and that is a
  second kind of player node.
- **Scene components and a scripting surface.** `DATATYPES_LIBRARIES.md` §15.1
  puts `audio` among the bound services — play, bus routing, spatialisation,
  analysis taps — and none of that is bound yet. The mixer is driven from C++.
- **`voice` is not `audio`.** Capture, codecs and jitter buffers are a different
  subsystem and `DATATYPES_LIBRARIES.md` says so explicitly.
