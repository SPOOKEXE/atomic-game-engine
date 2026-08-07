// The assets manager: what is in the local content store, and how to add to it.
//
// **A view over `cdn::LocalStore` and nothing of its own.** The folder is the
// index — `LocalStore.hpp` says so — so this panel reads `raw/` to say what is
// actually there, reads the published manifest to say what an author can name,
// and calls `ImportFile` and `PublishLocal`. It keeps no list it cannot rebuild
// and has no opinion the store does not already have.
//
// **Every row is a file in `~/Documents/atomic-game-engine/cdn` and nowhere
// else.** That is a rule and not a default. This panel used to list the *log*,
// whose subjects are the paths files came from — `~/Music/...`, `~/art/...` —
// so most of what it showed was somewhere else entirely, and half of it was
// paths that no longer existed. The store is what the store contains. The log is
// still read, but only to *label* a row: `raw/` is hash-named, so nothing else
// can say what a file was called before it came in.
//
// ## Getting files in
//
// Three ways, and all three end at `ImportFile`:
//
// - **Drag and drop**, single or many. SDL delivers one `SDL_EVENT_DROP_FILE`
//   per path, so a multi-file drop arrives as several events and a dropped
//   folder arrives as one path that is walked.
// - **Add files…**, which browses and takes one file at a time.
// - **Add folder…**, which browses and takes everything under a directory.
//
// The typed-path field is gone. It was there because there was no portable file
// dialog — and there is one, `studio::FilePrompt`, which six other dialogs in
// this editor already use. Keeping a text field beside a browser would be two
// places to say the same thing.

#include <cdn/LocalStore.hpp>
#include <engine/core/Log.hpp>
#include <engine/ui/Metrics.hpp>
#include <engine/ui/Theme.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <imgui.h>
#include <studio/Assets.hpp>
#include <studio/Editor.hpp>
#include <studio/Widgets.hpp>

namespace studio {

	namespace {
		// Seconds since the Unix epoch, for the log.
		//
		// Read here and passed down, because `cdn::LocalStore` holds no notion of
		// "now" of its own to drift — `assets::Grant`'s standing rule.
		uint64_t NowSeconds() {
			return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
											 std::chrono::system_clock::now().time_since_epoch()
			)
											 .count());
		}

		// A byte count somebody can read at a glance.
		std::string Readable(uint64_t bytes) {
			static const char *UNITS[] = {"B", "KB", "MB", "GB"};

			double scaled = static_cast<double>(bytes);
			size_t unit = 0;
			while (scaled >= 1024.0 && unit + 1 < std::size(UNITS)) {
				scaled /= 1024.0;
				unit++;
			}

			char text[32];
			std::snprintf(text, sizeof(text), unit == 0 ? "%.0f %s" : "%.1f %s", scaled, UNITS[unit]);
			return text;
		}

		const char *KindName(engine::assets::AssetKind kind) {
			switch (kind) {
			case engine::assets::AssetKind::Mesh:
				return "mesh";
			case engine::assets::AssetKind::Texture:
				return "texture";
			case engine::assets::AssetKind::Audio:
				return "audio";
			case engine::assets::AssetKind::Material:
				return "material";
			case engine::assets::AssetKind::Font:
				return "font";
			case engine::assets::AssetKind::Script:
				return "script";
			case engine::assets::AssetKind::Unknown:
				break;
			}
			// **Named rather than blank**, because "unknown" is a thing a
			// publisher decided from an extension it did not recognise — and a
			// row that looked empty would read as a bug in this panel instead of
			// as a file nothing will claim.
			return "unknown";
		}

		constexpr const char *ADD_FILE = "Add a file";
		constexpr const char *ADD_FOLDER = "Add a folder";
	}

	void Editor::DrawAssets() {
		if (!ShowAssets) {
			return;
		}

		if (!ImGui::Begin("Assets", &ShowAssets)) {
			ImGui::End();
			return;
		}

		const cdn::LocalPaths paths = cdn::DefaultLocalPaths();

		// **Read when the panel opens and after anything changes it**, not every
		// frame: listing `raw/` stats every file and reading the manifest parses
		// one, and a store with the roadmap's own seed import is 289 files.
		if (ImGui::IsWindowAppearing()) {
			RefreshStoreContents();
		}

		ImGui::TextUnformatted(paths.Root.string().c_str());
		ImGui::SameLine();
		if (ImGui::SmallButton("Refresh")) {
			RefreshStoreContents();
		}

		ImGui::Separator();

		// --- getting things in ------------------------------------------------

		const ImVec2 button(engine::ui::Scaled(120.0f), 0.0f);

		if (ImGui::Button("Add files...", button)) {
			AssetBrowsePath = paths.Root.string();
			ImGui::OpenPopup(ADD_FILE);
		}

		ImGui::SameLine();
		if (ImGui::Button("Add folder...", button)) {
			AssetBrowsePath = paths.Root.string();
			ImGui::OpenPopup(ADD_FOLDER);
		}

		ImGui::SameLine();
		ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());
		ImGui::TextUnformatted("or drop files and folders onto the window");
		ImGui::PopStyleColor();

		// **No extension filter.** A content store takes whatever somebody has —
		// `assetc` decides what it can bake and `Publish` decides what kind a
		// name is, and a dialog that hid a format the pipeline would have handled
		// would be a third opinion about what content is.
		if (FilePrompt(ADD_FILE, AssetBrowsePath, "Import", {}, true)) {
			ImportAssetPath(AssetBrowsePath);
		}
		if (FolderPrompt(ADD_FOLDER, AssetBrowsePath, "Import all")) {
			ImportAssetPath(AssetBrowsePath);
		}

		if (!AssetStatus.empty()) {
			ImGui::TextWrapped("%s", AssetStatus.c_str());
		}

		ImGui::Separator();

		// --- publishing -------------------------------------------------------
		//
		// **The key is asked for and never stored**, which is `PublishLocal`'s own
		// rule surfacing: a manifest signs what a client will trust, and a key kept
		// in the editor's preferences file is a key that signs anything anybody
		// drops in the folder. The *ingest* key on the Content page is saved and
		// that is not an inconsistency — `ContentSources.hpp` carries why.
		ImGui::TextUnformatted("Publish raw/ into processed/");

		ImGui::SetNextItemWidth(-engine::ui::Scaled(130.0f));
		ImGui::InputTextWithHint(
			"##key",
			"64 hex characters of signing seed",
			AssetSigningKey,
			sizeof(AssetSigningKey),
			ImGuiInputTextFlags_Password
		);

		ImGui::SameLine();
		ImGui::BeginDisabled(AssetSigningKey[0] == '\0');
		if (ImGui::Button("Publish", button)) {
			PublishAssets(AssetSigningKey);
		}
		ImGui::EndDisabled();

		ImGui::Separator();

		// --- sending it somewhere ---------------------------------------------
		//
		// **Uploading and publishing are two acts and stay two.** What an
		// origin's inbox receives is unsigned content, and no client will look
		// at it until a publisher has signed a manifest naming it — CDN.md §1.
		// A single button doing both would need the signing seed to be around
		// for the upload, which is the one thing this panel refuses to keep.
		ImGui::BeginDisabled(ContentUploads == nullptr || ContentUploads->Remaining() > 0);
		if (ImGui::Button("Upload", button)) {
			UploadStore();
		}
		ImGui::EndDisabled();

		ImGui::SameLine();
		if (ContentUploads == nullptr) {
			ImGui::TextDisabled("no write source — Preferences > Content");
		} else if (ContentUploads->Remaining() > 0) {
			ImGui::Text("%zu left", ContentUploads->Remaining());
		} else {
			ImGui::TextDisabled(
				"%zu destination(s) — the Network panel says how it went",
				ContentUploads->Destinations().size()
			);
		}

		if (!ContentStatus.empty()) {
			ImGui::TextWrapped("%s", ContentStatus.c_str());
		}

		ImGui::Separator();

		// --- what is there ----------------------------------------------------
		//
		// Two tabs because they are two different questions. `raw/` answers "did
		// my file get in"; the manifest answers "what can I name in a scene", and
		// nothing is in the second until somebody publishes.
		if (ImGui::BeginTabBar("##store")) {
			if (ImGui::BeginTabItem("Published")) {
				DrawPublishedList();
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("Raw")) {
				DrawRawList();
				ImGui::EndTabItem();
			}
			ImGui::EndTabBar();
		}

		ImGui::End();
	}

	void Editor::DrawPreview(const std::string &name, float side) {
		const ImVec2 box(side, side);

		if (void *handle = ThumbnailFor(name); handle != nullptr) {
			ImGui::Image(reinterpret_cast<ImTextureID>(handle), box);
			return;
		}

		// **A box of the same size rather than nothing**, so a row does not
		// change height when its picture arrives — a list that reflows while it
		// loads is one nobody can click in.
		//
		// Whether this is "not yet" or "never" is deliberately not
		// distinguished: `Thumbnails.cpp` caches a failure as null, so asking
		// again is free, and a spinner on a mesh that will never have a preview
		// would be a promise the editor cannot keep.
		const ImVec2 corner = ImGui::GetCursorScreenPos();
		ImGui::Dummy(box);
		ImGui::GetWindowDrawList()->AddRect(
			corner,
			ImVec2(corner.x + side, corner.y + side),
			ImGui::GetColorU32(ImGuiCol_Border)
		);
	}

	void Editor::DrawPublishedList() {
		ImGui::Text("%zu asset(s)", PickerContents.size());

		if (PickerContents.empty()) {
			ImGui::TextDisabled("nothing published yet — import files above, then Publish");
			return;
		}

		ImGui::SetNextItemWidth(-1.0f);
		TextField("##published-filter", AssetFilter, "filter");

		if (!ImGui::BeginTable(
				"published",
				4,
				ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
					ImGuiTableFlags_Sortable | ImGuiTableFlags_SortMulti
			)) {
			return;
		}

		// **The preview column is explicitly not sortable.** Every other column
		// is a fact about the asset; this one is a picture, and a header that
		// looked clickable and did nothing is worse than one that plainly is
		// not.
		ImGui::TableSetupColumn(
			"", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort, engine::ui::Scaled(40.0f)
		);
		ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_DefaultSort);
		ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, engine::ui::Scaled(80.0f));
		ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, engine::ui::Scaled(90.0f));
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableHeadersRow();

		// Filtered first, so the sort below orders what is actually drawn.
		std::vector<const cdn::PublishedEntry *> shown;
		shown.reserve(PickerContents.size());
		for (const cdn::PublishedEntry &entry : PickerContents) {
			int score = 0;
			if (FuzzyMatch(AssetFilter, entry.Name, score)) {
				shown.push_back(&entry);
			}
		}

		// **The view is sorted and the store is not.** `PickerContents` is what
		// the manifest says, in the order it says it; a click on a header must
		// not reorder the thing every other panel reads.
		if (const ImGuiTableSortSpecs *specs = ImGui::TableGetSortSpecs()) {
			SortPublished(shown, specs);
		}

		ImGuiListClipper clipper;
		clipper.Begin(static_cast<int>(shown.size()));

		while (clipper.Step()) {
			for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
				const cdn::PublishedEntry &entry = *shown[static_cast<size_t>(row)];

				ImGui::TableNextRow();

				ImGui::TableNextColumn();

				// **Only the rows the clipper drew ask for a picture**, which is
				// the bound that makes previews affordable over a store of
				// hundreds — Thumbnails.cpp.
				DrawPreview(entry.Name, engine::ui::Scaled(32.0f));

				ImGui::TableNextColumn();

				// **The name is what a scene writes**, so it is the thing worth
				// copying — one click rather than reading it off and retyping.
				ImGui::AlignTextToFramePadding();
				ImGui::TextUnformatted(entry.Name.c_str());
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("%s\nclick to copy", entry.Name.c_str());
				}
				if (ImGui::IsItemClicked()) {
					ImGui::SetClipboardText(entry.Name.c_str());
				}

				ImGui::TableNextColumn();
				ImGui::TextUnformatted(KindName(entry.Kind));

				ImGui::TableNextColumn();
				const std::string hex = entry.Root.ToHex();
				ImGui::TextUnformatted(hex.substr(0, 8).c_str());
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("%s", hex.c_str());
				}
			}
		}

		ImGui::EndTable();
	}

	void Editor::SortPublished(
		std::vector<const cdn::PublishedEntry *> &rows, const ImGuiTableSortSpecs *specs
	) {
		if (specs == nullptr || specs->SpecsCount == 0) {
			return;
		}

		// **Stable, and the specs are walked backwards.** imgui lists the
		// columns most-significant first; sorting stably from the least
		// significant upward is what makes a multi-column sort mean "by kind,
		// then by name" rather than "by whichever was clicked last".
		for (int index = specs->SpecsCount - 1; index >= 0; index--) {
			const ImGuiTableColumnSortSpecs &spec = specs->Specs[index];
			const bool ascending = spec.SortDirection == ImGuiSortDirection_Ascending;

			std::stable_sort(
				rows.begin(),
				rows.end(),
				[&](const cdn::PublishedEntry *left, const cdn::PublishedEntry *right) {
					bool less = false;
					switch (spec.ColumnIndex) {
					case 1:
						less = left->Name < right->Name;
						break;
					case 2:
						// **By the name a person reads, not by the enum's
						// ordinal.** The column shows "mesh" and "texture", and
						// a sort that ordered by the numbers behind them would
						// look arbitrary to everybody but the compiler.
						less = std::string_view(KindName(left->Kind)) <
							   std::string_view(KindName(right->Kind));
						break;
					case 3:
						less = left->Root.ToHex() < right->Root.ToHex();
						break;
					default:
						return false;
					}
					return ascending ? less : !less && left != right;
				}
			);
		}
	}

	void Editor::DrawRawList() {
		uint64_t total = 0;
		for (const cdn::RawEntry &entry : PickerRaw) {
			total += entry.Bytes;
		}

		ImGui::Text("%zu file(s), %s", PickerRaw.size(), Readable(total).c_str());

		if (PickerRaw.empty()) {
			ImGui::TextDisabled("nothing imported yet — drop files on the window, or use the buttons above");
			return;
		}

		ImGui::SetNextItemWidth(-1.0f);
		TextField("##raw-filter", AssetFilter, "filter");

		if (!ImGui::BeginTable(
				"raw",
				4,
				ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
					ImGuiTableFlags_Sortable | ImGuiTableFlags_SortMulti
			)) {
			return;
		}

		ImGui::TableSetupColumn(
			"", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort, engine::ui::Scaled(40.0f)
		);
		ImGui::TableSetupColumn("Was called");
		ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, engine::ui::Scaled(80.0f));
		ImGui::TableSetupColumn("Stored as", ImGuiTableColumnFlags_WidthFixed, engine::ui::Scaled(90.0f));
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableHeadersRow();

		// **A clipper, because the roadmap's own seed import is 289 rows** and a
		// store somebody has used for a while is thousands. Drawing every row of
		// a scrolled table is the cost imgui's clipper exists to remove.
		//
		// The filter is applied first so the clipper counts what is actually
		// drawn — a clipper over the unfiltered list would leave gaps.
		std::vector<const cdn::RawEntry *> shown;
		shown.reserve(PickerRaw.size());
		for (const cdn::RawEntry &entry : PickerRaw) {
			int score = 0;
			if (FuzzyMatch(AssetFilter, entry.Original, score)) {
				shown.push_back(&entry);
			}
		}

		// **The default order is newest first, which no column can express**, so
		// the sort is applied only once somebody has clicked a header. Sorting
		// by "Was called" the moment the tab opens would bury what they just
		// imported.
		if (const ImGuiTableSortSpecs *specs = ImGui::TableGetSortSpecs()) {
			SortRaw(shown, specs);
		}

		ImGuiListClipper clipper;
		clipper.Begin(static_cast<int>(shown.size()));

		while (clipper.Step()) {
			for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
				const cdn::RawEntry &entry = *shown[static_cast<size_t>(row)];

				ImGui::TableNextRow();

				ImGui::TableNextColumn();

				// **The file name and not the original**, because that is what
				// this file is called under `raw/` — which is the path
				// `ThumbnailFor` resolves. The two differ: `Original` is what it
				// was called before it came in.
				DrawPreview(entry.Path.filename().string(), engine::ui::Scaled(32.0f));

				ImGui::TableNextColumn();
				ImGui::AlignTextToFramePadding();
				ImGui::TextUnformatted(entry.Original.c_str());

				ImGui::TableNextColumn();
				ImGui::TextUnformatted(Readable(entry.Bytes).c_str());

				ImGui::TableNextColumn();

				// The hash-named file this actually is, which is what somebody
				// looking in the folder with a terminal will see.
				const std::string stored = entry.Path.filename().string();
				ImGui::TextUnformatted(stored.substr(0, 8).c_str());
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("%s", entry.Path.string().c_str());
				}
			}
		}

		ImGui::EndTable();
	}

	void Editor::SortRaw(std::vector<const cdn::RawEntry *> &rows, const ImGuiTableSortSpecs *specs) {
		if (specs == nullptr || specs->SpecsCount == 0) {
			return;
		}

		// Least significant column first, stably — `SortPublished` carries why.
		for (int index = specs->SpecsCount - 1; index >= 0; index--) {
			const ImGuiTableColumnSortSpecs &spec = specs->Specs[index];
			const bool ascending = spec.SortDirection == ImGuiSortDirection_Ascending;

			std::stable_sort(
				rows.begin(),
				rows.end(),
				[&](const cdn::RawEntry *left, const cdn::RawEntry *right) {
					bool less = false;
					switch (spec.ColumnIndex) {
					case 1:
						less = left->Original < right->Original;
						break;
					case 2:
						// **Numerically, which is the whole reason this column
						// is sortable.** The cell reads "1.4 MB"; sorting the
						// text would put 900 B after 1.4 MB and read as broken.
						less = left->Bytes < right->Bytes;
						break;
					case 3:
						less = left->Path.filename() < right->Path.filename();
						break;
					default:
						return false;
					}
					return ascending ? less : !less && left != right;
				}
			);
		}
	}

	void Editor::RefreshStoreContents() {
		const cdn::LocalPaths paths = cdn::DefaultLocalPaths();
		PickerRaw = cdn::RawContents(paths);
		PickerContents = cdn::PublishedContents(paths);
	}

	void Editor::ImportAssetPath(const std::string &given) {
		const cdn::LocalPaths paths = cdn::DefaultLocalPaths();
		if (!cdn::EnsureLocalStore(paths)) {
			AssetStatus = "could not create the content store";
			return;
		}

		const std::filesystem::path source(given);
		std::error_code failure;

		size_t imported = 0;
		size_t duplicates = 0;
		size_t failures = 0;
		const uint64_t now = NowSeconds();

		const auto take = [&](const std::filesystem::path &file) {
			const auto report = cdn::ImportFile(paths, file, now);
			if (!report.has_value()) {
				failures++;
			} else if (report->Duplicate) {
				duplicates++;
			} else {
				imported++;
			}
		};

		if (std::filesystem::is_directory(source, failure)) {
			// **A folder is walked**, because that is what a person drags and
			// what "Add folder" is for. Skipping what cannot be read rather than
			// stopping, for `contentimport`'s reason: a real content directory
			// has a broken symlink in it eventually.
			for (std::filesystem::recursive_directory_iterator walk(
					 source, std::filesystem::directory_options::skip_permission_denied, failure
				 );
				 walk != std::filesystem::recursive_directory_iterator();
				 walk.increment(failure)) {
				if (failure) {
					break;
				}
				if (walk->is_regular_file(failure)) {
					take(walk->path());
				}
			}
		} else if (std::filesystem::is_regular_file(source, failure)) {
			take(source);
		} else {
			AssetStatus = "not a file or a folder: " + given;
			return;
		}

		AssetStatus = std::to_string(imported) + " imported, " + std::to_string(duplicates) +
					  " already there, " + std::to_string(failures) + " failed";
		ENGINE_INFO("assets: {}", AssetStatus);

		// The list on screen is now wrong, and this is the only place that knows
		// it changed.
		RefreshStoreContents();
	}

	void Editor::DropAssetPath(const std::string &path) {
		// **Opened rather than only imported.** A person dropping a file on a
		// closed panel gets no feedback at all otherwise — the import happens,
		// the status line updates, and nothing they can see says so.
		ShowAssets = true;
		ImportAssetPath(path);
	}

	void Editor::PublishAssets(const std::string &hexSeed) {
		if (hexSeed.size() != 64) {
			AssetStatus = "the signing key is 64 hex characters";
			return;
		}

		std::array<std::byte, 32> seed{};
		for (size_t index = 0; index < seed.size(); index++) {
			const std::string byte = hexSeed.substr(index * 2, 2);
			char *end = nullptr;
			const long value = std::strtol(byte.c_str(), &end, 16);
			if (end != byte.c_str() + 2) {
				AssetStatus = "the signing key is not hex";
				return;
			}
			seed[index] = static_cast<std::byte>(value);
		}

		const auto signing = engine::assets::SigningKey::FromSeed(seed);
		if (!signing.has_value()) {
			AssetStatus = "that is not a usable signing key";
			return;
		}

		const cdn::LocalPaths paths = cdn::DefaultLocalPaths();
		const auto report = cdn::PublishLocal(paths, *signing, NowSeconds());
		if (!report.has_value()) {
			AssetStatus = "the publish failed — see the output panel";
			return;
		}

		AssetStatus = std::to_string(report->Assets) + " asset(s) in " +
					  std::to_string(report->Bundles) + " bundle(s), root " +
					  report->Root.ToHex().substr(0, 8);
		ENGINE_INFO("assets: {}", AssetStatus);

		// **Refreshed here rather than left for the next open.** Publishing is
		// what fills the picker, and a picker that still said "nothing
		// published" straight after would read as the publish having failed.
		RefreshStoreContents();
	}
}
