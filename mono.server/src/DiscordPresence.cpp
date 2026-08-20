// What Discord says this server is hosting.
//
// **Flags rather than a panel.** A headless server has no preferences page, so
// everything it reports comes from the `discord.*` table - a config file or a
// command line - and the wording below is what it says when nobody set any of
// it. `mono.studio` is the one program that configures this in an interface.
//
// A dedicated server in a datacentre will never find a Discord socket and that
// is fine: `discord::Link` treats a missing one as "try again later", at
// `TRACE`, forever. The case this is for is somebody hosting for friends off
// their own machine.

#include <chrono>
#include <server/Server.hpp>
#include <string>
#include <utility>

namespace server {

	namespace {
		// What a server with nothing configured would say, if it were switched
		// on. Off is still the default; this is only the wording.
		discord::Settings Defaults() {
			discord::Settings settings;
			settings.Details = "Hosting {game}";
			settings.State = "{players} of {capacity} players";

			// **A label with no link, deliberately.** Nothing here knows an
			// address a stranger could click - that depends on a port forward,
			// a domain and a joining flow this engine has not got - so setting
			// `discord.button-url` alone is enough to get a working button, and
			// setting neither gets no button rather than a broken one.
			settings.ButtonLabel = "Join this server";
			return settings;
		}

		constexpr const char *A_GAME = "a game";
	}

	discord::Facts Server::DiscordFacts() {
		const bool hidden = DiscordLink != nullptr && DiscordLink->Configured().HideNames;

		std::string game = "a placeholder world";
		if (!Settings.GamePath.empty()) {
			game = std::filesystem::path(Settings.GamePath).stem().string();
		}
		if (hidden) {
			game = A_GAME;
		}

		const size_t players = Replication ? Replication->Count() : 0;

		return discord::Facts{
			{"game", std::move(game)},
			{"players", std::to_string(players)},
			{"capacity", std::to_string(Settings.MaximumClients)},
			{"worlds", std::to_string(Worlds().Count())},
			{"port", std::to_string(ListeningOn().Port)},
		};
	}

	void Server::StartDiscord() {
		const discord::Settings settings = discord::SettingsFromFlags(Defaults());
		if (!discord::IsConfigured(settings)) {
			return;
		}

		DiscordStartedUnixSeconds =
			static_cast<int64_t>(std::chrono::duration_cast<std::chrono::seconds>(
									 std::chrono::system_clock::now().time_since_epoch()
			)
									 .count());
		DiscordLink = std::make_unique<discord::Link>(settings);
	}

	void Server::PumpDiscord(double nowSeconds) {
		if (DiscordLink == nullptr) {
			return;
		}

		const discord::Settings &settings = DiscordLink->Configured();
		const discord::Facts facts = DiscordFacts();

		discord::Activity activity;
		activity.Details = discord::Fill(settings.Details, facts, discord::TEXT_LIMIT);
		activity.State = discord::Fill(settings.State, facts, discord::TEXT_LIMIT);
		activity.LargeImage = discord::Clamp(settings.LargeImage, discord::IMAGE_KEY_LIMIT);
		activity.LargeText = discord::Clamp(settings.LargeText, discord::TEXT_LIMIT);
		activity.StartedUnixSeconds = settings.ShowElapsed ? DiscordStartedUnixSeconds : 0;

		// **A party, because this program is the one that actually has one.**
		// Discord draws "3 of 8" from these, which is the fact a person
		// deciding whether to join wants and the one a line of text says worse.
		if (Replication) {
			activity.PartyId = "atomic-" + std::to_string(ListeningOn().Port);
			activity.PartySize = static_cast<uint32_t>(Replication->Count());
			activity.PartyCapacity = static_cast<uint32_t>(Settings.MaximumClients);
		}

		if (!settings.ButtonLabel.empty() && !settings.ButtonUrl.empty()) {
			activity.Buttons.push_back(
				discord::Button{
					.Label = discord::Clamp(settings.ButtonLabel, discord::BUTTON_LABEL_LIMIT),
					.Url = discord::Clamp(settings.ButtonUrl, discord::BUTTON_URL_LIMIT),
				}
			);
		}

		DiscordLink->SetActivity(activity);
		DiscordLink->Pump(nowSeconds);
	}
}
