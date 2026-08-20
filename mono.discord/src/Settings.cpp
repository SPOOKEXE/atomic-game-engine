#include <engine/core/Flags.hpp>

#include <array>
#include <discord/Settings.hpp>
#include <string>

namespace discord {

	namespace {
		using engine::core::Flag;
		using engine::core::FlagDescription;
		using engine::core::FlagKind;
		using engine::core::FlagSource;

		// **Every text default here is empty, and that is not laziness.** The
		// templates differ per program - the studio says "Editing", the origin
		// says "Hosting" - so a default written here would be one of them
		// pretending to be all four. `SettingsFromFlags` keeps the caller's
		// value for anything nobody set, which is what makes one table serve
		// four programs without inventing a fifth set of words.
		constexpr std::array<FlagDescription, 11> TABLE{{
			{"discord.enabled",
			 FlagKind::Boolean,
			 "false",
			 "Tell Discord what this program is doing. Needs discord.app-id"},
			{"discord.app-id",
			 FlagKind::Text,
			 UNSET_APPLICATION_ID,
			 "The Discord Application ID to report as. -1 or empty reports nothing"},
			{"discord.details", FlagKind::Text, "", "The first line, as a template over {tokens}"},
			{"discord.state", FlagKind::Text, "", "The second line, as a template over {tokens}"},
			{"discord.large-image", FlagKind::Text, "", "An asset key uploaded to that application"},
			{"discord.large-text", FlagKind::Text, "", "The tooltip on that image"},
			{"discord.button-label", FlagKind::Text, "", "A button on the card. Empty for none"},
			{"discord.button-url", FlagKind::Text, "", "Where that button goes. https only"},
			{"discord.show-elapsed", FlagKind::Boolean, "true", "Draw the elapsed timer"},
			{"discord.hide-names",
			 FlagKind::Boolean,
			 "false",
			 "Replace place, world, game and store names with a generic word"},
			{"discord.join-secrets",
			 FlagKind::Boolean,
			 "false",
			 "Advertise a join secret. Needs a URI handler nothing registers yet"},
		}};

		// Whether somebody actually set this flag, as opposed to it reading its
		// declared default.
		//
		// **This is what lets one table carry four programs' wording.** Every
		// text row above declares empty, so without this a program's built-in
		// template would be overwritten by nothing at all on every start.
		bool Given(std::string_view name) {
			return Flag(name).Source() != FlagSource::Default;
		}

		// The flag's text when somebody set it, and `fallback` otherwise.
		std::string Chosen(std::string_view name, const std::string &fallback) {
			return Given(name) ? std::string(Flag(name).Text()) : fallback;
		}

		bool Chosen(std::string_view name, bool fallback) {
			return Given(name) ? Flag(name).Boolean() : fallback;
		}
	}

	bool DeclareFlags() {
		return engine::core::Flags::Declare(TABLE);
	}

	Settings SettingsFromFlags(const Settings &defaults) {
		Settings settings = defaults;

		// Nothing declared these, so this program does not use them - the same
		// direction `engine::parallel::ApplyFlags` takes, and for the same
		// reason: a dead flag reads `false`, which here would happen to be
		// right and for the wrong reason.
		if (!engine::core::Flags::Has("discord.enabled")) {
			return settings;
		}

		settings.Enabled = Chosen("discord.enabled", settings.Enabled);
		settings.ApplicationId = Chosen("discord.app-id", settings.ApplicationId);
		settings.Details = Chosen("discord.details", settings.Details);
		settings.State = Chosen("discord.state", settings.State);
		settings.LargeImage = Chosen("discord.large-image", settings.LargeImage);
		settings.LargeText = Chosen("discord.large-text", settings.LargeText);
		settings.ButtonLabel = Chosen("discord.button-label", settings.ButtonLabel);
		settings.ButtonUrl = Chosen("discord.button-url", settings.ButtonUrl);
		settings.ShowElapsed = Chosen("discord.show-elapsed", settings.ShowElapsed);
		settings.HideNames = Chosen("discord.hide-names", settings.HideNames);
		settings.JoinSecrets = Chosen("discord.join-secrets", settings.JoinSecrets);
		return settings;
	}

	bool IsConfigured(const Settings &settings) {
		return settings.Enabled && !settings.ApplicationId.empty() &&
			   settings.ApplicationId != UNSET_APPLICATION_ID;
	}
}
