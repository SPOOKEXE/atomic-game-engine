#pragma once

// The mixer's data model: nodes, wires, and the order they run in.
//
// `core-features.md` asks for "an audio pipeline (DAW-like), node system -
// inputs/outputs/processors". This is that, and the three words are the node
// kinds: a `Player` is an input, a `Fader`, `Emitter` and `Bus` are processors,
// and an `Output` is where it all lands.
//
// **Why this is not `engine::graph`.** `repo_layout.md` §9 plans one graph
// runtime with five consumers and lists audio among them, and that is right
// eventually. But §16 decision 12 states the losing condition and the ordering
// with it: *build `mono.engine/graph/` against render only, and do not claim it
// is general until the physics graph is the second user.* Today `graph` holds
// the description of a frame and none of the execution - no nodes, no compiler,
// no executor - so routing audio through it would mean building that runtime
// against a second consumer before it exists for the first. This graph is small
// and this file says plainly what it will be folded into.
//
// **A wire runs from a source's output to a target's input**, which is Roblox's
// arrangement and reads the right way round: you connect a sound *to* a fader,
// not a fader *from* a sound.
//
// **Cycles are refused at the wire rather than detected at the mix.** A
// feedback loop in an audio graph is not a subtle bug: it is either an infinite
// recursion on the device thread - which is a crash inside a callback with a
// hard deadline - or unbounded gain, which is the loudest possible failure. The
// check is a walk at connect time, and connecting is not on the audio path.
//
// **Everything here is plain data with no thread of its own.** The mixer owns
// one of these and mutates it only by applying commands, so a tick never writes
// what a device thread is reading. `Commands.hpp` is that hand-off.
//
// @tier L12 · client

#include <engine/audio/Sample.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace engine::audio {

	// Decoded audio, shared with everything currently playing it.
	//
	// A `shared_ptr` to something immutable, for `cdn::PreparedFrame`'s reason:
	// a sound may be dropped from a library while a voice is still walking
	// through it, and the voice must not have its samples freed underneath.
	using SoundRef = std::shared_ptr<const SampleBuffer>;

	// What a node does.
	//
	// A closed list. Adding a kind is adding a case to the mixer, and an open
	// set of node types would make "what does this graph do" unanswerable
	// without running it.
	//
	// @since v0.9
	enum class NodeKind : uint8_t {
		// **Input**. Walks a `SoundRef` and produces its samples.
		Player,

		// **Processor**. A gain, and the one place a level is set. Separate
		// from `Player` so that several players can share a fader - which is
		// what a bus fader *is*, and building it as a node rather than a
		// property is the difference between a mixer and a list of sounds.
		Fader,

		// **Processor**. Places its input in the world: distance attenuation
		// and stereo panning against the listener. `Spatial.hpp` does the
		// arithmetic.
		Emitter,

		// **Processor**. Sums everything wired into it. The node a submix is
		// made of.
		Bus,

		// **Output**. Where the device reads from. A graph has exactly one.
		Output,
	};

	// Returns a stable, human-readable name for a node kind.
	//
	// @param kind The kind to name.
	// @return A view valid for the lifetime of the process.
	const char *Describe(NodeKind kind);

	// A node in the graph.
	//
	// A number rather than a pointer: nodes live in a vector that grows, and a
	// pointer into it is invalid the moment somebody adds a sound.
	//
	// @since v0.9
	struct NodeId {
		// The value meaning "no node". Zero is never issued.
		static constexpr uint32_t NONE = 0;

		// The graph's monotonic counter.
		uint32_t Value = NONE;

		// Whether this names a node at all.
		constexpr bool IsValid() const {
			return Value != NONE;
		}

		// Whether two handles name the same node.
		constexpr bool operator==(const NodeId &other) const = default;
	};

	// Where a sound is, and how loudly it should be heard from there.
	//
	// @since v0.9
	struct EmitterPlacement {
		// Where it is, in world space.
		float X = 0.0f;
		float Y = 0.0f; // Its world-space Y.
		float Z = 0.0f; // Its world-space Z.

		// Inside this, it plays at full volume. Outside `FalloffEnd`, it is
		// silent; between them it attenuates.
		//
		// Two distances rather than one plus a curve constant, because two
		// distances are what a level designer can reason about and a rolloff
		// exponent is not.
		float FalloffStart = 5.0f;

		// Beyond this it is silent. See `FalloffStart` for why the pair is two
		// distances rather than a distance and a curve.
		float FalloffEnd = 60.0f;
	};

	// One node's parameters.
	//
	// One struct for every kind rather than a variant, and that is a deliberate
	// trade: a few unused floats per node against a visitor at every call site
	// in the mixer's inner loop. A graph is hundreds of nodes, not millions.
	//
	// @since v0.9
	struct Node {
		// What it does.
		NodeKind Kind = NodeKind::Bus;

		// Linear gain. Applied by `Fader`, and by `Player` as its own level.
		//
		// **Linear rather than decibels**, because the mixer multiplies. A
		// decibel is what a *user interface* shows, and converting there keeps
		// the conversion out of the audio path.
		float Gain = 1.0f;

		// Stereo placement, -1 hard left to +1 hard right. `Emitter` computes
		// this from the listener; a `Fader` can set it directly.
		float Pan = 0.0f;

		// Whether this node produces or passes anything at all.
		//
		// A muted node still runs its inputs - a muted sound still advances,
		// so unmuting does not resume something from where it was minutes ago.
		bool Muted = false;

		// `Player`: what it is playing. Null plays silence.
		SoundRef Sound{};

		// `Player`: where the playback cursor is, in source frames.
		//
		// **Fractional, because a sound's rate need not be the mixer's.** An
		// integer cursor would force every sound to be resampled at load, which
		// is the right default and not something to make impossible.
		double Cursor = 0.0;

		// `Player`: whether it is advancing.
		bool Playing = false;

		// `Player`: whether it returns to the start rather than stopping.
		bool Looping = false;

		// `Emitter`: where it is.
		EmitterPlacement Placement{};
	};

	// Where the listener is and which way it faces. One per graph.
	//
	// Named a *pose* rather than a `Listener` so the graph's accessor can be
	// called `Listener()` without an elaborated type specifier at every use -
	// a member function and a type of one name compiles and reads badly.
	//
	// @since v0.9
	struct ListenerPose {
		// Where it is, in world space.
		float X = 0.0f;
		float Y = 0.0f; // Its world-space Y.
		float Z = 0.0f; // Its world-space Z.

		// The direction it faces and the direction to its right, as unit
		// vectors. Panning needs the right vector; attenuation needs neither.
		//
		// Two vectors rather than a quaternion or a matrix, because these are
		// the two the arithmetic uses and deriving them per block from a
		// rotation would be work on the device thread.
		float ForwardX = 0.0f;
		float ForwardY = 0.0f;	// The facing vector's Y.
		float ForwardZ = -1.0f; // The facing vector's Z. Negative Z is forward.

		// The direction to the listener's right, as a unit vector. This is the
		// one panning reads.
		float RightX = 1.0f;
		float RightY = 0.0f; // The right vector's Y.
		float RightZ = 0.0f; // The right vector's Z.
	};

	// Nodes, the wires between them, and the order they run in.
	//
	// **Not thread-safe, and owned by the mixer.** A tick changes it by queuing
	// a command; the mixer applies commands and then mixes, both on the device
	// thread. That is what stops a tick writing a gain while a block is being
	// rendered against it.
	//
	// @since v0.9
	class AudioGraph {
	  public:
		// The most nodes one graph may hold.
		//
		// A bound rather than "as many as fit", because the topological walk is
		// O(nodes + wires) per block and an unbounded graph is an unbounded
		// amount of work on a thread with a hard deadline. Generous: a game
		// mixing a thousand simultaneous sources has a different problem.
		static constexpr size_t MAXIMUM_NODES = 1024;

		// The output node's id, reserved.
		//
		// **One id space, shared with `CommandQueue::Allocate`.** A graph mints
		// ids and so does the queue, and they name nodes in the same graph - so
		// the output has a fixed value and the queue starts above it. Without
		// that they collide on the very first allocation, and the symptom is a
		// player silently adopted as the output and then refused a wire to
		// itself: no crash, no error, no sound.
		static constexpr uint32_t OUTPUT_ID = 1;

		// The first id `CommandQueue::Allocate` may issue.
		static constexpr uint32_t FIRST_FREE_ID = OUTPUT_ID + 1;

		// Builds a graph holding only its output node.
		AudioGraph();

		// The output every chain eventually reaches.
		NodeId Output() const {
			return Sink;
		}

		// Adds a node.
		//
		// @param kind What it does. `Output` is refused - a graph has exactly
		//        one and it is made by the constructor, because two outputs is
		//        a graph with no answer to "what does the device play".
		// @return The node, or an invalid id when the kind is refused or the
		//         graph is full.
		NodeId Add(NodeKind kind);

		// Adds a node under an id somebody else allocated.
		//
		// **What makes creating a node fire-and-forget across the thread
		// boundary.** `CommandQueue::Allocate` hands the tick an id, the tick
		// posts `AddNode` with it and wires it up in the same tick, and the
		// mixer adopts it later - so there is no round trip and no moment where
		// the tick knows about a node the graph does not.
		//
		// There is one id space and the output's is reserved - see `OUTPUT_ID`.
		// Adopting an id a graph already issued is refused rather than silently
		// aliasing it.
		//
		// @param id The id to adopt. Refused if invalid or already in use.
		// @param kind What it does. `Output` is refused, as in `Add`.
		// @return Whether the node exists now.
		bool Adopt(NodeId id, NodeKind kind);

		// Removes a node and every wire touching it.
		//
		// Removing the output is refused. Wires are removed with the node
		// rather than left dangling: a wire to a node that is gone is a lookup
		// that fails once per block, for ever.
		//
		// @param id The node to remove.
		// @return Whether it was removed.
		bool Remove(NodeId id);

		// A node's parameters, or nullptr.
		//
		// @param id The node.
		// @return The node, or nullptr for a handle this graph did not issue.
		Node *Find(NodeId id);

		// A node's parameters, or nullptr.
		//
		// @param id The node.
		// @return The node, or nullptr.
		const Node *Find(NodeId id) const;

		// Wires a source's output into a target's input.
		//
		// **Refused when it would make a cycle**, and that check is the reason
		// this returns a bool rather than nothing. A feedback loop is either an
		// infinite recursion inside a callback with a hard deadline or
		// unbounded gain, and both are worse than a wire that did not connect.
		//
		// Connecting a pair that is already connected is a no-op that answers
		// true - a caller rebuilding a routing should not have to diff it.
		//
		// @param from The source.
		// @param to The target.
		// @return Whether the wire exists now. False for an unknown node, for
		//         a wire out of the output, or for a cycle.
		bool Connect(NodeId from, NodeId to);

		// Removes a wire.
		//
		// @param from The source.
		// @param to The target.
		// @return Whether a wire was removed.
		bool Disconnect(NodeId from, NodeId to);

		// Whether a wire exists.
		//
		// @param from The source.
		// @param to The target.
		// @return Whether they are wired.
		bool Connected(NodeId from, NodeId to) const;

		// What is wired into a node.
		//
		// **The span is stable until the topology changes**, because it points
		// into an adjacency list rebuilt alongside the order. A shared scratch
		// buffer would have been the obvious implementation and is wrong: the
		// mixer holds one node's inputs while asking about another, and the
		// second call would invalidate the first.
		//
		// @param id The target.
		// @return Its sources, in the order they were connected.
		std::span<const NodeId> InputsOf(NodeId id) const;

		// How many nodes there are.
		size_t Count() const {
			return Nodes.size();
		}

		// Every node, in an order where a source always comes before its
		// target.
		//
		// **Recomputed only when the topology changed**, because a mixer asks
		// for it every block and the answer only moves when somebody wires
		// something. Parameter changes - a gain, a cursor - do not invalidate
		// it, which is the common case by a wide margin.
		//
		// @return The order. Nodes that reach no output are still in it: a
		//         detached subgraph costs its own mixing and nothing else, and
		//         dropping it would make a half-built routing silently stop
		//         advancing its players.
		std::span<const NodeId> Order() const;

		// Where the listener is and which way it faces.
		const ListenerPose &Listener() const {
			return Ear;
		}

		// Where the listener is and which way it faces.
		ListenerPose &Listener() {
			return Ear;
		}

	  private:
		struct Wire {
			NodeId From;
			NodeId To;
		};

		size_t IndexOf(NodeId id) const;

		// Whether following wires forwards from `start` ever arrives at
		// `target`. A wire `from -> to` is a cycle exactly when
		// `CanReach(to, from)`.
		bool CanReach(NodeId start, NodeId target) const;

		void Rebuild() const;

		std::vector<NodeId> Ids;
		std::vector<Node> Nodes;
		std::vector<Wire> Wires;

		// Per node, what is wired into it. Derived from `Wires` and rebuilt
		// with the order, so a mixer asking per node per block neither
		// allocates nor walks every wire.
		mutable std::vector<std::vector<NodeId>> Sources;

		NodeId Sink;
		uint32_t NextNode = 1;

		ListenerPose Ear;

		// Mutable because `Order` is logically a query: it answers the same
		// thing whether or not the cache happened to be warm.
		mutable std::vector<NodeId> Sorted;
		mutable bool OrderStale = true;
	};
}
