// What Discord says this client is playing.
//
// **Flags rather than a panel, unlike the studio.** This program has no
// preferences page to hang a toggle on, so the whole of what it reports comes
// from the `discord.*` table - a config file or a command line - and the
// wording below is what it says when nobody set any of it.

#include <engine/core/Clock.hpp>

#include <chrono>
#include <client/Client.hpp>
#include <string>
#include <utility>

namespace client {

	namespace {
		// What a client with nothing configured would say, if it were switched
		// on. Off is still the default; this is only the wording.
		discord::Settings Defaults() {
			discord::Settings settings;
			settings.Details = "Playing {game}";
			settings.State = "In {world}";
			return settings;
		}

		// What a name becomes with `HideNames` on.
		//@{
		constexpr const char *A_GAME = "a game";
		constexpr const char *A_WORLD = "a world";
		//@}
	}

	discord::Facts Client::DiscordFacts() const {
		const bool hidden = DiscordLink != nullptr && DiscordLink->Configured().HideNames;

		std::string game =
			Settings.GameFile.empty() ? std::string("the demo scene") : Settings.GameFile.stem().string();
		std::string world = "no world";
		if (Universe_ != nullptr) {
			const auto worlds = Universe_->Worlds();
			if (!worlds.empty()) {
				world = std::string(Universe_->NameOf(worlds.front()).Text());
			}
		}

		if (hidden) {
			game = A_GAME;
			world = A_WORLD;
		}

		// **`server` is where this client is, not who it is.** An address is
		// the one fact here somebody might not want on a friends list, and
		// `HideNames` deliberately does not cover it - a person who typed an
		// address into a template asked for it, and the template is the switch.
		std::string server =
			Settings.ConnectAddress.empty() ? std::string("single player") : Settings.ConnectAddress;

		return discord::Facts{
			{"game", std::move(game)},
			{"world", std::move(world)},
			{"server", std::move(server)},
			{"worlds", std::to_string(Universe_ != nullptr ? Universe_->Count() : static_cast<size_t>(0))},
		};
	}

	void Client::PumpDiscord(double nowSeconds) {
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

	void Client::StartDiscord() {
		const discord::Settings settings = discord::SettingsFromFlags(Defaults());
		if (!discord::IsConfigured(settings)) {
			// Nothing allocated and no socket opened for a program nobody
			// configured this for, which is every default install.
			return;
		}

		DiscordStartedUnixSeconds =
			static_cast<int64_t>(std::chrono::duration_cast<std::chrono::seconds>(
									 std::chrono::system_clock::now().time_since_epoch()
			)
									 .count());
		DiscordLink = std::make_unique<discord::Link>(settings);
	}
}
