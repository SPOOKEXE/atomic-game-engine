#include <engine/core/Bytes.hpp>
#include <engine/core/Log.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/game/Values.hpp>

#include <algorithm>
#include <studio/EditStream.hpp>
#include <utility>

namespace studio {

	using engine::ecs::Entity;
	using engine::ecs::NULL_ENTITY;
	using engine::ecs::Store;
	using engine::world::WorldId;

	namespace {
		// How this session is paced.
		//
		// **A short keep-alive, and it is load-bearing rather than tidy.** An
		// acknowledgement rides on an outgoing packet, and an edit stream sends
		// nothing between edits — no world, no delta, no input. At the default
		// one-second keep-alive the reliable window therefore goes
		// unacknowledged for up to a second, and a burst of edits inside that
		// second is refused by a link that is working perfectly.
		//
		// A tenth of a second costs ten small datagrams a second per editor,
		// which against a session carrying whole subtrees is nothing, and it
		// bounds how long an edit can wait to be acknowledged rather than
		// leaving it to whenever the next one happens to be sent.
		engine::replication::SessionSettings EditSession() {
			engine::replication::SessionSettings settings;
			settings.Link.KeepAliveSeconds = 0.1;
			return settings;
		}

		// The frame's magic, so a payload that is not a waypoint fails at its
		// first four bytes rather than somewhere inside a path.
		constexpr uint32_t EDIT_MAGIC = 0x45445441; // "ATDE"

		// The frame version. Refused when unknown, for the reason every other
		// format here gives: a reader that guesses mis-parses hostile bytes.
		constexpr uint16_t EDIT_VERSION = 1;

		// The most records one waypoint may carry.
		//
		// A recording that touched more than this is a recording nobody made by
		// hand, and the bound is what stops a peer allocating from a length
		// field somebody else wrote.
		constexpr uint32_t MAXIMUM_RECORDS = 4096;

		// The most names one path may have, matching `InstancePath.cpp`'s own
		// cap. A tree this deep is a tree nobody authored.
		constexpr uint32_t MAXIMUM_DEPTH = 64;

		void WritePath(engine::core::ByteWriter &writer, const InstancePath &path) {
			writer.WriteUInt32(static_cast<uint32_t>(path.size()));
			for (const std::string &name : path) {
				writer.WriteString(name);
			}
		}

		bool ReadPath(engine::core::ByteReader &reader, InstancePath &path) {
			const uint32_t depth = reader.ReadUInt32();
			if (reader.Failed() || depth > MAXIMUM_DEPTH) {
				return false;
			}
			path.clear();
			path.reserve(depth);
			for (uint32_t index = 0; index < depth; ++index) {
				path.emplace_back(reader.ReadString());
				if (reader.Failed()) {
					return false;
				}
			}
			return true;
		}

		// The most locks one table or one snapshot may carry, matching
		// `LockSettings::MaximumHolds`. Checked before anything is reserved: a
		// length field is what somebody writes to make a peer allocate.
		constexpr uint32_t MAXIMUM_LOCKS = 256;

		void WriteRecords(engine::core::ByteWriter &writer, std::span<const EditRecord> records) {
			writer.WriteUInt32(static_cast<uint32_t>(records.size()));
			for (const EditRecord &record : records) {
				writer.WriteUInt8(static_cast<uint8_t>(record.Kind));
				writer.WriteString(record.World);
				WritePath(writer, record.Subject);
				WritePath(writer, record.OldParent);
				WritePath(writer, record.NewParent);
				writer.WriteString(record.Document);
				writer.WriteString(record.Property);
				writer.WriteUInt8(record.PropertyType);
				writer.WriteString(record.Before);
				writer.WriteString(record.After);
				writer.WriteString(record.Description);
			}
		}

		bool ReadRecords(engine::core::ByteReader &reader, std::vector<EditRecord> &records) {
			const uint32_t count = reader.ReadUInt32();
			if (reader.Failed() || count > MAXIMUM_RECORDS) {
				return false;
			}

			records.clear();
			records.reserve(count);
			for (uint32_t index = 0; index < count; ++index) {
				EditRecord record;

				const uint8_t kind = reader.ReadUInt8();
				if (reader.Failed() || kind > static_cast<uint8_t>(CommandKind::Property)) {
					return false;
				}
				record.Kind = static_cast<CommandKind>(kind);

				record.World = reader.ReadString();
				if (!ReadPath(reader, record.Subject) || !ReadPath(reader, record.OldParent) ||
					!ReadPath(reader, record.NewParent)) {
					return false;
				}
				record.Document = reader.ReadString();
				record.Property = reader.ReadString();
				record.PropertyType = reader.ReadUInt8();
				record.Before = reader.ReadString();
				record.After = reader.ReadString();
				record.Description = reader.ReadString();

				if (reader.Failed()) {
					return false;
				}
				records.push_back(std::move(record));
			}
			return true;
		}

		// The world a name belongs to, or an invalid id.
		//
		// **By name, because a `WorldId` is an index into one process's
		// registry.** Two editors that created their worlds in a different
		// order hold the same scene under different handles, and a stream that
		// carried the number would apply an edit to whichever scene happened to
		// share it.
		WorldId WorldNamed(engine::world::Universe &universe, std::string_view name) {
			for (const WorldId world : universe.Worlds()) {
				if (universe.NameOf(world).Text() == name) {
					return world;
				}
			}
			return {};
		}
	}

	std::vector<std::byte> EncodeMessage(const EditMessage &message) {
		engine::core::ByteWriter writer;
		writer.WriteUInt32(EDIT_MAGIC);
		writer.WriteUInt16(EDIT_VERSION);
		writer.WriteUInt8(static_cast<uint8_t>(message.Kind));

		switch (message.Kind) {
		case EditFrame::Claim:
		case EditFrame::Release:
			WritePath(writer, message.Subject);
			break;

		case EditFrame::Denied:
			WritePath(writer, message.Subject);
			writer.WriteUInt32(message.Holder);
			break;

		case EditFrame::Welcome:
			writer.WriteUInt32(message.Holder);
			break;

		case EditFrame::Hello:
			break;

		case EditFrame::Locks:
			writer.WriteUInt32(static_cast<uint32_t>(message.Locks.size()));
			for (const Lease &lease : message.Locks) {
				WritePath(writer, lease.Subject);
				writer.WriteUInt32(lease.Holder);
				writer.WriteBool(lease.Claimed);
			}
			break;

		case EditFrame::Waypoint:
			WriteRecords(writer, message.Records);
			break;
		}

		return {writer.Bytes().begin(), writer.Bytes().end()};
	}

	std::vector<std::byte> EncodeEdits(std::span<const EditRecord> records) {
		EditMessage message;
		message.Kind = EditFrame::Waypoint;
		message.Records.assign(records.begin(), records.end());
		return EncodeMessage(message);
	}

	std::optional<EditMessage> DecodeMessage(std::span<const std::byte> bytes) {
		engine::core::ByteReader reader(bytes);
		if (reader.ReadUInt32() != EDIT_MAGIC || reader.ReadUInt16() != EDIT_VERSION) {
			return std::nullopt;
		}

		const uint8_t kind = reader.ReadUInt8();
		if (reader.Failed() || kind > static_cast<uint8_t>(EditFrame::Welcome)) {
			return std::nullopt;
		}

		EditMessage message;
		message.Kind = static_cast<EditFrame>(kind);

		switch (message.Kind) {
		case EditFrame::Claim:
		case EditFrame::Release:
			if (!ReadPath(reader, message.Subject)) {
				return std::nullopt;
			}
			break;

		case EditFrame::Denied:
			if (!ReadPath(reader, message.Subject)) {
				return std::nullopt;
			}
			message.Holder = reader.ReadUInt32();
			break;

		case EditFrame::Welcome:
			message.Holder = reader.ReadUInt32();
			break;

		case EditFrame::Hello:
			break;

		case EditFrame::Locks: {
			const uint32_t count = reader.ReadUInt32();
			// The same bound the table itself keeps, checked before anything is
			// reserved: a length field is what somebody writes to make a peer
			// allocate.
			if (reader.Failed() || count > MAXIMUM_LOCKS) {
				return std::nullopt;
			}
			message.Locks.reserve(count);
			for (uint32_t index = 0; index < count; ++index) {
				Lease lease;
				if (!ReadPath(reader, lease.Subject)) {
					return std::nullopt;
				}
				lease.Holder = reader.ReadUInt32();
				lease.Claimed = reader.ReadBool();
				if (reader.Failed()) {
					return std::nullopt;
				}
				// **A guest's copy never expires on its own.** It is a picture
				// of what the host said, replaced whole by the next one; giving
				// it a deadline off this machine's clock would have it lapse at
				// a moment the host never chose.
				lease.ExpiresAtSeconds = 0.0;
				message.Locks.push_back(std::move(lease));
			}
			break;
		}

		case EditFrame::Waypoint:
			if (!ReadRecords(reader, message.Records)) {
				return std::nullopt;
			}
			break;
		}

		if (reader.Failed() || reader.Remaining() != 0) {
			// Trailing bytes are a refusal. A frame whose fields ended before
			// its bytes did is one somebody appended to.
			return std::nullopt;
		}
		return message;
	}

	std::optional<std::vector<EditRecord>> DecodeEdits(std::span<const std::byte> bytes) {
		std::optional<EditMessage> message = DecodeMessage(bytes);
		if (!message || message->Kind != EditFrame::Waypoint) {
			return std::nullopt;
		}
		return std::move(message->Records);
	}

	std::vector<EditRecord>
	DescribeEdits(CommandLog &log, engine::world::Universe &universe, std::span<const Command> commands) {
		std::vector<EditRecord> records;
		records.reserve(commands.size());

		for (const Command &command : commands) {
			if (!command.World.IsValid()) {
				continue;
			}

			EditRecord record;
			record.Kind = command.Kind;
			record.World = universe.NameOf(command.World).Text();
			record.Document = command.Document;
			record.Property = command.Property.Text();
			record.PropertyType = static_cast<uint8_t>(command.After.Type);
			record.Before = engine::game::FormatValue(command.Before);
			record.After = engine::game::FormatValue(command.After);
			record.Description = command.Description;

			// The paths are read out of the store while the instances still
			// exist, which is why this runs from `Watcher::Committed` and not
			// from a queue drained later.
			bool usable = true;
			universe.Enter(command.World, [&](Store &store) {
				const Entity subject = log.Resolve(command.Subject);
				const Entity oldParent = log.Resolve(command.OldParent);
				const Entity newParent = log.Resolve(command.NewParent);

				// A destroy is recorded *before* the destroy, so its subject is
				// still alive here — and a create's is alive because it was
				// just made.
				record.Subject = PathOf(store, subject);
				record.OldParent = PathOf(store, oldParent);
				record.NewParent = PathOf(store, newParent);

				// A create rebuilds from its document under `OldParent`, so the
				// subject's own path is not needed at the far end; everything
				// else has to name something that already exists there.
				if (command.Kind != CommandKind::Create && record.Subject.empty()) {
					usable = false;
				}
			});

			if (!usable) {
				continue;
			}
			records.push_back(std::move(record));
		}

		return records;
	}

	size_t
	ApplyEdits(CommandLog &log, engine::world::Universe &universe, std::span<const EditRecord> records) {
		std::vector<Command> commands;
		commands.reserve(records.size());

		for (const EditRecord &record : records) {
			const WorldId world = WorldNamed(universe, record.World);
			if (!world.IsValid()) {
				// A scene this editor does not have. Dropped rather than
				// created: a stream that conjured a world would have two
				// editors disagreeing about what the project contains, and the
				// place to fix that is the join rather than each edit.
				continue;
			}

			Command command;
			command.Kind = record.Kind;
			command.World = world;
			command.Document = record.Document;
			command.Property = engine::core::Name(record.Property);
			command.Description = record.Description;

			if (record.Kind == CommandKind::Property) {
				const auto type = static_cast<engine::ecs::PropertyType>(record.PropertyType);
				std::string reason;
				if (!engine::game::ParseValue(type, record.Before, command.Before, reason) ||
					!engine::game::ParseValue(type, record.After, command.After, reason)) {
					ENGINE_WARN("team create: could not read '{}': {}", record.Description, reason);
					continue;
				}
			}

			// **Every id here is this log's own.** The sender's are one process's
			// names for its own instances — see `EditStream.hpp`.
			bool usable = true;
			universe.Enter(world, [&](Store &store) {
				const auto track = [&](const InstancePath &path, EditId &out, bool required) {
					if (path.empty()) {
						// A root-level instance has no parent, which is a
						// legitimate answer rather than a missing one.
						out = EditId{};
						return !required;
					}
					const Entity found = ResolvePath(store, path);
					if (found == NULL_ENTITY) {
						return !required;
					}
					out = log.Track(world, found);
					return true;
				};

				if (record.Kind == CommandKind::Create) {
					// Nothing to resolve: the subject does not exist here yet.
					// A fresh id, which `ApplyForeign` binds to whatever the
					// rebuild produces — so a later record naming the same
					// instance resolves it by path and tracks the same entity.
					command.Subject = log.Mint();
				} else if (!track(record.Subject, command.Subject, true)) {
					usable = false;
					return;
				}

				// A create and a destroy hang their rebuild under this, so a
				// parent that does not resolve is a subtree rebuilt as a root
				// rather than a refusal — `CommandLog::Apply` already makes
				// that choice and this must not make a different one.
				track(record.OldParent, command.OldParent, false);
				track(record.NewParent, command.NewParent, false);
			});

			if (!usable) {
				continue;
			}
			commands.push_back(std::move(command));
		}

		return log.ApplyForeign(commands);
	}

	// -------------------------------------------------------------------------
	// The session
	// -------------------------------------------------------------------------

	std::unique_ptr<EditStream>
	EditStream::Host(engine::net::Transport &transport, CommandLog &log, engine::world::Universe &universe) {
		engine::replication::ListenerSettings serving;
		serving.Session = EditSession();

		std::unique_ptr<EditStream> stream(new EditStream(log, universe));
		stream->Server = std::make_unique<engine::replication::Listener>(transport, serving);

		EditStream *self = stream.get();
		stream->Server->OnUserMessage(
			[self](engine::replication::ClientId from, std::span<const std::byte> payload) {
				// The host's clock at the moment it polled. Passed down rather
				// than read here, for the module rule every layer under this
				// one keeps.
				self->Receive(payload, self->PollingAt, from);
			}
		);
		return stream;
	}

	std::unique_ptr<EditStream> EditStream::Join(
		engine::net::Transport &transport,
		const engine::net::Endpoint &host,
		double nowSeconds,
		CommandLog &log,
		engine::world::Universe &universe
	) {
		std::unique_ptr<EditStream> stream(new EditStream(log, universe));
		stream->Unused = std::make_unique<Store>("teamcreate.unused");
		engine::replication::ConnectorSettings joining;
		joining.Session = EditSession();
		stream->Client =
			std::make_unique<engine::replication::Connector>(transport, host, nowSeconds, joining);

		EditStream *self = stream.get();
		stream->Client->OnUserMessage([self](std::span<const std::byte> payload) {
			self->Receive(payload, self->PollingAt, {});
		});
		return stream;
	}

	EditStream::~EditStream() = default;

	bool EditStream::Connected() const {
		// A host needs nobody's permission to edit its own document.
		return Server != nullptr || (Client != nullptr && Client->Admitted());
	}

	size_t EditStream::Editors() const {
		if (Server != nullptr) {
			return Server->Count() + 1;
		}
		return Client != nullptr && Client->Admitted() ? 2 : 1;
	}

	bool EditStream::Publish(uint64_t waypoint, std::span<const Command> commands, double nowSeconds) {
		if (commands.empty() || Worlds == nullptr || Log == nullptr) {
			return false;
		}

		const std::vector<EditRecord> records = DescribeEdits(*Log, *Worlds, commands);
		if (records.empty()) {
			return false;
		}

		Published = waypoint;

		const std::vector<std::byte> payload = EncodeEdits(records);

		if (Server != nullptr) {
			// The host contends against its own table too. It is not exempt:
			// an editor hosting is still an editor, and a host that could edit
			// through somebody else's hold would make the whole thing
			// advisory.
			if (const std::optional<Blocked> blocked = Contest(records, HOST_EDITOR, nowSeconds)) {
				Refused = blocked;
				Tally.Contested++;

				// The same rollback a guest does on a denial. A host is an
				// editor like any other, including in what happens when it
				// loses.
				if (Log != nullptr && Log->CanUndo() && Log->Undoable().back().Waypoint == waypoint) {
					Log->Undo();
				}
				return false;
			}
			for (const EditRecord &record : records) {
				Holds.Hold(record.Subject, HOST_EDITOR, nowSeconds);
			}

			// Nobody excepted: this is the host's own edit and every guest has
			// to hear it.
			Server->Broadcast(payload, nowSeconds);
			Tally.Sent++;
			PublishLocks(nowSeconds);
			return true;
		}

		if (Client == nullptr || !Client->SendUser(payload, nowSeconds)) {
			// Counted rather than queued. A guest whose link is not up yet has
			// not joined the session, and holding edits until it does would
			// replay a burst of them into a document that has moved on.
			Tally.Undelivered++;
			return false;
		}
		Tally.Sent++;
		return true;
	}

	std::optional<Blocked>
	EditStream::Contest(std::span<const EditRecord> records, EditorId holder, double nowSeconds) {
		// **Every path a record touches, and the whole waypoint refused if any
		// of them is held.** Half a waypoint is a state the author never saw,
		// which is the same reason a waypoint is the unit in the first place.
		for (const EditRecord &record : records) {
			// **A create contends against where it is going and holds what it
			// made, and those are two different paths.** Somebody holding a
			// model owns what gets put inside it, so the parent is what decides
			// whether the create may happen — but *holding* the parent would
			// mean creating one part under Workspace locks the entire scene
			// against everybody, which is not a lock anybody asked for.
			const InstancePath &against =
				record.Kind == CommandKind::Create ? record.OldParent : record.Subject;

			if (const std::optional<Blocked> blocked = Holds.Blocking(against, holder, nowSeconds)) {
				return blocked;
			}

			// A reparent moves an instance *into* somewhere, so the destination
			// is contended too — otherwise two editors could drop parts into
			// one model nobody else was allowed to touch.
			if (record.Kind == CommandKind::Reparent) {
				if (const std::optional<Blocked> blocked =
						Holds.Blocking(record.NewParent, holder, nowSeconds)) {
					return blocked;
				}
			}
		}
		return std::nullopt;
	}

	void EditStream::PublishLocks(double nowSeconds) {
		if (Server == nullptr) {
			return;
		}

		EditMessage message;
		message.Kind = EditFrame::Locks;
		message.Locks.assign(Holds.Held().begin(), Holds.Held().end());
		Server->Broadcast(EncodeMessage(message), nowSeconds);
	}

	void EditStream::Receive(
		std::span<const std::byte> payload, double nowSeconds, engine::replication::ClientId from
	) {
		const std::optional<EditMessage> message = DecodeMessage(payload);
		if (!message) {
			// An editor somebody joined is an editor somebody can send anything
			// to. Counted and dropped.
			Tally.Malformed++;
			return;
		}

		// A guest is numbered by the host, so every editor in the session sees
		// the same number for the same person.
		//
		// **One past the client's slot, because slots start at zero and zero is
		// the host.** Without the offset the first guest to join *is* the host
		// as far as every hold is concerned, and the two would edit through
		// each other's locks — which is the one collision this whole mechanism
		// exists to prevent, arriving by arithmetic.
		const EditorId sender = Server != nullptr ? from.Index + 1 : HOST_EDITOR;

		switch (message->Kind) {
		case EditFrame::Locks:
			// A guest taking the host's picture of who holds what. Never
			// consulted to decide anything — a decision two processes could
			// reach differently is a decision that will be.
			Holds.Adopt(message->Locks);
			return;

		case EditFrame::Welcome:
			// Which editor the host is calling this one. Without it a guest
			// could see that somebody holds a model and not whether that
			// somebody is itself, and would grey out its own work.
			Me = message->Holder;
			return;

		case EditFrame::Denied:
			// Somebody else is working there. Kept so a panel can say who,
			// rather than leaving an edit to vanish with no explanation.
			Refused = Blocked{message->Subject, message->Holder};
			Tally.Contested++;

			// **And taken back**, because a guest applies its own edit the
			// moment it is made and only then hears that the host refused it.
			// Without this the loser of a race keeps a change nobody else has,
			// which is exactly the divergence an ordered stream exists to
			// prevent.
			//
			// Only when it is still the top of the stack: a person who has gone
			// on to do something else has built on it, and silently reaching
			// past their newer work would be worse than the divergence.
			if (Published != 0 && Log != nullptr && Log->CanUndo() &&
				Log->Undoable().back().Waypoint == Published) {
				Log->Undo();
				Published = 0;
			}
			return;

		case EditFrame::Hello: {
			if (Server == nullptr) {
				// A guest does not arbitrate. A hello arriving at one is a peer
				// that has the roles the wrong way round.
				Tally.Malformed++;
				return;
			}

			EditMessage welcome;
			welcome.Kind = EditFrame::Welcome;
			welcome.Holder = sender;
			Server->SendTo(from, EncodeMessage(welcome), nowSeconds);

			// And the table as it stands, so a guest joining a session already
			// in progress does not wait for the next change to see who is
			// where.
			EditMessage locks;
			locks.Kind = EditFrame::Locks;
			locks.Locks.assign(Holds.Held().begin(), Holds.Held().end());
			Server->SendTo(from, EncodeMessage(locks), nowSeconds);
			return;
		}

		case EditFrame::Claim:
		case EditFrame::Release: {
			if (Server == nullptr) {
				Tally.Malformed++;
				return;
			}
			if (message->Kind == EditFrame::Claim) {
				Holds.Hold(message->Subject, sender, nowSeconds, true);
			} else {
				Holds.Release(message->Subject, sender);
			}
			PublishLocks(nowSeconds);
			return;
		}

		case EditFrame::Waypoint:
			break;
		}

		if (Server != nullptr) {
			// **The arbitration, and it happens here because the ordering
			// already does.** One process decides who was first; a guest
			// deciding for itself would be two answers to one question, and the
			// two would differ exactly when it mattered.
			if (const std::optional<Blocked> blocked = Contest(message->Records, sender, nowSeconds)) {
				EditMessage denial;
				denial.Kind = EditFrame::Denied;
				denial.Subject = blocked->Subject;
				denial.Holder = blocked->Holder;
				Server->SendTo(from, EncodeMessage(denial), nowSeconds);

				Tally.Contested++;
				return;
			}

			// Taken by editing rather than asked for. Every further edit renews
			// it, so somebody working on a model holds it for as long as they
			// keep working and for `HoldSeconds` after they stop.
			//
			// What is held is what was *touched* — for a create that is the
			// instance it made, not the parent it made it under.
			for (const EditRecord &record : message->Records) {
				Holds.Hold(record.Subject, sender, nowSeconds);
			}
		}

		Tally.Received++;
		Tally.Applied += ApplyEdits(*Log, *Worlds, message->Records);

		if (Server != nullptr) {
			// **The relay, and the exception is the point.** Everybody else
			// hears it in the order this process applied it, which is what
			// makes a path resolve to the same instance in every editor;
			// sending it back to its author would have them apply their own
			// change twice.
			Server->Broadcast(payload, nowSeconds, from);
			Tally.Relayed++;
			PublishLocks(nowSeconds);
		}
	}

	bool EditStream::Claim(const InstancePath &path, double nowSeconds) {
		if (path.empty()) {
			return false;
		}

		if (Server != nullptr) {
			if (!Holds.Hold(path, HOST_EDITOR, nowSeconds, true)) {
				return false;
			}
			PublishLocks(nowSeconds);
			return true;
		}

		if (Client == nullptr) {
			return false;
		}
		// A guest asks and hears the answer in the next table. The host is the
		// one that decides, so a guest that returned "yes" from here would be
		// answering a question it does not have the information for.
		EditMessage message;
		message.Kind = EditFrame::Claim;
		message.Subject = path;
		return Client->SendUser(EncodeMessage(message), nowSeconds);
	}

	bool EditStream::Release(const InstancePath &path, double nowSeconds) {
		if (path.empty()) {
			return false;
		}

		if (Server != nullptr) {
			const bool released = Holds.Release(path, HOST_EDITOR);
			if (released) {
				PublishLocks(nowSeconds);
			}
			return released;
		}

		if (Client == nullptr) {
			return false;
		}
		EditMessage message;
		message.Kind = EditFrame::Release;
		message.Subject = path;
		return Client->SendUser(EncodeMessage(message), nowSeconds);
	}

	void EditStream::Pump(double nowSeconds) {
		PollingAt = nowSeconds;

		if (Server != nullptr) {
			// A hold that has lapsed stops blocking whether or not anybody
			// sweeps it, so this is tidiness — but it is also what makes the
			// table a guest sees match the one the host enforces.
			if (Holds.Expire(nowSeconds) > 0) {
				PublishLocks(nowSeconds);
			}
		}

		if (Server != nullptr) {
			Server->Poll(nowSeconds);
			// **Before the advance, and it is not optional.** A listener's only
			// other flush is inside `Publish`, which this session never calls:
			// there is no world here, the document is the thing being shared
			// and it is shared as edits. Without this the host's
			// acknowledgements never leave, every guest's reliable window
			// fills, and a link that is working perfectly gives up.
			Server->Flush(nowSeconds);
			Server->Advance(nowSeconds);
			return;
		}
		if (Client != nullptr) {
			Client->Poll(*Unused, nowSeconds);

			// Once, when the link comes up. A guest that never says hello never
			// learns its own number, and would grey out its own work.
			if (!Greeted && Client->Admitted()) {
				EditMessage hello;
				hello.Kind = EditFrame::Hello;
				Greeted = Client->SendUser(EncodeMessage(hello), nowSeconds);
			}

			Client->Advance(nowSeconds);
		}
	}
}
