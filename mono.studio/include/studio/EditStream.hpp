#pragma once

// One editor's change, arriving in everybody else's.
//
// Team create's replication layer, and it is the third of three pieces rather
// than a thing of its own. The unit was settled by `CommandLog`: a **committed
// waypoint**, handed over whole by `Watcher::Committed`, because a peer that
// applied half of a group would show a state the author never saw. The far end
// was settled by `ApplyForeign`: somebody else's edit lands without entering
// this author's undo stack, because Ctrl+Z is a promise about what *you* did.
// What is here is the middle - an identity two editors share, and a wire.
//
// ## Why the identity is a path
//
// An `EditId` is **one log's own name** for an instance. Two editors issue them
// independently, so the sender's id 5 and the receiver's id 5 are two different
// instances and swapping the tables round would silently edit the wrong one.
//
// So nothing crosses but a path - the chain of names from the world's root -
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
// same sequence - a path resolved against a document that has had every earlier
// edit applied is the path the sender meant. That is what the host's ordering
// buys, and it is why the relay goes through one process rather than
// peer-to-peer among the guests.
//
// ## Conflicts: take turns, and lose nothing
//
// **Ask, hold, edit, give back.** A guest asks the host before it publishes;
// the host hands out the subtree if it is free and queues the request behind
// whoever has it; the guest sends its waypoint; the host applies it, relays it,
// gives the subtree back and grants the next in line - `studio::EditLocks`.
//
// **Nobody's work is thrown away.** Two editors on one model do not race and
// they do not lose: the second edit lands *on top of* the first, in the order
// the host granted. That is the whole reason there is a queue rather than a
// refusal.
//
// **The person never waits.** Their edit is applied at their own machine the
// moment they make it; what waits for the grant is the *message*. A queued
// editor carries on working and their waypoints go out in the order the host
// lets them.
//
// **A waypoint is granted and applied whole.** Half of one is a state the
// author never saw, which is the same reason a waypoint is the unit at all - so
// a waypoint touching two models asks for the smallest subtree that covers
// both.
//
// **No history merge.** A guest's undo stack holds what that guest did. Undoing
// something a colleague has since built on is a conflict this layer does not
// have an answer for, and the honest shape of that is: an undo produces new
// edits, which replicate like any other - and go through the same holds.
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
		// receiver never resolves - the document carries the name.
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
		// rather than a format - putting it on a wire would make an editor's
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

		// A guest asking for a turn on a subtree, and giving one back. The host
		// answers a request with `Granted` when it is free, and with silence
		// when it is not - the grant arrives when the turn does.
		Request = 1,
		Release = 2,

		// The host telling everybody who holds what. A snapshot rather than a
		// difference: the table is small, it changes rarely, and a guest that
		// missed one difference would show the wrong person's name on a model
		// until something else happened to correct it.
		Locks = 3,

		// The host telling one guest that a subtree is theirs to edit now.
		//
		// **The only thing a guest waits for.** It arrives immediately when
		// nobody had the subtree, and when the person in front gives it back
		// otherwise.
		Granted = 4,

		// A guest saying it has arrived, and the host telling it which editor
		// it is.
		//
		// **A guest cannot work out its own number.** Holds are stamped with
		// the host's numbering, so without this a guest could see that
		// *somebody* holds a model and not whether that somebody is itself -
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

		// `Request`, `Release` and `Granted`.
		InstancePath Subject;

		// `Welcome`.
		EditorId Holder = HOST_EDITOR;

		// `Locks`.
		//
		// **Remaining seconds rather than an absolute expiry**, because two
		// editors have no clock in common - the host's `now` means nothing on a
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
	// the instances the commands name still exist - which is why it takes the
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
	// goes - see the note at the top of this file on why the sender's cannot be
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

		// Waypoints that had to wait for a turn.
		//
		// **Counted apart from `Malformed`**, because they are different
		// events: one is somebody working where you are - which is ordinary and
		// costs nothing but a moment - and the other is a peer speaking a
		// language this build does not.
		//
		// @since v0.13
		uint64_t Queued = 0;
	};

	// The editors in one team-create session, and the edits between them.
	//
	// **A host and its guests rather than a mesh**, and the ordering is the
	// reason: paths only resolve to the same instance everywhere if everybody
	// applies the same edits in the same order, and one process deciding that
	// order is the cheapest way to have one. The host applies an incoming edit
	// and relays it; a guest applies what the host sends.
	//
	// It carries edits over the link `replication` already owns - see
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

		// Who is holding what, as this editor last heard it.
		//
		// On a host this is the table itself. On a guest it is the last
		// snapshot the host sent, and it is never consulted to decide
		// anything - a decision two processes could reach differently is a
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

		// How many of this editor's waypoints are waiting for a turn.
		//
		// **For a panel, and it is the honest thing to show**: an edit that has
		// not replicated yet is not an edit that failed, and a person watching
		// a colleague's screen not change wants to know which.
		//
		// @return The count, zero when everything has gone.
		//
		// **Not called `Waiting`**, which is the name of the queue entry a
		// lock table hands back - one word for a count and a record would make
		// `for (const Waiting &...)` inside this class resolve to the method.
		size_t Backlog() const {
			return Pending.size();
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
		// **A peer whose handshake has not finished is not in.** Under QUIC a
		// connection exists before it can carry anything, so a host that counted
		// it would show a person an editor who would miss whatever they typed
		// next - `replication::Listener::Carrying` is the number this reads.
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

		// One waypoint this editor has made and not yet been allowed to send.
		//
		// **The records rather than the commands**, because a path is read out
		// of the store while the instances still exist and a queued waypoint
		// may outlive them - a delete's subject is gone by the time the turn
		// comes round.
		struct Held {
			// The subtree the whole waypoint asked for.
			InstancePath Subject;

			// What to send once the turn arrives.
			std::vector<std::byte> Payload;

			// The same waypoint, kept unencoded so it can be *replayed*.
			//
			// **This is what makes an optimistic edit converge.** A person's
			// edit is applied at their own machine the moment they make it, and
			// somebody else's may then arrive from the host having been ordered
			// *before* it - at which point this machine holds the wrong answer
			// and everybody else holds the right one. Re-applying what is still
			// waiting, after each foreign waypoint, puts it back on top. That
			// is the loser's view of "applied on top after the first person".
			std::vector<EditRecord> Records;

			// When the request was last sent, so a lost grant is asked for
			// again rather than waited on for ever.
			double AskedAtSeconds = 0.0;
		};

		// Sends the table to every guest. Host only.
		void PublishLocks(double nowSeconds);

		// Sends what a granted subtree was waiting to say.
		void Flush(const InstancePath &granted, double nowSeconds);

		// Tells one editor a subtree is theirs. Host only.
		void Grant(EditorId editor, const InstancePath &path, double nowSeconds);

		// Notes which client an editor id belongs to, so a grant can be
		// addressed. Host only.
		void Remember(EditorId editor, engine::replication::ClientId client);

		CommandLog *Log = nullptr;
		engine::world::Universe *Worlds = nullptr;

		// Who is holding what. Authoritative on the host, a copy on a guest.
		LockTable Holds;

		// Which editor this one is. A guest learns it from the host.
		EditorId Me = HOST_EDITOR;

		// Whether this guest has said hello. Once, when the link comes up.
		bool Greeted = false;

		// How long a guest waits for a grant before asking again.
		//
		// Longer than a round trip and shorter than a person notices. One lost
		// datagram must not strand an edit.
		static constexpr double RETRY_SECONDS = 1.0;

		// Waypoints made and not yet allowed out, oldest first.
		//
		// **Oldest first, because that is the order they were made in.** An
		// editor's own edits must reach everybody else in the order they
		// happened, whatever order the turns come back in.
		std::vector<Held> Pending;

		// Which client each editor id belongs to, so a grant can be addressed.
		// Host only.
		std::vector<std::pair<EditorId, engine::replication::ClientId>> Members;

		// Exactly one of these. A host orders; a guest is ordered.
		std::unique_ptr<engine::replication::Listener> Server;
		std::unique_ptr<engine::replication::Connector> Client;

		// The store a guest's connector writes its replica into.
		//
		// **Unused, and it has to exist.** `Connector::Poll` takes a store
		// because its ordinary job is a replicated world; this session carries
		// no world at all - the document is the thing being shared and it is
		// shared as edits. So the replica is an empty store nothing reads,
		// which is cheaper and clearer than a second Poll that skips it.
		std::unique_ptr<engine::ecs::Store> Unused;

		// The clock the poll is running at, for the handlers the links call
		// back into. A handler is invoked from inside `Poll` and has no
		// argument to carry a time on, and reading one here would put a wall
		// clock in the middle of a tick - which `net/AGENTS.md` bans.
		double PollingAt = 0.0;

		EditCounters Tally;
	};
}
