#include <cdn/DiscordPresence.hpp>
#include <chrono>
#include <string>
#include <utility>

namespace cdn {

	namespace {
		// What an origin with nothing configured would say, if it were switched
		// on. Off is still the default; this is only the wording.
		//
		// **No `{store}` in the default lines.** An origin's store path is a
		// deployment detail and often a person's home directory, and this
		// publishes to a friends list. The token exists for somebody who wants
		// it; the default does not reach for it.
		discord::Settings Defaults() {
			discord::Settings settings;
			settings.Details = "Hosting an Atomic origin";
			settings.State = "{groups} groups served";
			return settings;
		}
	}

	DiscordPresence::DiscordPresence(discord::Settings settings, int64_t startedUnixSeconds)
		: Wire(std::move(settings)), Started(startedUnixSeconds) {}

	std::unique_ptr<DiscordPresence> DiscordPresence::Start() {
		const discord::Settings settings = discord::SettingsFromFlags(Defaults());
		if (!discord::IsConfigured(settings)) {
			return nullptr;
		}

		const auto started = static_cast<int64_t>(std::chrono::duration_cast<std::chrono::seconds>(
													  std::chrono::system_clock::now().time_since_epoch()
		)
													  .count());

		// Not `make_unique`: the constructor is private, which is what makes
		// `Start` the only way to get one and therefore the only place the
		// "configured?" question is asked.
		return std::unique_ptr<DiscordPresence>(new DiscordPresence(settings, started));
	}

	void DiscordPresence::Pump(uint16_t port, const ServiceCounters &counters, double nowSeconds) {
		const discord::Settings &settings = Wire.Configured();

		const discord::Facts facts{
			{"groups", std::to_string(counters.Bundles)},
			{"manifests", std::to_string(counters.Manifests)},
			{"bytes", std::to_string(counters.ServedBytes)},
			{"port", std::to_string(port)},
		};

		discord::Activity activity;
		activity.Details = discord::Fill(settings.Details, facts, discord::TEXT_LIMIT);
		activity.State = discord::Fill(settings.State, facts, discord::TEXT_LIMIT);
		activity.LargeImage = discord::Clamp(settings.LargeImage, discord::IMAGE_KEY_LIMIT);
		activity.LargeText = discord::Clamp(settings.LargeText, discord::TEXT_LIMIT);
		activity.StartedUnixSeconds = settings.ShowElapsed ? Started : 0;

		if (!settings.ButtonLabel.empty() && !settings.ButtonUrl.empty()) {
			activity.Buttons.push_back(
				discord::Button{
					.Label = discord::Clamp(settings.ButtonLabel, discord::BUTTON_LABEL_LIMIT),
					.Url = discord::Clamp(settings.ButtonUrl, discord::BUTTON_URL_LIMIT),
				}
			);
		}

		Wire.SetActivity(activity);
		Wire.Pump(nowSeconds);
	}
}
