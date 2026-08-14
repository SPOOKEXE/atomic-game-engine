#include <engine/core/Flags.hpp>

#include <client/Settings.hpp>
#include <span>
#include <string>
#include <vector>

namespace client {
	namespace {
		// **Built from a default-constructed `Options`**, so the numbers here are
		// the member initialisers in `Client.hpp` and not a second copy of them.
		const engine::core::FlagTableBuilder &Table() {
			static const engine::core::FlagTableBuilder table = [] {
				const Options defaults;
				engine::core::FlagTableBuilder built;

				built.Integer("client.width", defaults.Width, "Window width in logical pixels");
				built.Integer("client.height", defaults.Height, "Window height in logical pixels");
				built.Integer("client.entities", defaults.Entities, "Cubes in the demo scene, per world");
				built.Integer("client.worlds", defaults.Worlds, "Worlds to simulate and composite");
				built.Number(
					"client.view-spacing", defaults.ViewSpacing, "World units between composited views"
				);
				built.Number("client.tick-rate", defaults.TickRate, "Simulation ticks per second");
				built.Integer(
					"client.surface-bounces",
					defaults.SurfaceBounces,
					"Levels of mirror-in-mirror resolved per frame, or 0 for the renderer's own default"
				);
				built.Integer(
					"client.max-fps",
					defaults.MaximumFrameRate,
					"Hold this frame rate. Needs client.uncapped; 0 is no limit"
				);
				built.Boolean("client.uncapped", defaults.Uncapped, "Present without waiting for vblank");

				built.Boolean(
					"client.show-statistics",
					defaults.ShowStatistics,
					"Open the F3 statistics panel at startup"
				);
				built.Boolean(
					"client.show-network",
					defaults.ShowNetwork,
					"Open the F4 network panel at startup. Ignored without a server to report on"
				);
				built.Boolean(
					"client.show-frame-graph", defaults.ShowFrameGraph, "Open the F5 frame graph at startup"
				);

				built.Text(
					"client.connect",
					defaults.ConnectAddress,
					"host:port of a server to replicate a world from"
				);
				built.Boolean(
					"client.browse",
					defaults.Browse,
					"Look for a session on this subnet instead of naming one"
				);
				built.Number(
					"client.browse-seconds", defaults.BrowseSeconds, "How long to look before giving up"
				);
				built.Text("client.session-name", defaults.SessionName, "Join the session with this name");
				built.Text(
					"client.session-id",
					defaults.SessionIdText,
					"Join the session with this id — 32 hex characters"
				);
				built.Text(
					"client.session-key",
					defaults.SessionSecret,
					"The secret for a private session: 64 hex characters, or a passphrase"
				);
				built.Text(
					"client.rendezvous",
					defaults.RendezvousAddress,
					"Reach a session through this rendezvous point"
				);
				built.Text(
					"client.server-key",
					defaults.ServerKey,
					"64 hex characters — the server identity to pin. Without it a relay in the path can read "
					"everything"
				);

				built.List(
					"client.content-sources",
					"A content origin. Repeat the key for more, in priority order. 'dir:PATH' selects a "
					"local store"
				);
				built.Text(
					"client.content-cache",
					defaults.ContentCache.string(),
					"Keep verified content here between runs"
				);
				built.Text(
					"client.publisher-key",
					defaults.ContentPublisherKey,
					"64 hex characters — the key whose manifests this client trusts"
				);
				built.Text(
					"client.assets-directory",
					defaults.AssetsDirectory.string(),
					"Read shaders and staged data from here instead of from beside the binary"
				);
				built.Text("client.sound", defaults.SoundPath.string(), "Play this .wav or .mp3 on a loop");

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
		if (!engine::core::Flags::Has("client.width")) {
			// Nothing declared them, so this is whatever `Options` says on its
			// own — the same direction `assets::ContentPolicy::FromFlags` takes
			// for a program that never registered a table.
			return options;
		}

		using engine::core::Flag;

		options.Width = static_cast<int>(Flag("client.width").Integer());
		options.Height = static_cast<int>(Flag("client.height").Integer());
		options.Entities = static_cast<uint32_t>(Flag("client.entities").Integer());
		options.Worlds = static_cast<uint32_t>(Flag("client.worlds").Integer());
		options.ViewSpacing = static_cast<float>(Flag("client.view-spacing").Number());
		options.TickRate = Flag("client.tick-rate").Number();
		options.SurfaceBounces = static_cast<int>(Flag("client.surface-bounces").Integer());
		options.MaximumFrameRate = static_cast<uint32_t>(Flag("client.max-fps").Integer());
		options.Uncapped = Flag("client.uncapped").Boolean();

		options.ShowStatistics = Flag("client.show-statistics").Boolean();
		options.ShowNetwork = Flag("client.show-network").Boolean();
		options.ShowFrameGraph = Flag("client.show-frame-graph").Boolean();

		options.ConnectAddress = std::string(Flag("client.connect").Text());
		options.Browse = Flag("client.browse").Boolean();
		options.BrowseSeconds = Flag("client.browse-seconds").Number();
		options.SessionName = std::string(Flag("client.session-name").Text());
		options.SessionIdText = std::string(Flag("client.session-id").Text());
		options.SessionSecret = std::string(Flag("client.session-key").Text());
		options.RendezvousAddress = std::string(Flag("client.rendezvous").Text());
		options.ServerKey = std::string(Flag("client.server-key").Text());

		const std::span<const std::string> sources = Flag("client.content-sources").Items();
		options.ContentSources.assign(sources.begin(), sources.end());
		options.ContentCache = std::filesystem::path(Flag("client.content-cache").Text());
		options.ContentPublisherKey = std::string(Flag("client.publisher-key").Text());
		options.AssetsDirectory = std::filesystem::path(Flag("client.assets-directory").Text());
		options.SoundPath = std::filesystem::path(Flag("client.sound").Text());

		return options;
	}
}
