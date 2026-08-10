#pragma once

// One editor's change, arriving in everybody else's.
//
// Team create's replication layer, and it is the third of three pieces rather
// than a thing of its own. The unit was settled by `CommandLog`: a **committed
// waypoint**, handed over whole by `Watcher::Committed`, because a peer that
// applied half of a group would show a state the author never saw. The far end
// was settled by `ApplyForeign`: somebody else's edit lands without entering
// this author's undo stack, because Ctrl+Z is a promise about what *you* did.
// What is here is the middle — an identity two editors share, and a wire.
//
// ## Why the identity is a path
//
// An `EditId` is **one log's own name** for an instance. Two editors issue them
// independently, so the sender's id 5 and the receiver's id 5 are two different
// instances and swapping the tables round would silently edit the wrong one.
//
// So nothing crosses but a path — the chain of names from the world's root —
// and every id on the wire is the *receiver's* own, minted as it applies. That
// is `AGENTS.md` rule 4 read strictly: a name crosses a boundary and a number
// does not, and an `EditId` is a number derived from the order one process
// happened to do things in.
//
// **A path is a list of names, not a joined string.** An instance may be called
// `a/b`, and a separator that can appear inside a name is a separator that
// eventually splits the wrong path.
//
// **Paths stay consistent because the stream is ordered.** A rename is itself a
// replicated property write, so everybody applies it at the same point in the
// same sequence — a path resolved against a document that has had every earlier
// edit applied is the path the sender meant. That is what the host's ordering
// buys, and it is why the relay goes through one process rather than
// peer-to-peer among the guests.
//
// ## What it deliberately does not do
//
// **No conflict resolution, and no locking.** Two editors that move the same
// part in the same beat both send a property write, and the second one applied
// wins for everybody — which is what Roblox's own team create does. Locking is
// the cheap answer and fails in the ordinary case rather than the rare one: two
// people laying out one model touch the same parts constantly, and a lock that
// has to be waited for turns collaboration into taking turns.
//
// **No history merge.** A guest's undo stack holds what that guest did. Undoing
// something a colleague has since built on is a conflict this layer does not
// have an answer for, and the honest shape of that is: an undo produces new
// edits, which replicate like any other.
//
// @tier L12 · client

#include <engine/ecs/Entity.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/replication/Connector.hpp>
#include <engine/replication/Listener.hpp>
#include <engine/world/Universe.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <studio/Commands.hpp>
#include <vector>

namespace studio {

	// An instance named the one way two editors can both read.
	//
	// The chain of names from the world's root down, so `{"Workspace", "Model",
	// "Part"}`. Empty names nothing.
	//
	// @since v0.13
	using InstancePath = std::vector<std::string>;

	// The path of an instance.
	//
	// @param store    The world it lives in.
	// @param instance The instance.
	// @return Its path, or empty for a null or dead handle.
	// @since v0.13
	InstancePath PathOf(const engine::ecs::Store &store, engine::ecs::Entity instance);

	// The instance a path names.
	//
	// **The first match wins and duplicates are the caller's problem.** Two
	// siblings may share a name — Roblox allows it and so does this store — so a
	// path is not a key. It is the best identity available without the document
	// format carrying one, and where it is ambiguous the ambiguity was already
	// there in what a person sees.
	//
	// @param store The world to look in.
	// @param path  The path.
	// @return The instance, or `NULL_ENTITY` when nothing there answers to it.
	// @since v0.13
	engine::ecs::Entity ResolvePath(const engine::ecs::Store &store, const InstancePath &path);

	// One command, in the form that crosses.
	//
	// **Not a `Command`.** A command names ids and a world handle, both of which
	// are one process's own; this names paths and a world's name, which are
	// what two processes can agree on.
	//
	// @since v0.13
	struct EditRecord {
		// Which edit this is.
		CommandKind Kind = CommandKind::Create;

		// Which scene, by name. A world handle is an index into one process's
		// registry and means something else in another.
		std::string World;

		// What was edited. For a `Create` this is where it *will* be, which the
		// receiver never resolves — the document carries the name.
		InstancePath Subject;

		// Where it hung before, and where a rebuild hangs it.
		InstancePath OldParent;

		// Where it hangs now. `Reparent` only.
		InstancePath NewParent;

		// The subtree, for the kinds that rebuild one.
		std::string Document;

		// Which property changed, by name.
		std::string Property;

		// What the property is, so the text below can be read back.
		//
		// **The type crosses with the value.** `game::ParseValue` needs it, and
		// deriving it at the far end from the class table would mean a receiver
		// whose build has a different property type reading the sender's text
		// as something else.
		uint8_t PropertyType = 0;

		// The value before and after, as `game::FormatValue` wrote them.
		//
		// **Text rather than the struct.** A `PropertyValue` is the better part
		// of a kilobyte of every field at once, and its layout is a C++ struct
		// rather than a format — putting it on a wire would make an editor's
		// build a compatibility surface.
		//@{
		std::string Before;
		std::string After;
		//@}

		// What to call this in an Edit menu.
		std::string Description;
	};

	// Encodes one waypoint's records.
	//
	// @param records The records, in the order they were made.
	// @return The bytes.
	// @since v0.13
	std::vector<std::byte> EncodeEdits(std::span<const EditRecord> records);

	// Decodes what `EncodeEdits` wrote.
	//
	// Every byte is hostile: it arrived over a link from another editor, and an
	// editor somebody joined is an editor somebody can send anything to.
	//
	// @param bytes The bytes.
	// @return The records, or nothing when the bytes are not a waypoint.
	// @since v0.13
	std::optional<std::vector<EditRecord>> DecodeEdits(std::span<const std::byte> bytes);

	// Turns a log's commands into records.
	//
	// Called on the sending side, from `CommandLog::Watcher::Committed`, while
	// the instances the commands name still exist — which is why it takes the
	// universe: a path is read out of the store.
	//
	// @param log      The log the commands came from.
	// @param universe The worlds they were made in.
	// @param commands One waypoint's worth.
	// @return The records. Shorter than `commands` when one named something
	//         that has already gone.
	// @since v0.13
	std::vector<EditRecord>
	DescribeEdits(CommandLog &log, engine::world::Universe &universe, std::span<const Command> commands);

	// Applies records to this editor's worlds.
	//
	// **Every id in the commands it builds is this log's own**, minted as it
	// goes — see the note at the top of this file on why the sender's cannot be
	// used. A `Create` gets a fresh one that `ApplyForeign` binds to whatever it
	// rebuilds; everything else resolves its path and tracks what it found.
	//
	// @param log      The log to apply through, which is what keeps the edits
	//        out of this author's undo stack.
	// @param universe The worlds to apply into.
	// @param records  One waypoint's worth.
	// @return How many landed. A record naming a scene this editor does not
	//         have, or an instance it cannot find, is dropped and counted out.
	// @since v0.13
	size_t
	ApplyEdits(CommandLog &log, engine::world::Universe &universe, std::span<const EditRecord> records);

	// What an edit stream has carried.
	//
	// @since v0.13
	struct EditCounters {
		// Waypoints this editor put on the wire.
		uint64_t Sent = 0;

		// Waypoints that arrived.
		uint64_t Received = 0;

		// Individual commands applied out of them.
		uint64_t Applied = 0;

		// Waypoints relayed to the other guests. Host only.
		uint64_t Relayed = 0;

		// Waypoints the link would not take.
		uint64_t Undelivered = 0;

		// Payloads that were not a waypoint.
		uint64_t Malformed = 0;
	};

	// The editors in one team-create session, and the edits between them.
	//
	// **A host and its guests rather than a mesh**, and the ordering is the
	// reason: paths only resolve to the same instance everywhere if everybody
	// applies the same edits in the same order, and one process deciding that
	// order is the cheapest way to have one. The host applies an incoming edit
	// and relays it; a guest applies what the host sends.
	//
	// It carries edits over the link `replication` already owns — see
	// `replication::Listener::SendTo`. A second session type beside that one is
	// the thing this design refuses.
	//
	// @since v0.13
	class EditStream {
	  public:
		// Hosts a session on a transport.
		//
		// @param transport The wire. Borrowed, not owned.
		// @param log       The log to apply arriving edits through.
		// @param universe  The worlds to apply them into.
		// @return The stream.
		static std::unique_ptr<EditStream>
		Host(engine::net::Transport &transport, CommandLog &log, engine::world::Universe &universe);

		// Joins a session hosted elsewhere.
		//
		// @param transport  The wire. Borrowed, not owned.
		// @param host       Where the host is.
		// @param nowSeconds The current time.
		// @param log        The log to apply arriving edits through.
		// @param universe   The worlds to apply them into.
		// @return The stream.
		static std::unique_ptr<EditStream> Join(
			engine::net::Transport &transport,
			const engine::net::Endpoint &host,
			double nowSeconds,
			CommandLog &log,
			engine::world::Universe &universe
		);

		~EditStream();

		EditStream(const EditStream &) = delete;
		EditStream &operator=(const EditStream &) = delete;

		// Puts one waypoint on the wire.
		//
		// **What `CommandLog::Watcher::Committed` hands over, and nothing
		// else.** An undo publishes nothing: it is this author navigating their
		// own history, and what a peer needs to hear about is the edits it
		// produces rather than the walking.
		//
		// @param commands   One waypoint's worth.
		// @param nowSeconds The current time.
		// @return Whether it went.
		bool Publish(std::span<const Command> commands, double nowSeconds);

		// Carries what is waiting, in both directions.
		//
		// @param nowSeconds The current time.
		void Pump(double nowSeconds);

		// Whether this end is the one that orders the session.
		//
		// @return `true` for a host.
		bool Hosting() const {
			return Server != nullptr;
		}

		// Whether a guest's link is up.
		//
		// @return `true` once admitted. Always `true` for a host, which needs
		//         nobody's permission to edit its own document.
		bool Connected() const;

		// How many editors are in, including this one.
		//
		// @return The count.
		size_t Editors() const;

		// What this stream has carried.
		//
		// @return The counters.
		const EditCounters &Counters() const {
			return Tally;
		}

	  private:
		EditStream(CommandLog &log, engine::world::Universe &universe) : Log(&log), Worlds(&universe) {}

		// Applies a payload that arrived, and relays it when hosting.
		void
		Receive(std::span<const std::byte> payload, double nowSeconds, engine::replication::ClientId from);

		CommandLog *Log = nullptr;
		engine::world::Universe *Worlds = nullptr;

		// Exactly one of these. A host orders; a guest is ordered.
		std::unique_ptr<engine::replication::Listener> Server;
		std::unique_ptr<engine::replication::Connector> Client;

		// The store a guest's connector writes its replica into.
		//
		// **Unused, and it has to exist.** `Connector::Poll` takes a store
		// because its ordinary job is a replicated world; this session carries
		// no world at all — the document is the thing being shared and it is
		// shared as edits. So the replica is an empty store nothing reads,
		// which is cheaper and clearer than a second Poll that skips it.
		std::unique_ptr<engine::ecs::Store> Unused;

		// The clock the poll is running at, for the handlers the links call
		// back into. A handler is invoked from inside `Poll` and has no
		// argument to carry a time on, and reading one here would put a wall
		// clock in the middle of a tick — which `net/AGENTS.md` bans.
		double PollingAt = 0.0;

		EditCounters Tally;
	};
}
