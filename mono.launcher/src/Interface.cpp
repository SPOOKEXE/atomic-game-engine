// The launcher's screens.
//
// **Deliberately the thin half.** Everything a test could get wrong - which
// options exist, how they group, what the search leaves on screen, what a `+`
// or a multi-folder pick does to the rows, what argv comes out - is in
// `Plan.cpp` and has a suite. What is here is imgui calls over those answers,
// and it is kept that way on purpose: a form generated from `--describe` is a
// form nobody can read the source of and predict, so the part that decides has
// to be the part that can be asserted about.
//
// **The line is the argument list.** A function of a `Mode`, a `Description` or
// a `Form` belongs in `Plan.hpp`. What stays here needs a live `ImGuiStyle` or
// a live id stack to mean anything - `NameColumnWidth` and
// `ActionsColumnWidth` measure text in the current font, `ForceHeaderState` and
// `TabLabel` are about imgui's own state, and `DrawBrowseDialogs` exists for
// where on the id stack a popup is opened. Six answers had drifted across that
// line by v0.19 and moved back; `launcher/AGENTS.md` records which.

#include <engine/ui/Fields.hpp>
#include <engine/ui/Icons.hpp>
#include <engine/ui/Metrics.hpp>
#include <engine/ui/Prompts.hpp>
#include <engine/ui/Theme.hpp>

#include <algorithm>
#include <cfloat>
#include <imgui.h>
#include <launcher/Launcher.hpp>
#include <launcher/Programs.hpp>

namespace launcher {

	namespace {
		using engine::ui::Scaled;

		// The dialog ids. One of each, shared by every row - see
		// `Launcher::BrowseKind`.
		constexpr const char *FILE_DIALOG = "Choose a file";
		constexpr const char *FOLDER_DIALOG = "Choose a folder";
		constexpr const char *FOLDERS_DIALOG = "Choose folders";

		// A one-line description under a heading, in the muted colour.
		void Muted(const char *text) {
			ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());
			ImGui::TextWrapped("%s", text);
			ImGui::PopStyleColor();
		}

		// The description as a tooltip, which is where forty of them fit.
		void Explain(const std::string &description) {
			if (description.empty() || !ImGui::IsItemHovered()) {
				return;
			}
			ImGui::SetTooltip("%s", description.c_str());
		}

		// Whether this frame should override a collapsing header's own state.
		//
		// **`SetNextItemOpen(false, ImGuiCond_Always)` is a header that cannot
		// be opened.** The click registers and the header opens, and then the
		// next frame forces it shut again - which reads as a dropdown that
		// closes the instant you touch it, with no hint that anything is
		// forcing it. That was this program's first bug.
		//
		// So the override happens only while there is a search to serve, plus
		// once on the frame the search is cleared so the groups tidy themselves
		// away. Every other frame the header keeps whatever state somebody put
		// it in.
		void ForceHeaderState(bool searching, bool wasSearching) {
			if (searching) {
				ImGui::SetNextItemOpen(true, ImGuiCond_Always);
			} else if (wasSearching) {
				ImGui::SetNextItemOpen(false, ImGuiCond_Always);
			}
		}

		// The full window, with no title bar or dock host - the launcher is one
		// panel and always has been.
		void BeginFullWindow(const char *id) {
			const ImGuiViewport *main = ImGui::GetMainViewport();
			ImGui::SetNextWindowPos(main->WorkPos);
			ImGui::SetNextWindowSize(main->WorkSize);
			ImGui::Begin(
				id,
				nullptr,
				ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
					ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus |
					ImGuiWindowFlags_NoSavedSettings
			);
		}

		// The three-column table every option row is drawn in.
		//
		// **A table rather than `SameLine` at a fixed offset**, which is what
		// this was: `--override-assets-directory` is thirty characters and ran
		// straight through the column its own value field started in. A table
		// sizes the name column to the longest name in the group instead, and
		// the person can drag it if they disagree.
		bool BeginRowTable(const char *id) {
			return ImGui::BeginTable(
				id,
				3,
				ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Resizable |
					ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_PadOuterX
			);
		}

		// How wide the name column has to be for the longest name in this
		// group, checkbox included.
		//
		// **Measured rather than given a fraction of the window.** A stretched
		// name column is a third of eleven hundred pixels whether the longest
		// name is `game` or `override-assets-directory`, which on the Common tab
		// left a canyon between every name and its own value field. Fitting the
		// content keeps the pair together and still lets somebody drag the
		// divider if they disagree.
		float NameColumnWidth(const std::vector<std::string> &names) {
			float widest = 0.0f;
			for (const std::string &name : names) {
				widest = std::max(widest, ImGui::CalcTextSize(name.c_str()).x);
			}

			// The checkbox, the gap after it, and a little air before the
			// divider. Floored so a group of short names does not produce a
			// column narrower than the header it sits under.
			const float furniture = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x * 2.0f;
			return std::max(Scaled(150.0f), widest + furniture);
		}

		// A tab's label with its hit count, and an id that does not move with it.
		//
		// **The `###` is load-bearing.** imgui derives a tab's id from its
		// label, so a label that changes as somebody types is a *different tab*
		// every keystroke - the selection resets and the page flickers back to
		// Common. Everything after `###` is the id and nothing before it is.
		std::string TabLabel(const char *name, const char *id, bool searching, size_t hits) {
			std::string label(name);
			if (searching) {
				label += " (" + std::to_string(hits) + ")";
			}
			label += "###";
			label += id;
			return label;
		}

		// How wide the buttons at the end of a row need to be.
		//
		// **Measured rather than a round number.** It was `Scaled(100)`, which
		// is enough for `+` and `-` at scale 1 on a wide window and is not
		// enough for a browse button beside them, or for either at `--scale
		// 1.5` - and what a too-narrow fixed column does is clip the last
		// button off the edge rather than wrap it, so the row loses `-`
		// silently.
		float ActionsColumnWidth(bool browse) {
			const ImGuiStyle &style = ImGui::GetStyle();

			// Two `SmallButton`s, which take frame padding on x only.
			const float small = ImGui::CalcTextSize("+").x + style.FramePadding.x * 2.0f;
			float width = small * 2.0f + style.ItemSpacing.x;

			if (browse) {
				// `ui::FolderButton` is a square of the line height plus frame
				// padding on both axes.
				width += ImGui::GetTextLineHeight() + style.FramePadding.x * 2.0f + style.ItemSpacing.x;
			}

			return width + style.CellPadding.x * 2.0f;
		}

		void SetUpRowColumns(float nameWidth, float actionsWidth) {
			ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthFixed, nameWidth);
			ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 1.0f);
			ImGui::TableSetupColumn("actions", ImGuiTableColumnFlags_WidthFixed, actionsWidth);
		}
	}

	void Launcher::DrawModes() {
		BeginFullWindow("##launcher");

		ImGui::PushFont(nullptr, Scaled(24.0f));
		ImGui::TextUnformatted("atomic");
		ImGui::PopFont();
		Muted("Pick what to run. Everything each program accepts is on its page.");
		ImGui::Separator();
		ImGui::Dummy(ImVec2(0.0f, Scaled(8.0f)));

		for (const Mode &mode : Catalogue) {
			const Description *description = Programs.Find(mode.Program);
			const bool available = description != nullptr;

			ImGui::PushID(mode.Id.c_str());
			ImGui::BeginDisabled(!available);

			if (ImGui::Button(mode.Label.c_str(), ImVec2(Scaled(180.0f), Scaled(38.0f)))) {
				Open(mode);
			}

			ImGui::EndDisabled();
			ImGui::SameLine();

			ImGui::BeginGroup();
			if (available) {
				Muted(mode.Blurb.c_str());

				// **The version, on every row.** A launcher beside a
				// half-rebuilt tree is the failure this program could otherwise
				// hide: two versions here means somebody built one target and
				// not the others, and the run that follows would be a mystery.
				ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());
				ImGui::Text("%s %s", mode.Program.c_str(), description->Version.c_str());
				ImGui::PopStyleColor();
			} else {
				ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::WarningColour());
				ImGui::TextWrapped("%s", Programs.Failure(mode.Program).c_str());
				ImGui::PopStyleColor();
			}
			ImGui::EndGroup();

			ImGui::PopID();
			ImGui::Dummy(ImVec2(0.0f, Scaled(6.0f)));
		}

		ImGui::End();
	}

	void Launcher::DrawOption(const DescribedOption &option, size_t row) {
		const auto form = Forms.find(Open_);
		if (form == Forms.end() || row >= form->second.Options.size()) {
			return;
		}
		FieldState &field = form->second.Options[row];

		ImGui::TableNextRow();
		ImGui::PushID(static_cast<int>(row));

		ImGui::TableSetColumnIndex(0);
		ImGui::Checkbox("##on", &field.Enabled);
		Explain(option.Description);
		ImGui::SameLine();
		ImGui::TextUnformatted(option.Name.c_str());
		Explain(option.Description);

		if (!option.TakesValue) {
			ImGui::PopID();
			return;
		}

		ImGui::TableSetColumnIndex(1);
		ImGui::SetNextItemWidth(-FLT_MIN);
		if (engine::ui::TextField("##value", field.Value, option.ValueName.c_str())) {
			// Typing into a row turns it on. A value somebody typed that did
			// not reach the command line is the one behaviour a form like this
			// must not have.
			field.Enabled = true;
		}

		ImGui::TableSetColumnIndex(2);

		const BrowseShape shape = BrowseShapeOf(option);
		const bool folder = shape == BrowseShape::Folder;

		if (shape != BrowseShape::None) {
			// **A folder and a page rather than the word "Browse".** The button
			// used to say the same thing on both, so which dialog it opened was
			// something you found out by pressing it. `ui::Icons` draws these
			// rather than setting them in a font - see that header for why
			// there is no font to set them in.
			const bool clicked = folder ? engine::ui::FolderButton("##browse", "Choose a folder, or several")
										: engine::ui::FileButton("##browse", "Choose a file");
			if (clicked) {
				BrowsePath = field.Value;
				BrowseRow = row;
				BrowseKind = folder ? Browsing::Folders : Browsing::File;

				// Recorded, not opened. See `Launcher::BrowseRequested` for
				// why opening from in here reaches a popup id that nothing
				// begins.
				BrowseRequested = true;
			}
			ImGui::SameLine();
		}

		// **`+` on every value option rather than only the repeatable ones.**
		// Which options repeat is not in the declaration - it is a sentence in
		// the description - and guessing from prose would be wrong for exactly
		// the option nobody tested. An extra row on an option that does not
		// repeat is refused by the child's own parser with a message that says
		// so, which is a better failure than a launcher that could not express
		// `--cdn` twice.
		if (ImGui::SmallButton("+")) {
			AddRow(form->second, row);
		}
		Explain("Give this option again with another value");

		// **`-` beside it, and it took a bug to notice it was missing.** `+`
		// could add rows and nothing could take one away, so a mistyped repeat
		// was permanent for the life of the window - and a multi-folder pick
		// adds three at a time. On the last row it clears rather than removes;
		// `RemoveRow` carries the reason.
		ImGui::SameLine();
		if (ImGui::SmallButton("-")) {
			RemoveRow(form->second, row);
		}
		Explain(RowsFor(form->second, option.Name) > 1 ? "Remove this row" : "Clear this option");

		ImGui::PopID();
	}

	void Launcher::DrawRowsOf(const DescribedOption &option) {
		const auto form = Forms.find(Open_);
		if (form == Forms.end()) {
			return;
		}

		// **The row count is re-read every step**, because `+`, `-` and a
		// multi-folder confirm all change it from inside `DrawOption`. A loop
		// that had cached `size()` would run off the end on the frame a row was
		// removed, and index a moved element on the frame one was added.
		for (size_t row = 0; row < form->second.Options.size(); row++) {
			if (form->second.Options[row].Option == option.Name) {
				DrawOption(option, row);
			}
		}
	}

	void Launcher::DrawCommonTab(const Mode &mode, const Description &description) {
		Muted("What this mode is. Everything else is on the next tab.");

		// **Filtered like the others, since the search says "every option".** It
		// did not use to be, and the result was a search box that visibly
		// changed two tabs and left the one you were looking at alone - which
		// reads as the search being broken rather than as this tab being
		// special. The counts on the tab labels are the other half: an empty
		// Common now says where the matches went instead of just being empty.
		const std::vector<std::string> names = MatchingOptions(description, mode.Pinned, Query);

		if (names.empty()) {
			Muted("Nothing here matches. Try the other tabs.");
			return;
		}

		if (!BeginRowTable("##common")) {
			return;
		}
		SetUpRowColumns(NameColumnWidth(names), ActionsColumnWidth(AnyBrowses(description, names)));

		for (const std::string &name : names) {
			DrawRowsOf(*description.Option(name));
		}

		ImGui::EndTable();
	}

	void Launcher::DrawAllTab(const Description &description) {
		const bool searching = !Query.empty();
		const bool wasSearching = WasSearching;

		for (const OptionGroup &group : GroupOptions(description)) {
			// A search opens the groups that have a hit, because a match hidden
			// inside a collapsed header reads as no match at all.
			const std::vector<std::string> hits = MatchingOptions(description, group.Options, Query);
			if (hits.empty()) {
				continue;
			}

			// **The count is in the header.** A collapsed group says nothing
			// about how much is inside it, so the choice to open one was a
			// guess - and with a search running, the count is the answer to
			// "where did my match go".
			const std::string heading = group.Title + " (" + std::to_string(hits.size()) + ")";

			ForceHeaderState(searching, wasSearching);
			if (!ImGui::CollapsingHeader(heading.c_str())) {
				continue;
			}

			if (!BeginRowTable(group.Title.c_str())) {
				continue;
			}
			// **Sized for the whole group rather than for the matches**, so that
			// the name column does not resize under the pointer with every
			// keystroke of a search.
			SetUpRowColumns(
				NameColumnWidth(group.Options), ActionsColumnWidth(AnyBrowses(description, group.Options))
			);

			for (const std::string &name : hits) {
				DrawRowsOf(*description.Option(name));
			}

			ImGui::EndTable();
		}
	}

	void Launcher::DrawSettingsTab(const Description &description) {
		const auto form = Forms.find(Open_);
		if (form == Forms.end()) {
			return;
		}

		Muted(
			"Engine settings, written as --flag NAME=VALUE - the source that outranks a config "
			"file and the environment."
		);

		const bool searching = !Query.empty();
		const bool wasSearching = WasSearching;

		for (const OptionGroup &group : GroupSettings(description)) {
			const std::vector<std::string> hits = MatchingSettings(description, group.Options, Query);
			if (hits.empty()) {
				continue;
			}

			const std::string heading = group.Title + " (" + std::to_string(hits.size()) + ")";

			ForceHeaderState(searching, wasSearching);
			if (!ImGui::CollapsingHeader(heading.c_str())) {
				continue;
			}

			if (!BeginRowTable(("##settings" + group.Title).c_str())) {
				continue;
			}
			// **The settings tab's third column holds a word, not buttons.** A
			// setting is a value rather than a path this launcher knows the
			// shape of, so there is nothing to browse for and nothing to
			// repeat - what goes there is the declared kind, and the column has
			// to be wide enough for the longest of them rather than for two
			// small buttons.
			SetUpRowColumns(
				NameColumnWidth(group.Options),
				ImGui::CalcTextSize("integer").x + ImGui::GetStyle().CellPadding.x * 2.0f
			);

			// **Walked in form order rather than in `hits` order**, because the
			// row index is the imgui id and the form is what `CommandLine`
			// reads - drawing the same rows in a second order would be a second
			// answer to "which row is this".
			for (size_t row = 0; row < form->second.Settings.size(); row++) {
				SettingState &state = form->second.Settings[row];

				if (std::find(hits.begin(), hits.end(), state.Name) == hits.end()) {
					continue;
				}

				const DescribedSetting *declared = description.Setting(state.Name);
				if (declared == nullptr) {
					continue;
				}

				ImGui::TableNextRow();
				ImGui::PushID(static_cast<int>(row) + 10000);

				ImGui::TableSetColumnIndex(0);
				ImGui::Checkbox("##on", &state.Enabled);
				Explain(declared->Description);
				ImGui::SameLine();
				ImGui::TextUnformatted(state.Name.c_str());
				Explain(declared->Description);

				ImGui::TableSetColumnIndex(1);
				ImGui::SetNextItemWidth(-FLT_MIN);
				if (IsBooleanSetting(*declared)) {
					bool on = state.Value == "true";
					if (ImGui::Checkbox("##value", &on)) {
						state.Value = on ? "true" : "false";
						state.Enabled = true;
					}
				} else if (engine::ui::TextField("##value", state.Value, declared->Default.c_str())) {
					state.Enabled = true;
				}

				ImGui::TableSetColumnIndex(2);
				ImGui::TextDisabled("%s", declared->Kind.c_str());

				ImGui::PopID();
			}

			ImGui::EndTable();
		}
	}

	void Launcher::DrawBrowseDialogs() {
		const auto form = Forms.find(Open_);
		if (form == Forms.end()) {
			return;
		}

		// **The open happens here, at the same id-stack level as the begins
		// below.** A row's button only records which dialog it wants.
		if (BrowseRequested) {
			BrowseRequested = false;
			switch (BrowseKind) {
			case Browsing::File:
				ImGui::OpenPopup(FILE_DIALOG);
				break;
			case Browsing::Folder:
				ImGui::OpenPopup(FOLDER_DIALOG);
				break;
			case Browsing::Folders:
				ImGui::OpenPopup(FOLDERS_DIALOG);
				break;
			case Browsing::None:
				break;
			}
		}

		if (engine::ui::FilePrompt(FILE_DIALOG, BrowsePath, "Choose", {}, false)) {
			SetRows(form->second, BrowseRow, {BrowsePath});
		}

		if (engine::ui::FolderPrompt(FOLDER_DIALOG, BrowsePath, "Choose")) {
			SetRows(form->second, BrowseRow, {BrowsePath});
		}

		// **One row per folder.** Three ticked folders fill the row that was
		// browsed from and add two below it, so a repeatable option like
		// `--cdn dir:PATH` is answered in one trip rather than three.
		std::vector<std::string> folders;
		if (engine::ui::FoldersPrompt(FOLDERS_DIALOG, BrowsePath, folders, "Choose")) {
			SetRows(form->second, BrowseRow, folders);
		}
	}

	void Launcher::DrawForm() {
		// **Read and committed before anything can return early.** The two
		// values decide whether a header's state is overridden this frame, and
		// a `WasSearching` updated at the bottom of a function with three early
		// returns is one that goes stale on exactly the paths nobody tests.
		const bool searching = !Query.empty();
		WasSearching = searching;

		const Mode *mode = FindMode(Catalogue, Open_);
		const Description *description = mode == nullptr ? nullptr : Programs.Find(mode->Program);

		BeginFullWindow("##launcher");

		// **`IsWindowFocused` is the public way to ask "is a modal up".** A
		// browse dialog takes focus away from this window, and its own Escape
		// closes it - so without this check one press would close the dialog
		// *and* leave the mode, which is the sort of thing that loses a form
		// somebody spent a minute on. `IsAnyItemActive` covers the other half:
		// Escape inside a text field means "undo my typing".
		const bool ours = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
		const bool back = ImGui::Button("< Back", ImVec2(Scaled(90.0f), 0.0f)) ||
						  (ours && ImGui::IsKeyPressed(ImGuiKey_Escape) && !ImGui::IsAnyItemActive());
		if (back) {
			Open_.clear();
			ImGui::End();
			return;
		}
		ImGui::SameLine();

		ImGui::PushFont(nullptr, Scaled(20.0f));
		ImGui::TextUnformatted(mode == nullptr ? "?" : mode->Label.c_str());
		ImGui::PopFont();

		if (mode == nullptr || description == nullptr) {
			ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::ErrorColour());
			ImGui::TextWrapped("%s", Failure.c_str());
			ImGui::PopStyleColor();
			ImGui::End();
			return;
		}

		ImGui::SameLine();
		ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());
		ImGui::Text("%s %s", mode->Program.c_str(), description->Version.c_str());
		ImGui::PopStyleColor();

		if (Child.State() != ChildState::Idle) {
			ImGui::Separator();
			DrawSupervision();
		}

		ImGui::Separator();

		// **The search is above the tabs and filters both of them**, so typing
		// a word and reading the tab labels answers "which of these two hundred
		// rows is it in" without opening either.
		ImGui::SetNextItemWidth(-Scaled(84.0f));
		engine::ui::TextField("##search", Query, "search every option and setting");
		ImGui::SameLine();
		ImGui::BeginDisabled(!searching);
		if (ImGui::Button("Clear", ImVec2(Scaled(76.0f), 0.0f))) {
			Query.clear();
		}
		ImGui::EndDisabled();

		const auto form = Forms.find(Open_);
		if (form == Forms.end()) {
			ImGui::End();
			return;
		}

		const float footer = Scaled(112.0f);
		if (ImGui::BeginChild("##options", ImVec2(0.0f, -footer))) {
			if (ImGui::BeginTabBar("##tabs", ImGuiTabBarFlags_None)) {
				// **The hit count is on the tab rather than only inside it**,
				// because a search that emptied the page you were looking at,
				// with no sign of where its matches went, reads as a broken
				// search rather than as a wrong tab. Each count is the same
				// function the tab's own rows come from.
				const size_t pinned = MatchingOptions(*description, mode->Pinned, Query).size();
				const std::string common = TabLabel("Common", "common", searching, pinned);
				const std::string all =
					TabLabel("All options", "all", searching, OptionHits(*description, Query));
				const std::string settings =
					TabLabel("Engine settings", "settings", searching, SettingHits(*description, Query));

				if (ImGui::BeginTabItem(common.c_str())) {
					DrawCommonTab(*mode, *description);
					ImGui::EndTabItem();
				}
				if (ImGui::BeginTabItem(all.c_str())) {
					DrawAllTab(*description);
					ImGui::EndTabItem();
				}
				if (!description->Settings.empty() && ImGui::BeginTabItem(settings.c_str())) {
					DrawSettingsTab(*description);
					ImGui::EndTabItem();
				}
				ImGui::EndTabBar();
			}
		}
		ImGui::EndChild();

		// Outside the scrolling child, or a modal would be clipped to a region
		// it is meant to cover.
		DrawBrowseDialogs();

		DrawLaunch();
		ImGui::End();
	}

	void Launcher::DrawSupervision() {
		const char *state = "";
		unsigned int colour = engine::ui::MutedColour();

		switch (Child.State()) {
		case ChildState::Running:
			state = "running";
			colour = engine::ui::AccentColour();
			break;
		case ChildState::Ended:
			state = "finished";
			break;
		case ChildState::Failed:
			state = "failed";
			colour = engine::ui::ErrorColour();
			break;
		case ChildState::Idle:
			return;
		}

		ImGui::PushStyleColor(ImGuiCol_Text, colour);
		ImGui::Text("%s - %s, %.0fs", state, Child.Summary().c_str(), Child.Seconds());
		ImGui::PopStyleColor();

		if (Child.Running()) {
			if (ImGui::Button("Stop", ImVec2(Scaled(100.0f), 0.0f))) {
				Child.RequestStop();
			}
			ImGui::SameLine();
			if (ImGui::Button("Kill", ImVec2(Scaled(100.0f), 0.0f))) {
				Child.Kill();
			}
			return;
		}

		if (ImGui::Button("Restart", ImVec2(Scaled(100.0f), 0.0f))) {
			(void)Child.Restart(Failure);
		}
		ImGui::SameLine();
		if (ImGui::Button("Dismiss", ImVec2(Scaled(100.0f), 0.0f))) {
			Child.Clear();
		}
	}

	void Launcher::DrawLaunch() {
		const Mode *mode = FindMode(Catalogue, Open_);
		const Description *description = mode == nullptr ? nullptr : Programs.Find(mode->Program);
		const auto form = Forms.find(Open_);
		if (mode == nullptr || description == nullptr || form == Forms.end()) {
			return;
		}

		const std::filesystem::path program = ProgramPath(Stage, mode->Program);
		const std::string line = DisplayCommandLine(program, form->second, *description);

		ImGui::Separator();

		// **Shown, every time, in full.** A launcher that hides what it ran is a
		// launcher whose bug reports say "it did not work" - so this is the line
		// that reproduces the run by hand, and it is the same list `Launch`
		// passes to `parallel::Process`.
		ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());
		ImGui::TextWrapped("%s", line.c_str());
		ImGui::PopStyleColor();

		if (!Failure.empty()) {
			ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::ErrorColour());
			ImGui::TextWrapped("%s", Failure.c_str());
			ImGui::PopStyleColor();
		}

		if (ImGui::Button("Copy", ImVec2(Scaled(90.0f), Scaled(30.0f)))) {
			ImGui::SetClipboardText(line.c_str());
		}
		ImGui::SameLine();

		ImGui::BeginDisabled(Child.Running());

		// **Ctrl+Enter rather than Enter.** Half this page is text fields, and
		// Enter in one of them is how somebody finishes typing a port number -
		// binding a bare Enter to Launch would start a server every time.
		const bool shortcut = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
							  ImGui::IsKeyPressed(ImGuiKey_Enter) && ImGui::GetIO().KeyCtrl;
		if (ImGui::Button("Launch", ImVec2(Scaled(140.0f), Scaled(30.0f))) || shortcut) {
			Launch();
		}
		ImGui::EndDisabled();

		ImGui::SameLine();
		ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());
		ImGui::TextUnformatted("Ctrl+Enter to launch, Esc to go back");
		ImGui::PopStyleColor();
	}
}
