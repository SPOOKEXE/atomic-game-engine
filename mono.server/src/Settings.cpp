#include <engine/core/Flags.hpp>
#include <engine/core/Log.hpp>

#include <server/Settings.hpp>
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
				built.Text(
					"server.identity-key",
					defaults.IdentityKey,
					"64 hex characters - the Ed25519 seed this server proves its identity with"
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
		options.Advertise = Flag("server.advertise").Boolean();
		options.SessionName = std::string(Flag("server.session-name").Text());
		options.SessionSecret = std::string(Flag("server.session-key").Text());
		options.RendezvousAddress = std::string(Flag("server.rendezvous").Text());
		options.IdentityKey = std::string(Flag("server.identity-key").Text());

		options.ControlPort = static_cast<int>(Flag("server.control-port").Integer());

		options.ContentStore = std::filesystem::path(Flag("server.content-store").Text());
		options.ContentPort = static_cast<uint16_t>(Flag("server.content-port").Integer());
		options.ContentGrantKey = std::string(Flag("server.content-grant-key").Text());

		options.WorldsPerHost = static_cast<uint32_t>(Flag("server.worlds-per-host").Integer());
		options.HostProgram = std::filesystem::path(Flag("server.host-program").Text());
		options.Processes = static_cast<uint32_t>(Flag("server.processes").Integer());
		options.AssetsDirectory = std::filesystem::path(Flag("server.assets-directory").Text());

		return options;
	}
}
