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
		// nothing between edits - no world, no delta, no input. At the default
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

		// The smallest subtree covering everything a waypoint touches.
		//
		// **One request per waypoint, not one per record.** A waypoint is
		// granted and applied whole - half of one is a state the author never
		// saw - so it takes one turn, and a waypoint spanning two models asks
		// for the smallest subtree that covers both. That is coarser than
		// asking per record and it is the only shape in which "applied whole"
		// and "took a turn" are the same statement.
		//
		// A create contends for where it is *going*: somebody holding a model
		// owns what gets put inside it.
		InstancePath CommonRoot(std::span<const EditRecord> records) {
			InstancePath root;
			bool first = true;

			for (const EditRecord &record : records) {
				const InstancePath &touched =
					record.Kind == CommandKind::Create ? record.OldParent : record.Subject;
				if (touched.empty()) {
					continue;
				}

				if (first) {
					root = touched;
					first = false;
				} else {
					size_t shared = 0;
					while (shared < root.size() && shared < touched.size() &&
						   root[shared] == touched[shared]) {
						shared++;
					}
					root.resize(shared);
				}

				// A reparent moves an instance *into* somewhere, so the
				// destination is part of what the turn has to cover.
				if (record.Kind == CommandKind::Reparent && !record.NewParent.empty()) {
					size_t shared = 0;
					while (shared < root.size() && shared < record.NewParent.size() &&
						   root[shared] == record.NewParent[shared]) {
						shared++;
					}
					root.resize(shared);
				}
			}

			return root;
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
		case EditFrame::Request:
		case EditFrame::Release:
		case EditFrame::Granted:
			WritePath(writer, message.Subject);
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
		case EditFrame::Request:
		case EditFrame::Release:
		case EditFrame::Granted:
			if (!ReadPath(reader, message.Subject)) {
				return std::nullopt;
			}
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
				// still alive here - and a create's is alive because it was
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
			// names for its own instances - see `EditStream.hpp`.
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
					// rebuild produces - so a later record naming the same
					// instance resolves it by path and tracks the same entity.
					command.Subject = log.Mint();
				} else if (!track(record.Subject, command.Subject, true)) {
					usable = false;
					return;
				}

				// A create and a destroy hang their rebuild under this, so a
				// parent that does not resolve is a subtree rebuilt as a root
				// rather than a refusal - `CommandLog::Apply` already makes
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
		// **QUIC by default, which is `ListenerSettings`'s own default and not a
		// choice made here.** The studio has one host path and one join path and
		// both go through `SessionPort`, so what the editor gets is whatever the
		// engine's listener serves - there is no second transport decision to
		// keep in step. A host with no operator key draws an ephemeral identity;
		// see `ListenerSettings::Quic`.
		engine::replication::ListenerSettings serving;
		serving.Session = EditSession();
		// The ceiling is the one the edit session already stated.
		// `docs/QUIC.md` §6: it survives above the congestion controller rather
		// than instead of it.
		serving.Quic.BytesPerTick = serving.Session.Link.BytesPerTick;

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
		// No transport choice here either: the connector opens with QUIC and
		// falls back if the host refuses. An editor joining another editor has
		// no advert in hand at this point - `Join` takes an address - so it pays
		// the refusal round trip in the case where the host is on the old wire.
		engine::replication::ConnectorSettings joining;
		joining.Session = EditSession();
		joining.Quic.BytesPerTick = joining.Session.Link.BytesPerTick;
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
			// **`Carrying` and not `Count`.** Under QUIC a connection exists
			// before its handshake finishes, and a peer in that window cannot be
			// sent an edit - so counting it would show a person an editor who
			// would miss whatever they typed next.
			return Server->Carrying() + 1;
		}
		return Client != nullptr && Client->Admitted() ? 2 : 1;
	}

	bool EditStream::Publish(uint64_t waypoint, std::span<const Command> commands, double nowSeconds) {
		(void)waypoint;
		if (commands.empty() || Worlds == nullptr || Log == nullptr) {
			return false;
		}

		// Described now, while the instances the commands name still exist - a
		// delete's subject is gone by the time a queued turn comes round.
		const std::vector<EditRecord> records = DescribeEdits(*Log, *Worlds, commands);
		if (records.empty()) {
			return false;
		}

		Held held;
		held.Subject = CommonRoot(records);
		held.Payload = EncodeEdits(records);
		held.Records = records;
		held.AskedAtSeconds = nowSeconds;

		if (Server != nullptr) {
			// The host asks its own table, which answers immediately - there is
			// nobody to ask. It is not exempt from the queue, though: a host
			// that could edit through somebody else's turn would make the whole
			// thing advisory.
			const Turn turn = Holds.Request(held.Subject, HOST_EDITOR, nowSeconds);
			if (turn == Turn::Granted) {
				Server->Broadcast(held.Payload, nowSeconds);
				Tally.Sent++;
				for (const Waiting &woken : Holds.Release(held.Subject, HOST_EDITOR, nowSeconds)) {
					Grant(woken.Holder, woken.Subject, nowSeconds);
				}
				PublishLocks(nowSeconds);
				return true;
			}

			// Queued behind somebody. Kept and sent when the turn arrives,
			// which is what makes this lossless: the edit lands on top of
			// theirs rather than being thrown away.
			Pending.push_back(std::move(held));
			Tally.Queued++;
			PublishLocks(nowSeconds);
			return true;
		}

		if (Client == nullptr) {
			Tally.Undelivered++;
			return false;
		}

		// A guest asks and waits for the grant. **The person does not wait** -
		// the edit is already applied at this machine; what waits is the
		// message.
		//
		// **Held first, and asked for second.** A request the link would not
		// take is a reason to ask again, not a reason to lose the edit: the
		// retry in `Pump` picks it up, and the whole point of the queue is that
		// nobody's work is thrown away.
		EditMessage request;
		request.Kind = EditFrame::Request;
		request.Subject = held.Subject;
		if (!Client->SendUser(EncodeMessage(request), nowSeconds)) {
			Tally.Undelivered++;
			// Asked for again a retry from now rather than immediately, so a
			// link that is briefly full is not hammered.
			held.AskedAtSeconds = nowSeconds;
		}

		Pending.push_back(std::move(held));
		Tally.Queued++;
		return true;
	}

	void EditStream::Grant(EditorId editor, const InstancePath &path, double nowSeconds) {
		if (Server == nullptr) {
			return;
		}

		if (editor == HOST_EDITOR) {
			// The host grants itself by sending what was waiting, with no
			// message in between.
			Flush(path, nowSeconds);
			return;
		}

		for (const auto &member : Members) {
			if (member.first != editor) {
				continue;
			}
			EditMessage granted;
			granted.Kind = EditFrame::Granted;
			granted.Subject = path;
			Server->SendTo(member.second, EncodeMessage(granted), nowSeconds);
			return;
		}
	}

	void EditStream::Flush(const InstancePath &granted, double nowSeconds) {
		// **Oldest first, and only what the grant covers.** An editor's own
		// edits must reach everybody else in the order they were made, whatever
		// order the turns come back in.
		for (size_t index = 0; index < Pending.size();) {
			if (!Contains(granted, Pending[index].Subject) && !Contains(Pending[index].Subject, granted)) {
				index++;
				continue;
			}

			const std::vector<std::byte> payload = Pending[index].Payload;
			const InstancePath subject = Pending[index].Subject;
			Pending.erase(Pending.begin() + static_cast<ptrdiff_t>(index));

			if (Server != nullptr) {
				Server->Broadcast(payload, nowSeconds);
				Tally.Sent++;
				for (const Waiting &woken : Holds.Release(subject, HOST_EDITOR, nowSeconds)) {
					Grant(woken.Holder, woken.Subject, nowSeconds);
				}
				PublishLocks(nowSeconds);
			} else if (Client != nullptr) {
				if (Client->SendUser(payload, nowSeconds)) {
					Tally.Sent++;
				} else {
					Tally.Undelivered++;
				}
			}
		}
	}

	void EditStream::Remember(EditorId editor, engine::replication::ClientId client) {
		for (auto &member : Members) {
			if (member.first == editor) {
				member.second = client;
				return;
			}
		}
		Members.emplace_back(editor, client);
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
		// as far as every turn is concerned, and the two take turns with
		// themselves.
		const EditorId sender = Server != nullptr ? from.Index + 1 : HOST_EDITOR;

		switch (message->Kind) {
		case EditFrame::Locks:
			// A guest taking the host's picture of whose turn it is. Never
			// consulted to decide anything - a decision two processes could
			// reach differently is a decision that will be.
			Holds.Adopt(message->Locks);
			return;

		case EditFrame::Welcome:
			// Which editor the host is calling this one. Without it a guest
			// could see that somebody has a turn and not whether that somebody
			// is itself.
			Me = message->Holder;
			return;

		case EditFrame::Granted:
			// The turn came round. Everything this editor was holding back for
			// that subtree goes now, oldest first.
			Flush(message->Subject, nowSeconds);
			return;

		case EditFrame::Hello: {
			if (Server == nullptr) {
				// A guest does not arbitrate. A hello arriving at one is a peer
				// that has the roles the wrong way round.
				Tally.Malformed++;
				return;
			}

			Remember(sender, from);

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

		case EditFrame::Request: {
			if (Server == nullptr) {
				Tally.Malformed++;
				return;
			}
			Remember(sender, from);

			// **Granted or queued, and never refused for being busy.** Somebody
			// asking for a subtree in use is not somebody doing anything wrong;
			// they are second, and second still gets a turn.
			const Turn turn = Holds.Request(message->Subject, sender, nowSeconds);
			if (turn == Turn::Granted) {
				Grant(sender, message->Subject, nowSeconds);
			}
			PublishLocks(nowSeconds);
			return;
		}

		case EditFrame::Release: {
			if (Server == nullptr) {
				Tally.Malformed++;
				return;
			}
			for (const Waiting &woken : Holds.Release(message->Subject, sender, nowSeconds)) {
				Grant(woken.Holder, woken.Subject, nowSeconds);
			}
			PublishLocks(nowSeconds);
			return;
		}

		case EditFrame::Waypoint:
			break;
		}

		Tally.Received++;
		Tally.Applied += ApplyEdits(*Log, *Worlds, message->Records);

		// **And then everything of this editor's that has not gone yet, on
		// top.** The waypoint just applied was ordered by the host *before*
		// them; without the replay this machine would sit on a value the host
		// has already superseded while everybody else moved on.
		for (const Held &waiting : Pending) {
			if (Overlaps(waiting.Subject, CommonRoot(message->Records))) {
				ApplyEdits(*Log, *Worlds, waiting.Records);
			}
		}

		if (Server != nullptr) {
			// **The relay, and the exception is the point.** Everybody else
			// hears it in the order this process applied it, which is what
			// makes a path resolve to the same instance in every editor;
			// sending it back to its author would have them apply their own
			// change twice.
			Server->Broadcast(payload, nowSeconds, from);
			Tally.Relayed++;

			// The turn is over the moment the edit has landed and gone out.
			// Whoever was next gets it now, and their edit lands on top.
			const InstancePath root = CommonRoot(message->Records);
			for (const Waiting &woken : Holds.Release(root, sender, nowSeconds)) {
				Grant(woken.Holder, woken.Subject, nowSeconds);
			}
			PublishLocks(nowSeconds);
		}
	}

	void EditStream::Pump(double nowSeconds) {
		PollingAt = nowSeconds;

		if (Server != nullptr) {
			// A grant whose guard has fired holds nobody up whether or not
			// anybody sweeps it - but sweeping is what hands the turn to
			// whoever was waiting behind an editor that died.
			const std::vector<Waiting> woken = Holds.Expire(nowSeconds);
			for (const Waiting &next : woken) {
				Grant(next.Holder, next.Subject, nowSeconds);
			}
			if (!woken.empty()) {
				PublishLocks(nowSeconds);
			}
		}

		// A grant that never arrived is asked for again rather than waited on
		// for ever. One lost datagram must not strand an edit.
		if (Client != nullptr && Client->Admitted()) {
			for (Held &held : Pending) {
				if (nowSeconds - held.AskedAtSeconds < RETRY_SECONDS) {
					continue;
				}
				EditMessage request;
				request.Kind = EditFrame::Request;
				request.Subject = held.Subject;
				if (Client->SendUser(EncodeMessage(request), nowSeconds)) {
					held.AskedAtSeconds = nowSeconds;
				}
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
