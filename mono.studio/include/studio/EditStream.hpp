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
// ## Conflicts
//
// **Whoever touched a subtree first holds it** — `studio::EditLocks`, enforced
// here at the host because the host is already the thing that decides order. A
// waypoint touching something somebody else holds is refused whole and the
// sender is told who is in the way; a hold lapses on its own, so an editor that
// crashed does not keep a model for ever.
//
// Nobody asks for a hold and nobody waits for one. A design where a guest
// requests and then edits costs a round trip before the first edit of every
// interaction, so clicking a part and dragging it would fail until the answer
// came back — worse than the problem it solves. `EditLocks.hpp` carries that
// argument in full.
//
// **Refused whole, never in part.** Half a waypoint is a state the author never
// saw, which is the same reason a waypoint is the unit in the first place.
//
// **No history merge.** A guest's undo stack holds what that guest did. Undoing
// something a colleague has since built on is a conflict this layer does not
// have an answer for, and the honest shape of that is: an undo produces new
// edits, which replicate like any other — and go through the same holds.
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
#include <studio/EditLocks.hpp>
#include <studio/InstancePath.hpp>
#include <vector>

namespace studio {

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

	// What one message on an edit stream is.
	//
	// **A closed list whose ordinal reaches a wire**, so a value may be added
	// at the end and none may be reordered.
	//
	// @since v0.13
	enum class EditFrame : uint8_t {
		// One committed waypoint's records, in the order they were made.
		Waypoint = 0,

		// A guest asking to hold a subtree before working on it, or giving one
		// up. The host answers by sending the table.
		Claim = 1,
		Release = 2,

		// The host telling everybody who holds what. A snapshot rather than a
		// difference: the table is small, it changes rarely, and a guest that
		// missed one difference would show the wrong person's name on a model
		// until something else happened to correct it.
		Locks = 3,

		// The host telling one guest that a waypoint was refused, and by whom.
		//
		// **Sent rather than left to be inferred from the lock table.** A guest
		// whose edit vanished with no message would report it as replication
		// being broken, and it is not — somebody else is working there.
		Denied = 4,

		// A guest saying it has arrived, and the host telling it which editor
		// it is.
		//
		// **A guest cannot work out its own number.** Holds are stamped with
		// the host's numbering, so without this a guest could see that
		// *somebody* holds a model and not whether that somebody is itself —
		// and would grey out its own work. The host answers with the current
		// table too, so a guest that joins a session already in progress does
		// not wait for the next change to see who is where.
		Hello = 5,
		Welcome = 6,
	};

	// One message, whichever kind it is.
	//
	// @since v0.13
	struct EditMessage {
		// Which kind, and therefore which fields below carry anything.
		EditFrame Kind = EditFrame::Waypoint;

		// `Waypoint`.
		std::vector<EditRecord> Records;

		// `Claim`, `Release` and `Denied`.
		InstancePath Subject;

		// `Denied` and `Welcome`.
		EditorId Holder = HOST_EDITOR;

		// `Locks`.
		//
		// **Remaining seconds rather than an absolute expiry**, because two
		// editors have no clock in common — the host's `now` means nothing on a
		// guest, and a lease stamped with it would look already lapsed or
		// eternal depending on which machine booted first.
		std::vector<Lease> Locks;
	};

	// Encodes one waypoint's records.
	//
	// @param records The records, in the order they were made.
	// @return The bytes.
	// @since v0.13
	std::vector<std::byte> EncodeEdits(std::span<const EditRecord> records);

	// Encodes any message.
	//
	// @param message The message.
	// @return The bytes.
	// @since v0.13
	std::vector<std::byte> EncodeMessage(const EditMessage &message);

	// Decodes what `EncodeMessage` wrote.
	//
	// Every byte is hostile: it arrived over a link from another editor, and an
	// editor somebody joined is an editor somebody can send anything to.
	//
	// @param bytes The bytes.
	// @return The message, or nothing when the bytes are not one.
	// @since v0.13
	std::optional<EditMessage> DecodeMessage(std::span<const std::byte> bytes);

	// Decodes a waypoint, refusing every other kind.
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

		// Payloads that were not a message.
		uint64_t Malformed = 0;

		// Waypoints refused because somebody else was holding what they
		// touched.
		//
		// **Counted apart from `Malformed`**, because they are different
		// events: one is somebody working where you are, the other is a peer
		// speaking a language this build does not. An operator — or a person
		// wondering why their edit did not stick — wants to tell them apart.
		//
		// @since v0.13
		uint64_t Contested = 0;
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

		// Reserves a subtree before working on it.
		//
		// Nothing needs this: a hold is taken by editing. It is here for the
		// person who wants to say *before* they start that they are about to
		// work somewhere, which is the one thing the implicit path cannot
		// express.
		//
		// @param path       The subtree.
		// @param nowSeconds The current time.
		// @return `false` when there is no session, or the host already knows
		//         somebody else holds it. A guest's answer arrives with the
		//         next lock table rather than from this call, because the host
		//         is the one that decides.
		// @since v0.13
		bool Claim(const InstancePath &path, double nowSeconds);

		// Gives up a subtree.
		//
		// @param path       The subtree.
		// @param nowSeconds The current time.
		// @return Whether anything was said.
		// @since v0.13
		bool Release(const InstancePath &path, double nowSeconds);

		// Who is holding what, as this editor last heard it.
		//
		// On a host this is the table itself. On a guest it is the last
		// snapshot the host sent, and it is never consulted to decide
		// anything — a decision two processes could reach differently is a
		// decision that will be.
		//
		// @return The table.
		// @since v0.13
		const LockTable &Locks() const {
			return Holds;
		}

		// Which editor this one is, as the session numbers them.
		//
		// @return `HOST_EDITOR` for a host, or what the host called this guest.
		// @since v0.13
		EditorId Self() const {
			return Me;
		}

		// The last refusal, for a panel that has to explain one.
		//
		// @return What was in the way, or nothing since the last edit that
		//         landed.
		// @since v0.13
		const std::optional<Blocked> &LastRefusal() const {
			return Refused;
		}

		// Puts one waypoint on the wire.
		//
		// **What `CommandLog::Watcher::Committed` hands over, and nothing
		// else.** An undo publishes nothing: it is this author navigating their
		// own history, and what a peer needs to hear about is the edits it
		// produces rather than the walking.
		//
		// @param waypoint   Which waypoint it is, so a refusal can name it.
		// @param commands   One waypoint's worth.
		// @param nowSeconds The current time.
		// @return Whether it went.
		bool Publish(uint64_t waypoint, std::span<const Command> commands, double nowSeconds);

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

		// Sends the table to every guest. Host only.
		void PublishLocks(double nowSeconds);

		// Whether a waypoint may land, and what to hold if it does.
		//
		// @param records    The waypoint.
		// @param holder     Who sent it.
		// @param nowSeconds The current time.
		// @return What is in the way, or nothing.
		std::optional<Blocked>
		Contest(std::span<const EditRecord> records, EditorId holder, double nowSeconds);

		CommandLog *Log = nullptr;
		engine::world::Universe *Worlds = nullptr;

		// Who is holding what. Authoritative on the host, a copy on a guest.
		LockTable Holds;

		// Which editor this one is. A guest learns it from the host.
		EditorId Me = HOST_EDITOR;

		// Whether this guest has said hello. Once, when the link comes up.
		bool Greeted = false;

		// What blocked the last refused waypoint, for a panel to explain.
		std::optional<Blocked> Refused;

		// The waypoint this editor published most recently.
		//
		// **So a refusal can be taken back.** A guest applies its own edit
		// locally the moment it is made and only then finds out the host
		// refused it — without this the loser of a race keeps a change nobody
		// else has, which is the divergence the whole ordered stream exists to
		// prevent.
		uint64_t Published = 0;

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
