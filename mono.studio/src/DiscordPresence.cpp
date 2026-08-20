// What Discord says this editor is doing, and the page that decides it.
//
// **The templates live in `Prefs` and the facts are read off the live editor.**
// That split is the whole design: `discord::Fill` knows nothing about worlds,
// and this file knows nothing about sockets. What connects them is a list of
// five short strings rebuilt every pump.
//
// **The studio configures this in a panel and the other three programs
// configure it with flags.** `studio/Config.hpp` says why: an editor persists
// what somebody set in a document it owns, and a `discord.*` command-line
// switch that this page then contradicted would be worse than no switch. The
// client, the server and the origin have no page, so they read the same struct
// through `discord::SettingsFromFlags`.

#include <engine/ui/Metrics.hpp>
#include <engine/ui/Theme.hpp>

#include <chrono>
#include <cstdio>
#include <imgui.h>
#include <string>
#include <studio/Editor.hpp>
#include <utility>
#include <vector>

namespace studio {

	namespace {
		// What a fresh install reports, before anybody opens the page.
		//
		// **Here rather than in `discord::Settings`' member initialisers.** The
		// four programs say different things - this one edits, an origin
		// serves - so a default written in the module would be one of them
		// pretending to be all four.
		constexpr const char *DEFAULT_DETAILS = "Editing {place}";
		constexpr const char *DEFAULT_STATE = "{instances} instances in {world}";

		// What `{place}` says when the universe has never been saved.
		constexpr const char *UNSAVED = "an unsaved place";

		// What a name becomes with `HideNames` on.
		//@{
		constexpr const char *A_PLACE = "a project";
		constexpr const char *A_WORLD = "a world";
		//@}

		// Every token this program publishes, for the list beside each field.
		constexpr const char *TOKENS = "{place}  {world}  {instances}  {worlds}  {selection}";

		// One editable line, with its token list under it.
		//
		// **The text follows every keystroke and the answer does not.** The
		// preview reads `text`, so it has to be current; the link and the file
		// react to `true`, which arrives once when the field is left.
		//
		// @param label   What the field is called.
		// @param text    The template, edited in place.
		// @param hint    What the field says while it is empty.
		// @return Whether the edit has settled.
		bool TextRow(const char *label, std::string &text, const char *hint, size_t capacity) {
			// A fixed buffer, because that is what `InputText` takes and every
			// other text field in this editor does the same.
			std::vector<char> buffer(capacity, '\0');
			std::snprintf(buffer.data(), buffer.size(), "%s", text.c_str());

			ImGui::SetNextItemWidth(engine::ui::Scaled(360.0f));
			if (ImGui::InputTextWithHint(label, hint, buffer.data(), buffer.size())) {
				text = buffer.data();
			}
			return ImGui::IsItemDeactivatedAfterEdit();
		}

		// A line of text in the muted colour, for a note under a field.
		void Note(const char *text) {
			ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());
			ImGui::TextUnformatted(text);
			ImGui::PopStyleColor();
		}
	}

	discord::Facts Editor::DiscordFacts() {
		const bool hidden = Prefs.Discord.HideNames;

		std::string place = GamePath.empty() ? std::string(UNSAVED) : GamePath.stem().string();
		std::string world = ActiveWorldName();
		if (world.empty()) {
			world = "no world";
		} else if (hidden) {
			world = A_WORLD;
		}
		if (hidden) {
			place = A_PLACE;
		}

		// **Substituted here rather than at each call site**, which is what
		// makes `HideNames` one rule. A branch per token would be four places
		// for somebody's unannounced project to leak from.
		size_t instances = 0;
		size_t worlds = 0;
		if (Universe != nullptr) {
			worlds = Universe->Count();
			if (Active.IsValid() && !Universe->IsRemote(Active)) {
				instances = InstanceCountOf(Active);
			}
		}

		return discord::Facts{
			{"place", std::move(place)},
			{"world", std::move(world)},
			{"instances", std::to_string(instances)},
			{"worlds", std::to_string(worlds)},
			{"selection", std::to_string(Selection.size())},
		};
	}

	discord::Activity Editor::DiscordActivity() {
		const discord::Settings &settings = Prefs.Discord;
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

		return activity;
	}

	void Editor::PumpDiscord(double nowSeconds) {
		const bool wanted = discord::IsConfigured(Prefs.Discord);

		if (!wanted && DiscordLink == nullptr) {
			// The overwhelming case, and it is one comparison and one null
			// check. Nothing is allocated for an editor nobody configured this
			// for.
			return;
		}

		if (DiscordLink == nullptr) {
			// **The epoch second is read once, here.** Discord draws the
			// elapsed timer from it, and re-reading it per update would make
			// the timer jump whenever the system clock was corrected.
			DiscordStartedUnixSeconds =
				static_cast<int64_t>(std::chrono::duration_cast<std::chrono::seconds>(
										 std::chrono::system_clock::now().time_since_epoch()
				)
										 .count());
			DiscordLink = std::make_unique<discord::Link>(Prefs.Discord);
		} else if (DiscordSettled) {
			// **Only when the page says an edit settled**, never on any
			// difference: `Prefs.Discord` is also what the preview reads, so it
			// follows every keystroke, and a link that followed it too would
			// try a connection per character of a half-typed application id.
			//
			// `Configure` drops the socket only when the identity changed, so
			// rewording a template does not flicker somebody's profile.
			DiscordLink->Configure(Prefs.Discord);
		}
		DiscordSettled = false;

		DiscordLink->SetActivity(DiscordActivity());
		DiscordLink->Pump(nowSeconds);
	}

	void Editor::DrawDiscordSettings() {
		discord::Settings &settings = Prefs.Discord;
		// A checkbox settles the instant it is clicked; a text field settles
		// when it is left. See `Editor::DiscordSettled`.
		bool settled = false;

		// **The state line first, because it is what somebody came here for
		// when it is not working.** Everything below is what to type; this is
		// whether any of it arrived.
		ImGui::SeparatorText("Connection");
		if (DiscordLink == nullptr) {
			Note(discord::IsConfigured(settings) ? "off" : "off - no application id");
		} else {
			ImGui::TextUnformatted(discord::Describe(DiscordLink->State()));
			if (DiscordLink->State() == discord::LinkState::Ready) {
				ImGui::SameLine();
				ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());
				ImGui::Text("- %llu update(s) sent", static_cast<unsigned long long>(DiscordLink->Sent()));
				ImGui::PopStyleColor();
			}
			if (!DiscordLink->Trouble().empty()) {
				Note(DiscordLink->Trouble().c_str());
			}
		}

		ImGui::Spacing();
		ImGui::SeparatorText("Application");

		settled |= ImGui::Checkbox("Report what I am editing to Discord", &settings.Enabled);

		// **Said out loud rather than left to be discovered.** This publishes a
		// file name to a friends list, which is not what a checkbox in an
		// editor usually does.
		Note("Your friends see the place name, the world name and the counts below.");

		settled |= TextRow("Application ID", settings.ApplicationId, "empty reports nothing", 64);

		// **Said, because the field is filled in and it is not an id.** `-1` is
		// not a snowflake and never will be, which is what lets it mean "nobody
		// has set this" without a second switch saying so.
		if (settings.ApplicationId.empty() || settings.ApplicationId == discord::UNSET_APPLICATION_ID) {
			Note("Not set. Make an application at discord.com/developers and paste its ID here.");
			Note("Its name is the bold first line your friends see, so make it the game's name.");
		} else {
			Note("Reporting as this application. Its name is the bold first line on the card.");
		}

		// Not a credential, and it looks like one sitting in a file. An
		// Application ID ships inside every Discord game's binary, and the
		// *client secret* beside it in the portal is for OAuth and is never
		// used here.
		Note("An ID is a name rather than a secret. The client secret is not needed or asked for.");

		// Everything below reports as that application, so there is nothing to
		// set until there is one.
		ImGui::BeginDisabled(
			settings.ApplicationId.empty() || settings.ApplicationId == discord::UNSET_APPLICATION_ID
		);

		ImGui::Spacing();
		ImGui::SeparatorText("What it says");

		settled |= TextRow("First line", settings.Details, DEFAULT_DETAILS, 512);
		settled |= TextRow("Second line", settings.State, DEFAULT_STATE, 512);
		Note(TOKENS);

		settled |= ImGui::Checkbox("Show how long this session has been open", &settings.ShowElapsed);
		settled |= ImGui::Checkbox("Hide place and world names", &settings.HideNames);
		Note("Keeps the presence and drops the names, for an unannounced project.");

		ImGui::Spacing();
		ImGui::SeparatorText("Preview");

		// **The reason this page is worth building.** Discord does not draw a
		// person their own card, so this is the only way to see what a friend
		// sees without asking one.
		{
			const discord::Activity card = DiscordActivity();
			ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());
			ImGui::TextUnformatted("Atomic Game Engine");
			ImGui::PopStyleColor();
			ImGui::TextUnformatted(card.Details.empty() ? "(no first line)" : card.Details.c_str());
			ImGui::TextUnformatted(card.State.empty() ? "(no second line)" : card.State.c_str());
			if (card.StartedUnixSeconds > 0) {
				Note("00:00 elapsed");
			}
			for (const discord::Button &button : card.Buttons) {
				ImGui::TextUnformatted(button.Label.c_str());
			}
		}

		ImGui::Spacing();
		ImGui::SeparatorText("Button");

		settled |= TextRow("Label", settings.ButtonLabel, "empty for no button", 64);
		settled |= TextRow("Link", settings.ButtonUrl, "https://", 600);

		// The single most confusing thing about this feature, and the reason
		// somebody would conclude the whole page does nothing.
		Note("Discord shows buttons to everybody except you, so your own card looks empty.");

		ImGui::EndDisabled();

		if (settled) {
			// **Written when an edit settles rather than on every keystroke.**
			// Eighteen digits of application id is eighteen rewrites of
			// `preferences.json` otherwise, and `PumpDiscord` would open and
			// fail seventeen connections working through the prefixes.
			DiscordSettled = true;
			Prefs.Save();
		}
	}
}
