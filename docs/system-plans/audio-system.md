# Audio system plan

## Status

This document defines future audio work. It extends the existing mixer, scene
components, content delivery, and scripting surface. It does not describe a
complete runtime that exists today.

The first production target is a dependable stereo game mixer with spatial
sound, buses, bounded effects, zones, streaming music, sample-accurate
automation, offline rendering, and complete Studio inspection. Surround,
binaural output, voice chat, third-party effect plugins, and a general graph
runtime remain later work.

## Product goal

An author can place a sound in a world, route it through a reusable mix graph,
shape how it travels through space, automate it against a stable sample
timeline, and inspect the result without writing device code. The same graph
and DSP kernels must drive live playback, Studio audition, and deterministic
offline rendering.

The system must provide:

- one source and listener model across scene instances, scripts, and Studio;
- sample-accurate starts, stops, parameter changes, and automation;
- distance attenuation, panning, Doppler, occlusion, portals, and audio zones;
- buses, sends, snapshots, effects, meters, and bounded analysis taps;
- whole-buffer playback for short assets and streamed playback for long ones;
- strict device-thread safety with no allocation, blocking, locks, file access,
  logging, or script calls in the render path;
- stable saved and replicated intent with client-local rendering state;
- graceful silence when content, a codec, or an output device is unavailable;
- bounded work and hostile-media limits at every decode and graph boundary;
- headless server support without linking the audio device module;
- useful Studio authoring, diagnostics, offline export, and profiling.

Audio is presentation. It may reflect gameplay, but gameplay never advances,
branches, or completes work from the audio sample clock.

## Current foundation

### Mixer and device

`mono.engine/audio` is an L12 client module. It currently owns:

- interleaved 32-bit float sample buffers;
- WAV and MP3 decoding;
- load-time format conversion;
- a closed graph with player, fader, emitter, bus, and output nodes;
- a bounded single-producer, single-consumer command ring;
- sample-deadline commands and block splitting at deadlines;
- equal-power stereo panning and bounded distance attenuation;
- one listener pose per graph;
- SDL output plus a deterministic `NullDevice`;
- mix reports for peak, clipping, applied commands, and completed voices.

The output clips once after the float graph. Gains inside the graph are linear.
The UI may display decibels, but the render path must not convert a stored
decibel value per sample.

The current graph refuses cycles when a wire is created and builds a stable
topological order. Its node limit and command capacity are explicit. These
bounds remain required even when the node set expands.

### Scene and client adapter

`scene::Sound` is shared authored state. It stores a published asset name,
volume, loop state, playing state, and minimum and maximum rolloff distances.
A sound inherits position from its parent. A sound without a placed parent is
non-positional.

`scene::AudioState` is a per-world resource. It stores the world master gain
and either a camera listener or an instance listener. A headless server may
store and replicate these values, but it does not produce audio.

`client::SoundStage` is the current tier adapter. It walks `scene::Sound` rows,
resolves delivered content, builds mixer voices, posts changed values, retries
coalescable commands after queue pressure, and retains terminal teardown work
until it can be posted. One stage exists per world because entity ids are local
to a store while one client mixer may host several worlds.

This remains the governing dependency direction:

| Layer | Owns |
|---|---|
| shared scene | authored and replicated audio intent |
| client integration | listener resolution, spatial queries, asset residency, voice policy, and mixer commands |
| audio module | decoded data, graph compilation, DSP state, sample scheduling, mixing, and device output |
| Studio | authoring views, audition, meters, and export controls |

The audio module must not acquire an ECS dependency. The scene module must not
acquire an audio or device dependency.

### Content and scripting

Audio is already a stable `AssetKind`. WAV, Ogg, FLAC, and MP3 names classify as
audio in content metadata. Runtime decoding currently accepts WAV and MP3 by
inspecting bytes rather than trusting the file extension. Ogg and FLAC are not
yet decoded.

Delivery verifies content-addressed bytes and leaves format parsing to the
consumer. Studio already treats `SoundId` as an audio content property and
opens the shared asset picker for it.

`SoundService` currently exposes world volume and listener get or set methods
to both script languages through `scene::AudioState`. It cannot name the L12
mixer. Future script APIs must continue to write shared scene intent or invoke
a client adapter. They must never hand a mixer node, device pointer, or callback
thread object to a script VM.

### Graph decision

The current audio graph intentionally does not use `engine::graph`. The shared
graph runtime has not yet reached the point at which audio can consume a proven
executor. Audio graph work continues in `mono.engine/audio` until the generic
runtime has an executor, a second non-render consumer, bounded real-time
execution rules, and evidence that migration removes code rather than wrapping
it.

Studio may reuse its nodegraph widget as a view. The saved audio graph schema
and compiler remain engine-owned data and are not Qt objects.

## Non-negotiable rules

1. The device callback allocates nothing, blocks on nothing, takes no lock,
   accesses no file or network resource, logs nothing, and invokes no script.
2. One device thread owns the live DSP graph and its mutable processing state.
   Other threads communicate through bounded queues and prepared immutable
   objects.
3. Commands that affect audible timing carry an absolute sample deadline.
   There is no `play now` path that quantises work to a block boundary.
4. A command whose deadline is in the past applies at the next safe sample. It
   is counted as late and is not silently discarded.
5. Gameplay uses the fixed simulation tick. Audio clocks, completion events,
   meters, and device latency may not steer authoritative gameplay.
6. Saved assets and wire formats use stable names and versioned bytes. Runtime
   node ids and device handles never cross a world or process boundary.
7. Decoders trust the received span length, not a length declared by media.
   Every output allocation has an engine-owned ceiling.
8. A missing device is ordinary. The client continues with a null or silent
   backend and reports the condition once.
9. Headless targets may store audio intent and validate graph assets, but they
   do not link SDL audio, decode for playback, or run DSP.
10. A graph is compiled and costed before it becomes live. The callback does
    not discover topology, allocate scratch, or reject an authored graph.
11. Every cross-thread queue has one named overflow policy and counters that
    reveal it.
12. Render output is local presentation. Replication carries intent and timing,
    never PCM blocks, meters, decoded buffers, or DSP state.

## Architecture and ownership

No new general audio module is required. The existing `mono.engine/audio`
module is the right owner for sample data, DSP, compiled graphs, the mixer, and
devices. Work should split into focused headers and sources as the node set
grows rather than creating a parallel mixer.

| Owner | Responsibility |
|---|---|
| `scene` | `Sound`, buses, zones, environment references, saved playback intent, and replicated revisions |
| `audio` | codecs, sample conversion, graph document values, compiler, processors, voices, automation, mixer, meters, and device abstraction |
| `spatial` and `scene` queries | geometry and portal facts queried outside the callback |
| `client` | resolve listeners, sources, worlds, portals, occlusion, device settings, content, and voice priority into audio commands |
| `assets` and `delivery` | classify, publish, verify, cache, and deliver encoded audio plus graph assets |
| `script` | language parity over scene and client-facing service requests |
| Studio | graph and waveform editors, mixer view, zone tools, audition, diagnostics, and offline render requests |

The exact dependency graph must be checked when new value types are placed.
If a reusable graph document must be understood by shared scene serialization,
put its plain data below both scene and audio or store it as an opaque,
versioned asset. Do not move the mixer downward to make serialization easier.

## Source model

### `Sound` remains the ordinary source

`Sound` stays the simple path for most games. Its parent determines whether it
is positional. Planned properties are:

| Property | Meaning |
|---|---|
| `SoundId` | stable published audio asset name |
| `Volume` | linear source gain |
| `PlaybackSpeed` | bounded source-rate multiplier |
| `Looped` | restart at the declared loop region |
| `Playing` | saved playback intent |
| `TimePosition` | requested seek position, exposed through a revisioned command rather than per-frame replication |
| `RollOffMinDistance` | full-gain radius |
| `RollOffMaxDistance` | silence radius |
| `RollOffMode` | stable named attenuation curve |
| `EmitterSize` | optional volumetric source radius or extent |
| `AudioBus` | optional bus instance or stable bus name |
| `Priority` | authored voice importance within a bounded range |
| `PlaybackPolicy` | catch up, restart, or discard when content arrives late |

Existing fields keep their present meaning. New fields need explicit defaults,
padding, save coverage, property registration, bindings, and migration tests.

`TimePosition` is not polled from the device cursor into shared ECS every
frame. A seek request carries a revision and target time into the client stage.
A client-local computed playback position may be exposed for display and
scripts that explicitly accept presentation timing.

### Source shapes and direction

The first spatial source is a point at its parent's transform. Volumetric
sources extend placement with a sphere or box extent. A listener inside the
extent hears the source without a hard centre-point pan.

Directional sources add a forward vector and an authored directivity curve.
The client derives orientation from the parent transform and posts a compact
placement value. The callback evaluates only the prepared curve table. It does
not evaluate splines or inspect transforms.

Source position, orientation, and velocity are derived from the authoritative
transform and recent client presentation samples. They are not duplicated as
saved fields on `Sound`.

### One-shot playback

Class-specific instance methods should eventually provide `Play`, `Pause`,
`Resume`, and `Stop`. These methods update the same state and revisions as
properties. They do not introduce a private voice path.

`SoundService:PlayLocalSound` may create a bounded client-local one-shot voice
without a replicated instance once the adapter owns completion and retirement.
The request copies a stable asset name and settings. It never passes a
`SoundRef` or node id through the script boundary.

One-shot pools have explicit per-world and global limits. Overflow uses a
documented priority policy and increments a refusal counter.

## Listener model

The default listener is the composed camera pose. An object listener follows a
live scene instance. Both modes provide position, forward, right, up, and a
smoothed presentation velocity to the client audio adapter.

If an object listener disappears, the client falls back to the camera and
records the fallback. It never moves the ear to the origin.

The full listener pose unlocks directional panning and Doppler. Existing
position-only behavior remains the migration baseline. Rotation must not be
added to the enum until the client actually posts and the mixer actually uses
it.

Multiple local listeners are not mixed into one output in the first release.
Split-screen needs either one selected listener, separate output submixes, or a
defined compromise. It must not average positions because that puts the ear in
a place no player occupies.

Listener choice and output device are client-local. A server may store a
world's listener mode for script compatibility, but camera pose and device
state are not replicated.

## Spatialization

### Attenuation

The current full-gain and silence distances remain the simple authoring model.
The engine supplies a small closed set of stable rolloff modes:

- inverse distance, normalized to reach silence at the maximum distance;
- inverse-square style, preserving the existing response;
- linear, for deliberately even fades;
- a baked custom curve table.

Custom curves are monotonic by default, sampled into a bounded lookup table at
compile or property-commit time, and evaluated without allocation. Studio shows
distance in world units and gain in both linear and decibel forms.

Invalid ranges are repaired at the adapter boundary. A maximum at or below the
minimum becomes an immediate cutoff with a diagnostic, not a divide by zero.

### Panning and output layouts

Stereo keeps equal-power panning. The listener right vector determines left
and right placement. Front and rear ambiguity is acceptable for the first
stereo path and must be visible in the Studio spatial preview.

The spatializer contract should return an output-channel gain vector rather
than embed stereo fields once a second output layout is implemented. Stereo
remains the only shipped layout until surround or binaural work has measured
cost, test fixtures, and a device matrix.

### Doppler

Doppler changes source playback rate from relative radial velocity, the speed
of sound, world distance scale, and an authored scale. It is presentation only.

The client computes stable source and listener velocities from presentation
poses using a monotonic clock. Teleports, portal transitions, respawns, and
large discontinuities reset velocity history. They do not create an extreme
pitch sweep.

The adapter posts a target playback ratio. The mixer smooths toward it over a
bounded number of samples. Ratios are clamped to an audible and computationally
safe range. Manual playback speed and Doppler multiply once before resampling.

The first implementation may use the existing fractional cursor interpolator
for correctness. A measured high-quality resampler must replace it before
large rate shifts are claimed as production quality.

## Occlusion, portals, and propagation

### Occlusion queries stay off the callback

The device thread never raycasts. `client::AudioEnvironmentStage` gathers
source and listener poses, spends a bounded spatial-query budget, and posts
smoothed propagation parameters.

Each spatial voice may receive:

- direct-path gain;
- obstruction and occlusion factors;
- low-pass cutoff or prepared filter coefficient target;
- path distance for delay and attenuation;
- reverb-send adjustment;
- a propagation revision.

A single ray is the initial cheap path. Higher quality uses a fixed pattern of
rays against the same collision masks. Results are amortized across frames,
cached by source, listener, geometry revision, and portal topology revision,
then invalidated when one of those inputs changes materially.

The budget favors loud, nearby, high-priority voices. A stale result fades
toward the unobstructed value rather than snapping or remaining blocked
forever.

### Materials

Surface materials may provide audio transmission, obstruction, and reflection
coefficients beside their physical and visual facts. The propagation stage
combines only a bounded number of hits. It does not trace an unbounded acoustic
simulation.

Missing coefficients use a neutral, documented default. Audio must not infer a
material from a texture filename.

### Portals

The client already gathers portal seams and cross-world mappings for rendering
and collision. Audio consumes the same derived seam facts rather than measuring
a second portal geometry.

An audible portal path contains copied values:

- stable source world and destination world names;
- seam transforms;
- aperture area and openness;
- accumulated path length;
- transmission gain and filter target;
- a bounded hop count;
- the portal topology revision.

The propagation stage searches a bounded number of portal hops and candidate
paths. It chooses or blends the strongest paths with hysteresis. A closed or
disabled portal removes its path. A source visible through a portal is not also
mixed as an unattenuated direct source unless a direct physical path exists.

Portal traversal maps source position and orientation before panning. It resets
Doppler velocity history on a discontinuous path change. Cross-world paths use
owned values and stable world names, never store pointers into another world.

Diffraction and full geometric acoustics are later work. The first path is
aperture transmission, distance, low-pass filtering, and zone sends.

## Zones and reverb

### Audio zones

An `AudioZone` component or class describes a volume through its parent's
existing bounds or collider. It contains:

- a stable environment profile name;
- priority;
- interior and exterior blend distances;
- wet and dry multipliers;
- optional bus snapshot name;
- enabled state.

The zone does not copy geometry. A zone without a usable parent volume is
inactive and diagnosed in Studio.

The listener resolves overlapping zones by priority, then blends equal-priority
zones by bounded spatial weights. Sources may also use their own zone for early
reflection or send selection. Portal paths carry source-zone ambience into the
listener zone with transmission loss.

### Reverb profiles

An `AudioEnvironment` asset stores perceptual controls such as room size,
decay, damping, diffusion, early reflection level, and wet gain. The first
runtime effect is a bounded algorithmic reverb whose delay storage is allocated
when the graph is prepared.

Impulse-response convolution is a later effect node. An imported response must
have bounded duration, channels, sample rate, partition count, and memory. Its
partitions are prepared on a worker before graph activation. No impulse
response is transformed on the callback.

Zone changes ramp sends and effect parameters over samples. A zone boundary
must not click.

## Buses, sends, and snapshots

### Bus hierarchy

Add a shared `SoundGroup` compatibility class or an engine-named `AudioBus`
class only after the naming choice is settled once. It owns authored routing,
not device nodes. Planned fields are:

- stable bus name;
- parent bus reference or name;
- volume;
- mute and solo state for Studio audition;
- voice limit and priority bias;
- ordered effect-chain asset references;
- pre-fader and post-fader send definitions.

Routing must form a directed acyclic graph. A bus cycle is refused at property
commit or graph compile time. Missing buses route to the world master and emit
one diagnostic.

Every world receives a world master bus. World masters feed the client master.
This preserves the current rule that one world's volume cannot overwrite
another world's level on a shared device.

Solo is Studio and client-local monitoring state unless a game explicitly
authors it. It should not become a replicated gameplay fact by accident.

### Sends and returns

Sends are explicit graph edges with gain and pre-fader or post-fader placement.
A return is an ordinary bus. Shared reverb is therefore one effect chain fed by
many sends, not one reverb allocation per source.

Feedback remains refused. Delay and echo nodes hold internal history but do not
make a graph cycle. Supporting feedback later requires a compiler proof that
every cycle crosses a positive sample delay, plus a gain policy. It is not part
of the first production graph.

### Snapshots

An `AudioSnapshot` asset contains sparse overrides for bus gains, mutes, send
levels, and effect parameters. Applying a snapshot posts one prepared,
sample-deadline transaction with a ramp duration and curve.

Snapshots may blend by stable name and weight. The compiler resolves targets
and parameter ids before activation. A missing target is a compile diagnostic,
not a string lookup on the callback.

Typical uses include pause menus, underwater hearing, low-health filtering,
cinematics, and zone transitions. Snapshot choice may follow gameplay state,
but completion of its audio ramp never drives gameplay.

## Effects and DSP

### First processor set

The first bounded processor set is:

- gain and equal-power pan;
- low-pass and high-pass biquad filters;
- peaking and shelving equalization;
- compressor with side-chain input;
- hard or soft limiter at an explicit bus;
- bounded delay and echo;
- algorithmic reverb;
- waveshaper distortion;
- peak and RMS meter;
- spectrum analysis tap at a reduced update rate.

Each processor declares:

- audio and control ports;
- parameter types, units, defaults, and safe ranges;
- persistent state bytes per channel;
- scratch bytes per block;
- fixed or bounded latency;
- whether it supports in-place processing;
- bypass behavior;
- a worst-case work estimate used by the compiler.

Parameters that can click are smoothed in the processor or delivered as sample
ramps. Control-rate automation is converted into bounded ramps before reaching
the node.

### Latency

Every node reports fixed latency in samples after preparation. The compiler
adds delay compensation at parallel merge points when a graph needs phase
alignment. The total graph latency is exposed to Studio and the device adapter.

Variable, unbounded lookahead is refused. Changing a latency-affecting setting
requires a newly prepared graph and a safe swap.

### Analysis

Meters and analysis taps write compact summaries to a bounded callback-to-owner
ring. They never publish full sample blocks by default. The consumer polls at a
fixed low rate such as 20 or 30 Hz.

An offline tool may request full analysis because it is not on the device
deadline. Live scripts receive bounded peak, RMS, and selected band values with
explicit permission and rate limits. They do not receive a callback per sample
or per device block.

### Effect extension policy

Arbitrary native plugins and script-authored DSP are not accepted in the first
system. One unsafe processor can miss the device deadline or execute foreign
code inside the client. New built-in processors use the same descriptor,
offline parity suite, state bound, and timing benchmark as existing ones.

Shader-style user DSP may be reconsidered only with a validated instruction
set, bounded memory, bounded loops, denormal handling, deterministic offline
semantics, and an isolation story. A text editor alone is not that system.

## Graph and DAW authoring

### Audio graph document

`AudioGraphDocument` is versioned plain data. It stores:

- stable graph and node names;
- closed node kinds;
- typed port connections;
- parameter constants;
- exposed parameters with stable names;
- bus, sample, impulse-response, and subgraph asset references;
- automation lanes and markers;
- layout metadata ignored by runtime compilation;
- schema version and content signature.

Runtime node ids are assigned by compilation and are never saved. Unknown node
kinds are preserved by the editor when possible but refused by compilation.

Subgraphs have typed exposed ports and parameters. Recursion is refused. The
compiler flattens or statically calls bounded subgraphs and reports the complete
expanded cost.

### Typed ports

The initial port domains are:

- interleaved audio at the graph format;
- scalar control values;
- sample-deadline events;
- side-chain audio, distinct from ordinary summed audio where required.

Implicit audio-to-control or control-to-audio conversion is refused. Explicit
envelope, constant, and modulation nodes make the conversion visible.

### Compilation

Compilation runs off the device thread and produces an immutable
`PreparedAudioGraph` containing:

- validated topological order;
- compact node and edge tables;
- resolved parameter ids and asset handles;
- processor state layout;
- scratch and delay-buffer layout;
- channel and format conversions;
- latency compensation;
- automation tables;
- command capacity estimate;
- total memory and work budgets;
- source diagnostics mapped back to authoring node names.

Compilation refuses cycles, missing required ports, incompatible channel
layouts, unresolved required assets, unbounded delay, excessive state, too many
nodes, excessive automation density, and a projected cost above the selected
runtime budget.

The prepared graph owns all memory the callback may touch. Activation is a
bounded pointer or index swap at a block boundary. The old graph enters a
retire queue and is destroyed on an owner thread after the callback can no
longer reach it.

### DAW timeline

The graph editor includes a small timeline for preview and authored audio
sequences. It supports clips, regions, loop ranges, markers, and automation
lanes against integer sample positions or rational musical time.

Tempo maps use rational beat positions and explicit tempo events. Conversion
to samples occurs during compilation with one declared rounding rule. Runtime
does not accumulate floating-point beat deltas.

This timeline schedules audio presentation only. It is not a gameplay
sequencer, quest system, or cinematic authority. A cinematic system may submit
audio events from its own simulation timeline.

## Sample clocks, scheduling, and automation

### Clock domains

The system has three distinct clocks:

| Clock | Owner | Use |
|---|---|---|
| simulation tick | world authority | gameplay and replicated event order |
| presentation clock | client | interpolation, source and listener velocity, local policy |
| sample clock | mixer device | exact DSP and playback deadlines |

`AudioTimelineBridge` maintains a client-local affine mapping from presentation
time to sample position using the device's rendered count and measured queue
latency. Simulation events enter with a tick and sequence. The client maps them
to a sample deadline once, with a configurable safety lead.

The bridge absorbs slow device drift without moving already posted events. A
device reset establishes a new epoch. It does not rewrite gameplay time.

### Commands

Command kinds fall into three policies:

- coalescable values retry from the latest requested state;
- repairable graph transactions reserve queue capacity before posting;
- terminal teardown remains pending until accepted.

Graph swaps, snapshots, and multi-parameter changes are transactions. A partial
transaction is never made audible. The queue either accepts one prepared
transaction reference or the owner retries later.

Every queue reports pending count, high-water mark, refused commands, late
commands, maximum lateness, and repair backlog.

### Automation

Automation lanes compile into bounded segments with integer sample boundaries.
Supported first curves are step, linear, and a bounded exponential form.
Bezier authoring is flattened to an error tolerance and point ceiling before
runtime.

The callback evaluates only segments intersecting its current block. Dense or
pathological curves are refused or simplified during compilation with a visible
diagnostic.

Seeks define whether markers and automation between old and new positions are
skipped, fired, or reconstructed. The default for audio graph markers is skip.
Gameplay must not depend on these markers.

## Streaming, decode, and resampling

### Two residency modes

Short effects decode into immutable `SampleBuffer` objects. Long music,
ambience, dialogue, and large impulse responses use `AudioStream`.

Import metadata selects a default mode from decoded size and author override.
The threshold is configurable for a build profile, not silently selected by
device memory at playback time.

### Stream pipeline

A streamed voice has:

- verified encoded content or verified content chunks;
- one cancellable decode job owned outside the callback;
- a bounded decoded-frame ring;
- low and target watermarks;
- format and loop metadata;
- source and decode revisions;
- underrun, seek, decode, and delivery counters.

The callback reads available frames and emits silence on underrun. It never
waits for decode or delivery. An underrun is counted once per run and shown in
Studio. Recovery ramps in over a short period to avoid a click.

Seeking increments a revision, cancels stale delivery and decode work, resets
the ring on its owner side, and primes from a codec seek point. Results from an
older revision are discarded before publication.

Looping uses declared loop points when available. A compressed stream prepares
enough overlap or decoder state to make the loop seam exact. If a codec cannot
support a seamless loop, the importer reports it instead of claiming one.

### Delivery and CDN

Audio assets remain content addressed. The manifest gains optional audio
metadata or a sidecar asset containing:

- codec and channel layout;
- sample rate and duration;
- decoded frame bound;
- streaming hint;
- seek table or chunk map;
- loop points;
- cue markers;
- integrated loudness and true peak when calculated offline.

Metadata is signed with the manifest or verified by its own content address.
Runtime still sniffs the actual bytes before choosing a decoder.

Large streams should be chunked at codec-safe or independently decodable
boundaries during publication. Delivery can then request a bounded window and
cancel future chunks when a voice stops. Arbitrary HTTP range behavior is not a
substitute for a verified chunk map.

### Codecs

WAV and MP3 remain supported. Ogg Vorbis and FLAC require a recorded license,
vendored version, fuzz corpus, output ceiling, and parity fixtures before their
extensions become playable rather than merely classifiable.

Every decoder must bound:

- input bytes;
- output frames and samples;
- channel count and sample rate;
- metadata and tag size;
- seek-table entries;
- loop and cue counts;
- decode work per pump;
- total error reports.

Unexpected midstream format changes are refused unless the streaming adapter
explicitly supports a prepared format transition.

### Resampling

Decoded content is converted to graph format off the callback. Replace linear
interpolation with a measured band-limited resampler for import and stream
workers. The selected quality profiles must declare filter length, latency,
memory, and worst-case work.

Variable playback and Doppler need a real-time resampler whose state is
preallocated per audible voice. Its ratio changes smoothly and stays within a
declared bound. Offline rendering uses the same kernel and quality setting when
parity is requested.

## Voice limits and virtualization

### Limits

The client owns explicit limits for:

- total logical voices;
- real mixed voices;
- streamed voices;
- voices per world;
- voices per bus;
- one-shot voices;
- effect and analysis cost.

A logical voice may exist without being mixed. Limits are chosen from a device
profile and may be overridden downward by a game. Unbounded source creation is
never allowed to grow graph work or memory without limit.

### Priority

The client computes a stable score from:

- authored priority;
- bus bias;
- expected post-attenuation loudness;
- listener distance;
- occlusion;
- whether the voice is dialogue, UI, or music;
- whether it is already real;
- age and starvation prevention.

The policy uses hysteresis so nearby scores do not swap every frame. Ties use a
stable source name and entity order within one world.

UI, selected dialogue, and critical accessibility sounds may reserve slots.
These labels are audio policy only and do not grant gameplay authority.

### Virtual voices

A virtual voice advances a lightweight sample cursor and loop state without
running its graph branch. It resumes at the correct phase with a ramp. A
streamed virtual voice may stop filling its decode ring and seek forward before
resuming.

Virtualization does not emit live analysis or audible completion. A local
`Ended` presentation signal may fire from the logical cursor, but it is not
replicated and must not drive authoritative game state.

Voices beyond the logical limit are refused or replace a lower-priority
one-shot according to explicit policy. The action and reason are counted.

## Assets and publication

Audio import should produce or validate:

- stable original content bytes;
- trusted decoded bounds;
- optional normalized preview waveform at several resolutions;
- duration, channels, rate, codec, loop points, and cues;
- optional loudness and peak analysis;
- stream chunk and seek metadata;
- an import report with warnings and errors.

The preview waveform is derived content addressed by the source root and import
settings. Studio does not decode an hour-long source just to draw a thumbnail.

Audio graph, environment, snapshot, and impulse-response assets use distinct
stable kinds or typed metadata. They must not masquerade as ordinary sample
audio just to reuse a picker.

Publication is cancellable and reports stages such as inspect, decode probe,
analyze, build seek map, chunk, verify, and publish. A malformed asset fails
without leaving a partial manifest entry.

Builds may transcode to platform profiles only when the output codec and
settings are part of the artifact key. The original published source remains
traceable for re-import.

## Script API

### Principles

Luau and JavaScript receive one feature set with language-appropriate syntax.
The service surface validates values at the same boundary and writes the same
scene facts.

No script API exposes:

- a device pointer or callback;
- a runtime node id;
- mutable sample memory owned by a live voice;
- an unbounded PCM callback;
- the sample clock as an authoritative time source;
- a blocking load or decode operation.

### `Sound`

Planned class-specific methods and signals are:

- `Play`, `Pause`, `Resume`, and `Stop`;
- `Seek(seconds)` with a bounded finite value;
- `Loaded` and `Ended` presentation signals;
- a failure or diagnostic signal with a small stable reason enum;
- computed `IsLoaded`, `PlaybackLoudness`, and local `TimePosition`;
- saved properties listed in the source model.

Methods update intent and revisions. They do not call the mixer directly. A
server call replicates the resulting intent. A client-local instance stays
local through the existing replication rules.

`PlaybackLoudness` is a low-rate meter value. It may differ by client because
occlusion, device policy, and virtualization differ.

### `SoundService`

Planned additions are:

- enumerate and find authored buses by stable name;
- apply or blend a named snapshot with a duration;
- play a bounded local one-shot;
- select listener mode and object through the existing state seam;
- request client-local output and mixer diagnostics;
- open Studio editors only through Studio capability checks;
- expose distance scale and Doppler scale as world presentation settings.

Device selection and master hardware volume belong to a client settings
service, not replicated `SoundService` state.

### Graph and automation

Scripts may instantiate a published graph, set exposed parameters by stable
name, and schedule a bounded parameter change relative to simulation time.
They may not add arbitrary nodes to a live callback graph one operation at a
time.

Procedural graph construction, when added, builds an editable document or
buffer away from the device. `CompileAsync` validates and prepares it, then a
single activation request swaps it in. This follows the same build then commit
shape as editable meshes and animation buffers.

## Save and replication

### Saved facts

Save:

- `Sound` asset, routing, volume, playback, rolloff, source shape, and policy;
- bus hierarchy and effect-chain references;
- zone geometry ownership, profile, blends, and priority;
- world audio settings;
- graph, snapshot, environment, and automation asset references;
- a playback revision plus authoritative start tick when phase continuity is
  required.

Do not save:

- runtime node ids;
- decoded samples or stream rings;
- device format, device name, or output latency;
- listener camera pose or presentation velocity;
- occlusion rays, portal paths, filter history, reverb tails, or meters;
- voice-priority and virtualization decisions;
- prepared graph pointers or DSP scratch.

### Replicated playback

A replicated playback record needs enough information to resolve late arrival:

- stable sound or graph asset name;
- playing, paused, or stopped state;
- start simulation tick and sub-tick fraction when available;
- seek origin;
- loop region;
- playback speed;
- monotonic playback revision;
- late-arrival policy.

The client maps this record to its sample epoch. A looping ambience catches up
to phase. A one-shot may restart or be discarded according to policy. Asset
delivery time does not silently redefine the authored start time.

Rapid updates collapse by revision before mixer commands are posted. A stopped
revision cannot be overtaken by a late decode result from an older play.

### Local presentation

Listener choice, device settings, output layout, voice limits, meters, analysis,
occlusion quality, and Studio solo state remain client-local. Network packets
never carry PCM output.

Audio events that must affect gameplay are ordinary replicated gameplay events
that may also trigger sound. They are not inferred from `Ended`, a cue marker,
or a meter threshold.

## Studio authoring and diagnostics

### Audio mixer dock

Add a `View > Audio Mixer` dock with an information-dense bus tree. It shows:

- bus gain, mute, solo, and routing;
- peak and RMS meters;
- clipping and limiter gain reduction;
- real and virtual voice counts;
- stream underruns;
- effect cost and latency;
- command queue pressure and late commands;
- world and client master paths.

Meter updates are polled at a bounded rate and repainted only when values or
visibility change. A hidden or occluded dock consumes summaries without
continuously repainting.

### Graph editor

The graph editor provides:

- searchable typed nodes;
- typed ports and wire validation;
- effect parameter inspectors with units;
- subgraph navigation;
- timeline, markers, loop range, and automation lanes;
- compile errors attached to nodes and ports;
- prepared memory, latency, and work estimates;
- live audition through a disposable preview graph;
- A and B snapshot comparison;
- save, save as, reset preview, and offline render actions.

Closing the editor retires preview graphs. Preview playback does not keep a
device voice or timer alive after the panel is hidden.

### Waveform and asset inspector

The audio asset view shows the derived waveform, duration, codec, rate,
channels, decoded size bound, loudness, true peak, loop points, cues, stream
chunks, and import warnings. Authors can audition a selection and set loop
points without editing source bytes in place.

Waveforms use cached lower-resolution levels and draw only the visible range.
Long files are never expanded into one UI point per sample.

### Spatial and zone tools

The viewport can display:

- source point or volume;
- min and max rolloff bounds;
- directivity lobe;
- listener pose;
- active zone weights;
- occlusion rays selected for debugging;
- chosen portal propagation path;
- estimated direct and wet gain.

These overlays are demand-driven debug views. They do not run propagation when
the view is closed.

### Offline render and export

Studio can render a graph or selected timeline to WAV without an output device.
The request chooses sample rate, channel layout, duration or loop count,
quality profile, and optional stems.

Offline rendering reports progress and supports cancellation. It writes to a
temporary output and atomically publishes the completed file. A cancelled or
failed render does not replace an existing export.

## Device lifecycle

### Opening and silence

Startup requests a preferred device and format. Failure records one diagnostic
and uses a null backend. Worlds, scripts, and content continue to run.

The device adapter reports actual sample rate, channel layout, block size, and
estimated latency. A requested format is not treated as actual until SDL
confirms it.

### Device loss and hot change

Device callbacks report loss through a thread-safe flag or bounded event. A
client owner thread then:

1. stops new graph activation;
2. closes or detaches the lost stream outside the callback;
3. opens the selected replacement or a null backend;
4. prepares content and graph format for the new device;
5. establishes a new sample-clock epoch;
6. rebuilds voices from scene and client stage state;
7. fades output in and records any timing discontinuity.

The old live graph is not serialized out of the callback. Logical voices and
their playback records are sufficient to rebuild it.

A sample-rate change invalidates prepared resamplers, delay lengths, and
automation sample positions. Re-preparation occurs on workers. The client may
remain silent briefly rather than resample or allocate in the callback.

### Pause and suspend

Application suspension stops or idles the device according to platform policy.
On resume, music and ambience catch up or remain paused according to each
playback policy. One-shots that expired while suspended are discarded unless
explicitly marked to restart.

## Jobs and real-time safety

### Thread roles

| Thread or owner | Allowed work |
|---|---|
| simulation owner | change scene intent and revisions |
| client presentation owner | gather worlds, rank voices, query spatial data, post commands, poll meters |
| worker jobs | decode, resample, analyze, compile graphs, prepare effects, render offline chunks |
| device callback | drain bounded commands, run prepared DSP, write summaries, copy output |

The command ring retains one producer and one consumer. Several world stages
may post only when they are serialized on the same client owner thread. A
second producer must use an owner-side merge queue rather than write the ring.

Each streamed voice has an explicitly owned transfer path. A decode worker does
not also post topology commands. It publishes frames to its bounded stream ring
and the callback consumes them.

### Callback audit

Before effects or graph assets ship, audit and enforce that the callback path
does not:

- grow graph vectors or topological caches;
- create or destroy shared ownership that can free memory;
- resize scratch buffers;
- construct strings or diagnostics;
- touch the ECS, content cache, filesystem, network, or job scheduler;
- call a codec that allocates internally;
- produce denormal slow paths;
- take an operating-system or allocator lock.

Prepared graph swaps and owner-side retirement are the migration path for any
current topology mutation that cannot prove these properties.

### Asynchronous work

Audio decode and graph preparation may finish across simulation ticks because
they produce presentation artifacts. Their results carry source and request
revisions. A stale result is dropped at the client owner boundary.

No asynchronous result changes authoritative world state on arrival. A server
does not wait for a client's decode before advancing a sound or a game event.

### Shutdown

Every worker owns a stop token or bounded cancellation state. Shutdown order is
explicit:

1. stop new content and graph requests;
2. request worker cancellation;
3. detach the device callback;
4. retire prepared graphs and stream rings on owner threads;
5. join workers;
6. release codecs, content, and SDL state.

No worker captures an ECS pointer or client object past its owner's lifetime.

## Hostile media and resource limits

Every build profile declares ceilings for:

- encoded bytes per asset and per stream chunk;
- decoded frames, samples, channels, and sample rate;
- metadata, cover art, tags, cue points, and loop points;
- seek-table entries and stream chunks;
- graph nodes, wires, subgraph depth, and expanded nodes;
- automation lanes, points, and flattened segments;
- delay and reverb state bytes;
- impulse-response duration and partitions;
- logical, real, streamed, and one-shot voices;
- command transactions and analysis taps;
- worker decode time per pump and queued jobs;
- total decoded and prepared audio residency.

Bounds are checked before multiplication and allocation. Sample count arithmetic
uses checked wide integers. A malformed file or graph is refused as one asset;
it does not take down the client or mute unrelated buses.

Error strings are bounded and rate limited. Logs identify the stable asset name,
content root, codec or graph stage, and refusal class without printing hostile
metadata verbatim.

Fuzz WAV, MP3, future codecs, graph readers, metadata readers, seek maps, and
automation flattening. Seed corpora include truncated headers, integer wraps,
format changes, oversized tags, bad loops, graph cycles, deep subgraphs, and
dense curves.

## Offline render parity

`NullDevice` remains the headless live test backend. Add an `OfflineRenderer`
that drives the same `PreparedAudioGraph`, processors, automation, resamplers,
and mixer segmentation without SDL.

Parity means:

- the same graph document and assets compile to the same prepared plan;
- the same command sequence at the same sample deadlines produces the same
  samples within a declared floating-point tolerance;
- live and offline processor state transitions match across block sizes;
- loop seams, seeks, ramps, and graph swaps occur on the same samples;
- meters are derived from the same unclipped or clipped stage as documented;
- offline high-quality mode is a named alternate profile, not silently used to
  bless live output that differs.

Tests render golden signals generated from equations or independent reference
tools, not only from a previous engine run. Codec parity fixtures name the
external decoder and version used for comparison.

Block-size invariance is required where DSP semantics allow it. Rendering 512
frames in one block and in irregular smaller blocks must match around command
deadlines and automation boundaries.

## Diagnostics and profiling

### Counters

Expose bounded counters and gauges for:

- callback duration, budget, and missed deadlines;
- blocks, frames, segments, and graph swaps;
- command pending, high-water, refused, repaired, late, and maximum lateness;
- active, real, virtual, refused, and stolen voices;
- decoded and prepared CPU bytes;
- stream fill, underruns, seeks, stale results, and cancelled requests;
- graph nodes, scratch bytes, delay bytes, and reported latency;
- per-bus peak, RMS, clipping, and limiter reduction;
- occlusion queries, cache hits, portal paths, and stale propagation results;
- device changes, null-backend time, and reopen failures.

Metrics are observed, not used to steer authoritative behavior. Voice policy
uses current local state and declared budgets, not a profiler readback.

### Profiling

Profile owner-side sync, spatial propagation, decode, resampling, graph compile,
and offline render with normal engine scopes. The device callback uses Tracy or
audio-safe fixed records only if the instrumentation has proven it does not
allocate or block.

Report callback work per block, per node kind, and per active voice without a
dynamic string per node. Use bounded node-kind ids and aggregate values.

Every performance claim names:

- release preset;
- platform and audio backend;
- sample rate, channels, and block size;
- graph shape and effect set;
- real and streamed voice counts;
- scene and portal-query settings;
- p50, p95, p99, and maximum callback time;
- underruns or missed buffers;
- memory and queue high-water marks.

The primary gate is that p99 callback work remains below an agreed fraction of
the device block duration with zero missed buffers during the soak. The exact
fraction is chosen from measurements and written beside the benchmark.

## Migration plan

Migration keeps one audible path at each step.

### Preserve current behavior

Pin current WAV and MP3 decoding, distance gain, panning, command deadlines,
looping, `Sound` defaults, `SoundService` behavior, and null-device output in
tests. Record the current mixer benchmark before changing node storage.

### Prepare callback-owned state

Introduce processor descriptors, prepared state layouts, fixed-capacity command
transactions, callback summaries, and owner-side retirement. Move topology and
scratch preparation out of live rendering while keeping the existing node
kinds and `SoundStage` API.

Delete the old live topology-mutation path once the prepared path has parity.
Do not leave two graph executors.

### Add buses before effects

Create world and client master buses, route every existing sound through its
world master, and prove that multi-world volume remains isolated. Add authored
bus routing and snapshots next. Effects then have one clear place to live.

### Add spatial features incrementally

Post full listener orientation, then source orientation and velocity, then
Doppler, then one-ray occlusion, then material filters, zones, and bounded
portal propagation. Each step retains an off switch and a parity case for the
previous simple path until it is proven, then removes redundant branches.

### Add streaming without changing short sounds

Introduce a streamed player node and worker ring. Keep short assets on immutable
whole buffers. Make import metadata choose the path. Add seeking, looping, CDN
chunks, and additional codecs only after underrun and cancellation behavior is
pinned.

### Add graph assets and Studio tools

Make the graph document, compiler, and offline renderer headless first. Then add
the Studio editor as a view over those APIs. Runtime never depends on the
editor's node objects.

### Expand scripting last

Expose buses, snapshots, local one-shots, graph parameters, methods, signals,
and analysis only after their engine and client adapters have focused tests.
Regenerate both language bindings in the same change.

## Delivery phases and gates

### Phase 0: invariants and measurement

- pin existing output and script behavior;
- audit the callback for allocation, locks, ownership destruction, and logging;
- add callback timing, queue high-water, late-command, and voice counters;
- add irregular-block offline fixtures;
- capture release mixer baselines.

Gate: current features have no callback-unsafe operation, or every remaining
one has a named migration test and is removed before Phase 1 completes.

### Phase 1: prepared mixer core

- processor descriptors and fixed parameter ids;
- prepared graph memory and scratch layout;
- atomic block-boundary graph swap;
- owner-side graph retirement;
- bounded command transactions;
- callback summary ring;
- offline renderer using the same DSP path.

Gate: existing mixer parity passes, callback allocation count is zero, and
live versus offline output matches at command boundaries.

### Phase 2: buses, snapshots, and core effects

- world and client masters;
- authored buses, sends, and returns;
- gain, filters, EQ, compressor, limiter, delay, meters, and algorithmic reverb;
- sample ramps and snapshot blending;
- latency reporting and compensation;
- mixer dock diagnostics.

Gate: routing cycles and over-budget graphs are refused before activation;
snapshot transitions are click-free; multi-world isolation is pinned.

### Phase 3: complete spatial audio

- listener orientation;
- source shape and directivity;
- configurable attenuation;
- Doppler with discontinuity reset;
- bounded occlusion queries and material filtering;
- zones and environment profiles;
- bounded portal paths;
- viewport diagnostics.

Gate: no spatial query reaches the callback, budgets remain bounded under a
large source set, and portal transitions do not double-mix or pitch-spike.

### Phase 4: streaming and content

- streaming player and worker rings;
- import metadata, waveforms, seek maps, chunks, and loop points;
- cancellable CDN window delivery;
- high-quality worker resampling;
- codec additions after license and fuzz gates;
- asset inspector and stream diagnostics.

Gate: hour-long playback has bounded memory, seeks discard stale work, stop
cancels future delivery, and stress runs report zero callback waits.

### Phase 5: voice policy and device lifecycle

- logical and real voice limits;
- stable prioritization and virtualization;
- reserved critical categories;
- hot device change and null fallback;
- sample epoch reset and voice rebuild;
- suspend and resume policy.

Gate: an adversarial source burst respects all budgets, a lost device cannot
stop the game, and voice recovery has no leaked graph or worker state.

### Phase 6: graph and DAW authoring

- versioned graph document and compiler;
- subgraphs, typed ports, exposed parameters, and automation;
- timeline, clips, markers, and tempo map;
- graph editor, live audition, compile diagnostics, and offline export;
- graph and snapshot script activation.

Gate: saved documents round-trip, hostile graphs refuse safely, hidden previews
consume no live work, and exported audio passes offline parity.

### Phase 7: scripting and compatibility completion

- class-specific sound methods and signals;
- local one-shot service path;
- buses, snapshots, graph parameters, and bounded analysis;
- Luau and JavaScript parity;
- compatibility documentation and migration helpers.

Gate: generated bindings, runtime methods, save behavior, and client or server
availability agree. No listed API exists as a no-op.

## Focused test plan

### Sample and codec tests

- invalid formats, partial frames, conversion bounds, silence, mixing, and peak;
- RIFF chunk truncation, padding, overflow, unknown chunks, and format refusal;
- MP3 tags, output limits, truncation, changing format, and reference decode;
- each future codec against independent fixtures and malformed fuzz corpora;
- loop points and seek positions at start, end, and invalid boundaries;
- worker resampler frequency response, alias rejection, ratio ramps, and bounds.

### Graph and DSP tests

- unknown nodes, ports, assets, parameters, and schema versions;
- cycle, recursion, expansion, scratch, delay, automation, and work limits;
- stable compile order and content signature;
- graph swap at the exact block boundary;
- old graph retirement after callback release;
- processor impulse, step, silence, clipping, NaN, and denormal behavior;
- latency compensation at parallel merges;
- bypass and parameter ramps without clicks;
- meter stage before or after clipping exactly as documented.

### Scheduling tests

- commands at block start, middle, end, past, and beyond the current block;
- stable order for commands on one sample;
- transaction all-or-nothing behavior;
- queue refusal and repair for every policy class;
- automation steps and ramps on exact samples;
- seek and loop behavior across irregular block sizes;
- simulation-to-sample mapping, drift correction, and epoch reset;
- proof that scripts cannot read a clock used for gameplay branching.

### Spatial tests

- equal-power pan and each attenuation mode;
- inside and outside volumetric source bounds;
- source and listener orientation;
- Doppler approach, retreat, clamp, teleport, and portal reset;
- occlusion cache invalidation and stale-result fade;
- material transmission and filter targets;
- overlapping zone priority and blending;
- disabled, closed, nested, and cross-world portal paths;
- no duplicate direct and portal voice;
- fixed query and hop budgets under hostile geometry.

### Streaming tests

- prime, consume, underrun, recover, stop, seek, and loop;
- stale decode and delivery results after revision change;
- content cancellation and bounded read-ahead;
- virtual stream suspend and resume at phase;
- truncated or corrupt later chunks;
- device-format change during preparation;
- hour-long synthetic stream with flat memory;
- multiple streams under worker and bandwidth limits.

### Scene and replication tests

- current `Sound` defaults and existing save round-trip;
- new properties, padding, enum names, and migration defaults;
- non-positional and parented positional rules;
- bus loss and fallback;
- playback revision ordering and late asset policy;
- looping phase catch-up on late join;
- no device, meter, occlusion, or decoded data in replication;
- one world master cannot alter another world's mix.

### Script tests

- Luau and JavaScript parity for every method, property, signal, and error;
- service availability on client and headless server;
- finite and bounded argument validation;
- play, pause, resume, stop, seek, and revision races;
- local one-shot limits and retirement;
- snapshot and exposed graph parameter lookup by stable name;
- analysis permissions and rate limits;
- no callback invocation into either VM.

### Studio tests

- audio asset picker and import diagnostics;
- graph document round-trip independent of widget layout;
- compile errors select the correct node and port;
- hidden mixer and graph docks stop repaint and preview work;
- waveform level selection for long assets;
- offline render progress, cancellation, and atomic output;
- device loss while auditioning;
- meters, clipping, queue pressure, and underrun displays from fixed summaries.

### Soak and performance tests

- dense short one-shots;
- moving spatial voices with occlusion;
- many virtual voices and a stable real set;
- streamed music plus effects and reverb;
- repeated graph and device swaps;
- portal-rich multi-world audio;
- malformed asset requests under normal playback;
- shutdown during decode, graph compile, and device callback activity.

Soaks fail on missed callbacks, callback allocation, unbounded live bytes,
growing retire queues, unrecovered stream underruns, or command refusal that
continues after load subsides.

## Non-goals for the first production system

- voice capture, chat codecs, echo cancellation, and jitter buffers;
- surround, Ambisonics, or binaural head-related transfer functions;
- full wave, beam, or path-traced acoustic simulation;
- arbitrary third-party native audio plugins;
- arbitrary script code inside DSP;
- feedback graphs;
- a universal graph virtual machine shared with render, physics, or animation;
- music composition, notation, or a full digital audio workstation;
- authoritative gameplay driven by audio playback, markers, meters, or device
  completion;
- replicated mixed PCM;
- cloth, facial, animation, or cinematic sequencing owned by audio;
- automatic loudness normalization that changes authored gain without an
  explicit import or bus setting.

## Completion definition

The audio expansion is complete when:

- current `Sound` and `SoundService` behavior migrates without a second mixer
  path;
- shared scene code and headless servers remain independent of the device;
- the callback has no allocations, locks, blocking, logging, script calls, file
  access, network access, or unsafe destruction;
- starts, stops, automation, seeks, loops, snapshots, and graph swaps land on
  specified samples;
- buses, effects, zones, Doppler, occlusion, portals, and streaming have bounded
  costs and focused tests;
- voice limits and virtualization keep hostile source counts bounded;
- malformed media and graphs refuse before unsafe allocation;
- a missing or changed device leaves the game running and audio recoverable;
- save and replication carry stable intent, while local DSP state stays local;
- Luau and JavaScript expose the same working surface;
- Studio can author, inspect, audition, diagnose, and export without hidden
  continuous work;
- live and offline rendering meet the declared parity tolerance;
- release benchmarks meet the callback deadline gate with zero missed buffers;
- architecture, formatting, documentation, sanitizer, fuzz, unit, integration,
  offline parity, soak, and profiling gates pass.
