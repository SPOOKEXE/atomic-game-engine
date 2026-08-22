#include <engine/core/Flags.hpp>
#include <engine/core/Log.hpp>

#include <algorithm>
#include <filesystem>
#include <optional>
#include <server/Settings.hpp>
#include <span>
#include <string>

namespace server {
	namespace {
		// **Built from a default-constructed `Options`**, so the numbers here
		// are the member initialisers in `Server.hpp` and not a second copy.
		const engine::core::FlagTableBuilder &Table() {
			static const engine::core::FlagTableBuilder table = [] {
				const Options defaults;
				engine::core::FlagTableBuilder built;

				built.Number("server.tick-rate", defaults.TickRate, "Ticks per second");
				built.Number(
					"server.physics-tick-rate",
					defaults.PhysicsTickRate,
					"Physics steps per second, or 0 to follow the tick rate"
				);
				built.Number(
					"server.replication-tick-rate",
					defaults.ReplicationTickRate,
					"Snapshots published per second, or 0 to publish every tick"
				);
				built.Integer("server.entities", defaults.Entities, "Entities in the placeholder world");
				built.Boolean(
					"server.unpaced", defaults.Unpaced, "Tick back to back instead of pacing to the tick rate"
				);
				built.Boolean(
					"server.chatter",
					defaults.Chatter,
					"Make every world publish on a shared topic, so a multi-process universe has traffic"
				);

				built.Boolean(
					"server.manage-world-lifetime",
					defaults.ManageWorldLifetime,
					"Suspend worlds nobody is in. Off is this program's behaviour before v0.13"
				);
				built.Text(
					"server.idle-sleep",
					"timeout",
					"timeout, never or immediate - what happens to a world that empties"
				);
				built.Number(
					"server.idle-close-seconds",
					defaults.IdleCloseSeconds,
					"How long a world may sit empty under the timeout mode"
				);

				built.Boolean(
					"server.listen", defaults.Listening, "Serve the primary world to clients over UDP"
				);
				built.Integer(
					"server.listen-port",
					defaults.ListenPort,
					"The UDP port to serve on. Zero binds an ephemeral one, which is a real answer"
				);
				built.Integer(
					"server.max-clients", defaults.MaximumClients, "The hard cap on connected clients"
				);
				built.Boolean(
					"server.advertise", defaults.Advertise, "Announce this server on the local subnet"
				);
				built.Text(
					"server.session-name",
					defaults.SessionName,
					"What to call this session in somebody's browser"
				);
				built.Text(
					"server.session-key",
					defaults.SessionSecret,
					"Make this session private: 64 hex characters, or a passphrase"
				);
				built.Text(
					"server.rendezvous",
					defaults.RendezvousAddress,
					"Register with this rendezvous point, so clients off this subnet can reach it"
				);
				built.Boolean(
					"server.quic",
					defaults.Quic,
					"Serve over QUIC rather than the datagram wire. Needs server.identity-key, and "
					"every client must be started with the same flag"
				);
				built.Text(
					"server.identity-key",
					defaults.IdentityKey,
					"64 hex characters - the Ed25519 seed this server proves its identity with"
				);

				built.Text(
					"server.profile-out",
					defaults.ProfilePath.string(),
					"Fold this run's frame graph into a .folded file for scripts/flamegraph.py"
				);
				built.Integer(
					"server.profile-window",
					static_cast<int64_t>(defaults.ProfileWindowTicks),
					"With server.profile-out, also snapshot every N ticks for flamegraph.py --average"
				);

				built.Integer(
					"server.control-port",
					defaults.ControlPort,
					"Listen for Model Context Protocol on 127.0.0.1:PORT, or -1 for off"
				);

				built.Text(
					"server.content-store",
					defaults.ContentStore.string(),
					"Serve this content store to clients"
				);
				built.Integer(
					"server.content-port", defaults.ContentPort, "Port the attached origin listens on"
				);
				built.Text(
					"server.content-grant-key",
					defaults.ContentGrantKey,
					"64 hex characters - the secret grants are issued and checked with"
				);
				built.Text(
					"server.content-mode",
					Describe(defaults.ContentDelivery),
					"relay or redirect - whether this server streams content to clients or names the "
					"origins they should fetch from"
				);
				// **Repeatable, and the order is the priority** - the same shape
				// `client.content-sources` has, because it means the same thing. A
				// repeated key appends and a source that outranks it replaces, so a
				// file naming three origins and a `--flag` naming one is one origin.
				built.List(
					"server.content-sources",
					"An origin to fetch content from, as dir:PATH or HOST:PORT, optionally NAME=. "
					"Repeatable; the order is the priority"
				);
				built.Text(
					"server.content-publisher-key",
					defaults.ContentPublisherKey,
					"64 hex characters - the publisher whose signature content must carry"
				);

				built.Integer(
					"server.worlds-per-host",
					defaults.WorldsPerHost,
					"Shared worlds per supervised host process"
				);
				built.Text("server.host-program", defaults.HostProgram.string(), "The program a host runs");
				built.Integer(
					"server.processes",
					defaults.Processes,
					"How many processes share this machine, or 0 to work it out"
				);
				built.Text(
					"server.assets-directory",
					defaults.AssetsDirectory.string(),
					"Read staged data from here instead of from beside the binary"
				);

				return built;
			}();
			return table;
		}
	}

	bool DeclareFlags() {
		return engine::core::Flags::Declare(Table().Rows());
	}

	Options OptionsFromFlags() {
		Options options;
		if (!engine::core::Flags::Has("server.tick-rate")) {
			return options;
		}

		using engine::core::Flag;

		options.TickRate = Flag("server.tick-rate").Number();
		options.PhysicsTickRate = Flag("server.physics-tick-rate").Number();
		options.ReplicationTickRate = Flag("server.replication-tick-rate").Number();
		options.Entities = static_cast<uint32_t>(Flag("server.entities").Integer());
		options.Unpaced = Flag("server.unpaced").Boolean();
		options.Chatter = Flag("server.chatter").Boolean();

		options.ManageWorldLifetime = Flag("server.manage-world-lifetime").Boolean();
		options.IdleCloseSeconds = Flag("server.idle-close-seconds").Number();
		if (const std::string_view mode = Flag("server.idle-sleep").Text(); mode == "never") {
			options.IdleSleepMode = engine::world::IdleSleep::Never;
		} else if (mode == "immediate") {
			options.IdleSleepMode = engine::world::IdleSleep::Immediate;
		} else if (mode == "timeout") {
			options.IdleSleepMode = engine::world::IdleSleep::Timeout;
		} else {
			// **Named rather than defaulted**, for `engine.log-level`'s reason: a
			// misspelling that silently means `timeout` is a 24/7 server that
			// suspends after five minutes and a deployment that thinks it said
			// otherwise.
			ENGINE_WARN("server.idle-sleep: '{}' is not timeout, never or immediate; using timeout", mode);
		}

		options.Listening = Flag("server.listen").Boolean();
		options.ListenPort = static_cast<uint16_t>(Flag("server.listen-port").Integer());
		options.MaximumClients = static_cast<uint32_t>(Flag("server.max-clients").Integer());
		options.Advertise = Flag("server.advertise").Boolean();
		options.SessionName = std::string(Flag("server.session-name").Text());
		options.SessionSecret = std::string(Flag("server.session-key").Text());
		options.RendezvousAddress = std::string(Flag("server.rendezvous").Text());
		options.Quic = Flag("server.quic").Boolean();
		options.IdentityKey = std::string(Flag("server.identity-key").Text());

		options.ProfilePath = std::filesystem::path(Flag("server.profile-out").Text());
		options.ProfileWindowTicks =
			static_cast<uint64_t>(std::max<int64_t>(0, Flag("server.profile-window").Integer()));

		options.ControlPort = static_cast<int>(Flag("server.control-port").Integer());

		options.ContentStore = std::filesystem::path(Flag("server.content-store").Text());
		options.ContentPort = static_cast<uint16_t>(Flag("server.content-port").Integer());
		options.ContentGrantKey = std::string(Flag("server.content-grant-key").Text());
		if (const std::optional<ContentMode> mode = ContentModeOf(Flag("server.content-mode").Text())) {
			options.ContentDelivery = *mode;
		} else {
			// **Named rather than defaulted**, the same position `server.idle-sleep`
			// takes: a misspelling that silently means `relay` is a deployment that
			// believes it redirected and is quietly paying for every byte.
			ENGINE_WARN(
				"server.content-mode: '{}' is not relay or redirect; using relay",
				Flag("server.content-mode").Text()
			);
		}
		// **The span is taken off a named flag rather than a temporary**, which
		// is `-Wdangling-reference` and is the warning that made `ci` uncompilable
		// twice inside v0.15 - see `RUNNING.md`. The items live in the table, not
		// in the handle, and the compiler cannot see that.
		const Flag configured("server.content-sources");
		const std::span<const std::string> sources = configured.Items();
		options.ContentSources.assign(sources.begin(), sources.end());
		options.ContentPublisherKey = std::string(Flag("server.content-publisher-key").Text());

		options.WorldsPerHost = static_cast<uint32_t>(Flag("server.worlds-per-host").Integer());
		options.HostProgram = std::filesystem::path(Flag("server.host-program").Text());
		options.Processes = static_cast<uint32_t>(Flag("server.processes").Integer());
		options.AssetsDirectory = std::filesystem::path(Flag("server.assets-directory").Text());

		return options;
	}
}
