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

		// The most names one path may have. A tree this deep is a tree nobody
		// authored.
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

	InstancePath PathOf(const Store &store, Entity instance) {
		if (instance == NULL_ENTITY || !store.Alive(instance)) {
			return {};
		}

		InstancePath path;
		for (Entity walk = instance; walk != NULL_ENTITY && store.Alive(walk); walk = store.ParentOf(walk)) {
			path.emplace_back(store.InstanceNameOf(walk).Text());
			if (path.size() > MAXIMUM_DEPTH) {
				// A cycle, or a tree past anything anybody authored. Refused
				// rather than truncated: half a path resolves somewhere, and
				// somewhere is worse than nowhere.
				return {};
			}
		}

		std::reverse(path.begin(), path.end());
		return path;
	}

	Entity ResolvePath(const Store &store, const InstancePath &path) {
		if (path.empty()) {
			return NULL_ENTITY;
		}

		Entity found = NULL_ENTITY;
		store.EachRoot([&](Entity root) {
			if (found == NULL_ENTITY && store.InstanceNameOf(root).Text() == path.front()) {
				found = root;
			}
		});

		for (size_t index = 1; index < path.size() && found != NULL_ENTITY; ++index) {
			Entity next = NULL_ENTITY;
			store.EachChild(found, [&](Entity child) {
				// **The first match wins.** Two siblings may share a name, so a
				// path is not a key — it is the best identity available without
				// the document format carrying one, and where it is ambiguous
				// the ambiguity was already there in what a person sees.
				if (next == NULL_ENTITY && store.InstanceNameOf(child).Text() == path[index]) {
					next = child;
				}
			});
			found = next;
		}

		return found;
	}

	std::vector<std::byte> EncodeEdits(std::span<const EditRecord> records) {
		engine::core::ByteWriter writer;
		writer.WriteUInt32(EDIT_MAGIC);
		writer.WriteUInt16(EDIT_VERSION);
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

		return {writer.Bytes().begin(), writer.Bytes().end()};
	}

	std::optional<std::vector<EditRecord>> DecodeEdits(std::span<const std::byte> bytes) {
		engine::core::ByteReader reader(bytes);
		if (reader.ReadUInt32() != EDIT_MAGIC || reader.ReadUInt16() != EDIT_VERSION) {
			return std::nullopt;
		}

		const uint32_t count = reader.ReadUInt32();
		if (reader.Failed() || count > MAXIMUM_RECORDS) {
			return std::nullopt;
		}

		std::vector<EditRecord> records;
		records.reserve(count);
		for (uint32_t index = 0; index < count; ++index) {
			EditRecord record;

			const uint8_t kind = reader.ReadUInt8();
			if (reader.Failed() || kind > static_cast<uint8_t>(CommandKind::Property)) {
				return std::nullopt;
			}
			record.Kind = static_cast<CommandKind>(kind);

			record.World = reader.ReadString();
			if (!ReadPath(reader, record.Subject) || !ReadPath(reader, record.OldParent) ||
				!ReadPath(reader, record.NewParent)) {
				return std::nullopt;
			}
			record.Document = reader.ReadString();
			record.Property = reader.ReadString();
			record.PropertyType = reader.ReadUInt8();
			record.Before = reader.ReadString();
			record.After = reader.ReadString();
			record.Description = reader.ReadString();

			if (reader.Failed()) {
				return std::nullopt;
			}
			records.push_back(std::move(record));
		}

		// Trailing bytes are a refusal. A frame whose fields ended before its
		// bytes did is one somebody appended to.
		if (reader.Remaining() != 0) {
			return std::nullopt;
		}
		return records;
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

	bool EditStream::Publish(std::span<const Command> commands, double nowSeconds) {
		if (commands.empty() || Worlds == nullptr || Log == nullptr) {
			return false;
		}

		const std::vector<EditRecord> records = DescribeEdits(*Log, *Worlds, commands);
		if (records.empty()) {
			return false;
		}

		const std::vector<std::byte> payload = EncodeEdits(records);

		if (Server != nullptr) {
			// Nobody excepted: this is the host's own edit and every guest has
			// to hear it.
			Server->Broadcast(payload, nowSeconds);
			Tally.Sent++;
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

	void EditStream::Receive(
		std::span<const std::byte> payload, double nowSeconds, engine::replication::ClientId from
	) {
		const std::optional<std::vector<EditRecord>> records = DecodeEdits(payload);
		if (!records) {
			// An editor somebody joined is an editor somebody can send anything
			// to. Counted and dropped.
			Tally.Malformed++;
			return;
		}

		Tally.Received++;
		Tally.Applied += ApplyEdits(*Log, *Worlds, *records);

		if (Server != nullptr) {
			// **The relay, and the exception is the point.** Everybody else
			// hears it in the order this process applied it, which is what
			// makes a path resolve to the same instance in every editor;
			// sending it back to its author would have them apply their own
			// change twice.
			Server->Broadcast(payload, nowSeconds, from);
			Tally.Relayed++;
		}
	}

	void EditStream::Pump(double nowSeconds) {
		PollingAt = nowSeconds;

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
			Client->Advance(nowSeconds);
		}
	}
}
