#pragma once

// How a tick tells the mixer what to do, and **when** to do it.
//
// This file exists because of one requirement, marked required rather than
// desirable:
//
//   *Sample-accurate scheduling. A game ticks at frame rate and audio runs at
//   sample rate. Events scheduled on tick boundaries jitter audibly.*
//
// and the constraint it puts on this module by name: **the tick queues an event
// and the audio graph schedules it against the sample clock. This is the one
// place where "close enough to the frame" is wrong, it is audible immediately
// to anyone who notices it, and it cannot be fixed from above.**
//
// So a command carries a *sample deadline*, not a "do this now". At 60 Hz and
// 48 kHz a frame is 800 samples; a footstep applied at the start of whichever
// block happens to come next lands up to a block early or late, and a run of
// them is audibly uneven. The mixer splits its block at each deadline and
// applies the command exactly there.
//
// **The queue is single-producer, single-consumer and lock-free**, because the
// consumer is a device callback with a hard deadline and a missed buffer is
// audible. A mutex would usually be fine and would occasionally not be: the
// producer is a tick thread that can be preempted while holding it, and the
// consumer cannot wait. One producer and one consumer is what makes the
// lock-free version simple enough to be obviously right - a ring, two atomic
// indices, and a slot only ever touched by one side at a time.
//
// **Node ids are allocated by the producer**, which is what lets creating a
// node be a fire-and-forget command instead of a round trip. The tick asks for
// an id, posts `AddNode` with it, and can wire it up in the same tick - before
// the mixer has seen any of it.
//
// @tier L12 · client

#include <engine/audio/Graph.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace engine::audio {

	// What a command does.
	//
	// @since v0.9
	enum class CommandKind : uint8_t {
		// Nothing. What an empty slot holds.
		None,

		// Create a node with the id the producer already allocated.
		AddNode,

		// Destroy a node and its wires.
		RemoveNode,

		// Wire `Target`'s output into `Second`'s input.
		Connect,

		// Remove that wire.
		Disconnect,

		// Give a player something to play, and rewind it.
		SetSound,

		// Start a player from its current cursor.
		//
		// **This is the one whose timing is audible**, and the reason the whole
		// file exists.
		Play,

		// Stop a player, leaving its cursor where it is.
		Stop,

		// Rewind a player to the start without changing whether it is playing.
		Rewind,

		// Set a node's linear gain.
		SetGain,

		// Set a node's stereo placement.
		SetPan,

		// Mute or unmute a node.
		SetMuted,

		// Set whether a player loops.
		SetLooping,

		// Move an emitter, and set its falloff.
		SetPlacement,

		// Move the listener.
		SetListener,
	};

	// One instruction, and the sample it takes effect on.
	//
	// One struct with a kind tag rather than a variant, for `Node`'s reason:
	// this is copied into a ring slot on a thread with a deadline, and a
	// trivially copyable fixed-size record is what makes that cheap and
	// obvious. The `SoundRef` is the one field that is not trivial, and it is
	// the one a `SetSound` genuinely needs.
	//
	// @since v0.9
	struct Command {
		// What to do.
		CommandKind Kind = CommandKind::None;

		// The sample this takes effect on, on the mixer's absolute clock.
		//
		// **A deadline in the past is applied at the start of the next block
		// rather than dropped.** A tick that ran late still meant its command
		// to happen, and dropping it would turn a frame hitch into a missing
		// sound - which is far worse than one that is a few samples late.
		uint64_t AtSample = 0;

		// Which node.
		NodeId Target;

		// The second node, for `Connect` and `Disconnect`.
		NodeId Second;

		// What kind of node, for `AddNode`.
		NodeKind Node = NodeKind::Bus;

		// The value, for `SetGain` and `SetPan`.
		float Value = 0.0f;

		// The flag, for `SetMuted` and `SetLooping`.
		bool Flag = false;

		// Where, for `SetPlacement`.
		EmitterPlacement Placement;

		// Where, for `SetListener`.
		ListenerPose Pose;

		// What to play, for `SetSound`.
		SoundRef Sound;
	};

	// A bounded ring between one producer and one consumer.
	//
	// **One producer and one consumer, and that is not a detail.** Two ticks
	// posting at once, or two device callbacks draining at once, would both
	// corrupt the ring silently. The engine's arrangement gives exactly one of
	// each: a world's tick posts, and the device thread drains.
	//
	// @threadsafe For one producer and one consumer. Not otherwise.
	// @since v0.9
	class CommandQueue {
	  public:
		// How many commands may be waiting.
		//
		// A bound rather than a growing buffer, because growing means
		// allocating, and the producer may be doing it while the consumer has
		// a deadline. A full queue **refuses** rather than blocking - see
		// `Post`.
		static constexpr size_t CAPACITY = 1024;

		CommandQueue();

		// Allocates a node id the producer may use immediately.
		//
		// What lets `AddNode` be fire-and-forget rather than a round trip: the
		// tick names the node, posts its creation, and wires it up in the same
		// tick - all before the mixer has seen any of it.
		//
		// @return A fresh id, never `NONE` and never reissued.
		NodeId Allocate();

		// Posts a command.
		//
		// **A full queue drops the command and says so, rather than blocking.**
		// The producer is a tick and the consumer has a deadline: waiting would
		// stall the world to keep a sound, which is the wrong way round.
		//
		// ## What a producer owes a refusal, and it is not one policy
		//
		// "Check the return" is not an instruction until it says what to do
		// with a `false`, and the answer is different for three kinds of
		// command. Until v0.19 every one of `mono.client/src/Sounds.cpp`'s
		// fifteen call sites discarded it and recorded the state as landed,
		// which turns each of the three into a different permanent fault.
		//
		//  * **Coalescable** - `SetGain`, `SetPan`, `SetMuted`, `SetLooping`,
		//    `SetPlacement`, `SetListener`. Each states a value rather than an
		//    edge, so a dropped one is repaired by the next pass that notices
		//    the value still differs. The producer must therefore record what
		//    it *posted*, never what it meant to post: a producer that updates
		//    its last-sent value before the refusal has coalesced the command
		//    into nothing, and the node keeps the old level for ever.
		//  * **Repairable** - `AddNode`, `Connect`, `SetSound`, `Play`,
		//    `Rewind`. These build state and are not idempotent under a partial
		//    burst: half a voice is a player wired to nothing. A producer
		//    reserves with `Free()` before it starts, and treats a refusal as
		//    "not opened" so the next pass builds it again.
		//  * **Terminal** - `Stop`, `Disconnect`, `RemoveNode`. Nothing later
		//    reposts these, because the row that would have noticed is the one
		//    being torn down. A producer that drops one has leaked a node and
		//    left a voice sounding, so it must hold the teardown and retry it.
		//
		// The counter is what an operator looks at; `Dropped()` is the
		// difference between "audio is broken" and "the queue is too small for
		// what this scene does".
		//
		// @param command What to do and when.
		// @return Whether it was queued.
		bool Post(const Command &command);

		// How many more commands fit right now.
		//
		// **For reserving a burst, which is the only way to post one safely.**
		// A voice is five to eight commands that mean nothing apart, and
		// posting them one at a time and checking each leaves the producer
		// holding half a graph with no way to undo it - the undo is itself
		// commands, into the queue that just refused. Asking first turns that
		// into a decision made before anything was built.
		//
		// **A floor, not a reservation.** One producer is the whole contract of
		// this class, so nothing else can take the space; the consumer only
		// ever frees it. So the answer can grow between the ask and the post
		// and never shrink, which is the direction that makes it safe.
		//
		// @return Slots free for this producer, which is one less than the
		//         distance to `CAPACITY` because the ring always leaves one
		//         empty.
		size_t Free() const;

		// Takes every command that is waiting.
		//
		// Drains the whole ring rather than up to a deadline: the mixer needs
		// to know about commands *later* in the block as well as earlier ones,
		// so it can decide where to split. Filtering by time is the mixer's.
		//
		// @param[out] into Appended to, in the order posted.
		// @return How many were taken.
		size_t Drain(std::vector<Command> &into);

		// How many commands are waiting.
		size_t Pending() const;

		// How many were dropped because the queue was full, over its life.
		//
		// **Counted rather than silent.** A game that drops commands is a game
		// missing sounds, and the number is the difference between "audio is
		// broken" and "the queue is too small for what this scene does".
		uint64_t Dropped() const {
			return Missed.load(std::memory_order_relaxed);
		}

	  private:
		// Power of two, so the index wrap is a mask rather than a modulo.
		static constexpr size_t MASK = CAPACITY - 1;
		static_assert((CAPACITY & MASK) == 0, "CAPACITY must be a power of two.");

		std::vector<Command> Slots;

		// Written by the producer, read by both. Release/acquire around them is
		// what publishes a slot's contents: the producer fills a slot and
		// *then* releases the index, so a consumer that acquires the index sees
		// a complete command.
		std::atomic<size_t> Write{0};
		std::atomic<size_t> Read{0};

		std::atomic<uint64_t> Missed{0};
		std::atomic<uint32_t> NextNode{1};
	};
}
