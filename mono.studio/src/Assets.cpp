// The assets manager: what is in the local content store, and how to add to it.
//
// **A view over `cdn::LocalStore` and nothing of its own.** The folder is the
// index - `LocalStore.hpp` says so - so this panel reads `raw/` to say what is
// actually there, reads the published manifest to say what an author can name,
// and calls `ImportFile` and `PublishLocal`. It keeps no list it cannot rebuild
// and has no opinion the store does not already have.
//
// **Every row is a file in `~/Documents/atomic-game-engine/cdn` and nowhere
// else.** That is a rule and not a default. This panel used to list the *log*,
// whose subjects are the paths files came from - `~/Music/...`, `~/art/...` -
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
// dialog - and there is one, `ui::FilePrompt`, which six other dialogs in
// this editor already use. Keeping a text field beside a browser would be two
// places to say the same thing.

#include <engine/assets/AssetKind.hpp>
#include <engine/assets/Mesh.hpp>
#include <engine/assets/Texture.hpp>
#include <engine/core/Bytes.hpp>
#include <engine/core/Log.hpp>
#include <engine/ui/Metrics.hpp>
#include <engine/ui/Prompts.hpp>
#include <engine/ui/Theme.hpp>

#include <algorithm>
#include <array>
#include <assetc/Bake.hpp>
#include <cdn/LocalStore.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <imgui.h>
#include <iterator>
#include <studio/Assets.hpp>
#include <studio/Editor.hpp>
#include <studio/Preview.hpp>
#include <studio/Widgets.hpp>
#include <vector>

namespace studio {

	namespace {
		// Seconds since the Unix epoch, for the log.
		//
		// Read here and passed down, because `cdn::LocalStore` holds no notion of
		// "now" of its own to drift - `assets::Grant`'s standing rule.
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

		// **`assets::Describe` names a kind, and this panel does not get to have
		// its own opinion.** There was a copy of that switch here, and it had
		// fallen three kinds behind: `Video`, `Data` and `Shader` all reached
		// the default and drew as "unknown", which is the exact reading the
		// original comment said it existed to prevent - a row that looks like a
		// bug in this panel rather than a file nothing will claim.
		//
		// The names are identical for every kind both spellings knew, so this
		// changes no row that was already right and fixes the three that were
		// not. `assets` is where the enum lives and where the next kind will be
		// added, which is the only place a name for it can be kept in step.
		const char *KindName(engine::assets::AssetKind kind) {
			return engine::assets::Describe(kind);
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

		// **Held rather than rebuilt.** `path::string()` allocates, and the
		// store's root is fixed for the run - so drawing one line of text was a
		// heap allocation a frame. The path itself is cheap to derive and is
		// wanted below as a path.
		if (AssetRootText.empty()) {
			AssetRootText = paths.Root.string();
		}
		ImGui::TextUnformatted(AssetRootText.c_str());
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

		// **No extension filter.** A content store takes whatever somebody has -
		// `assetc` decides what it can bake and `Publish` decides what kind a
		// name is, and a dialog that hid a format the pipeline would have handled
		// would be a third opinion about what content is.
		if (engine::ui::FilePrompt(ADD_FILE, AssetBrowsePath, "Import", {}, true)) {
			ImportAssetPath(AssetBrowsePath);
		}
		if (engine::ui::FolderPrompt(ADD_FOLDER, AssetBrowsePath, "Import all")) {
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
		// that is not an inconsistency - `ContentSources.hpp` carries why.
		ImGui::TextUnformatted("Publish baked/ into processed/");

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
		// at it until a publisher has signed a manifest naming it - CDN.md §1.
		// A single button doing both would need the signing seed to be around
		// for the upload, which is the one thing this panel refuses to keep.
		ImGui::BeginDisabled(ContentUploads == nullptr || ContentUploads->Remaining() > 0);
		if (ImGui::Button("Upload", button)) {
			UploadStore();
		}
		ImGui::EndDisabled();

		ImGui::SameLine();
		if (ContentUploads == nullptr) {
			ImGui::TextDisabled("no write source - Preferences > Content");
		} else if (ContentUploads->Remaining() > 0) {
			ImGui::Text("%zu left", ContentUploads->Remaining());
		} else {
			ImGui::TextDisabled(
				"%zu destination(s) - the Network panel says how it went",
				ContentUploads->Destinations().size()
			);
		}

		if (!ContentStatus.empty()) {
			ImGui::TextWrapped("%s", ContentStatus.c_str());
		}

		ImGui::Separator();

		// --- what is there ----------------------------------------------------
		//
		// **One tab per place, then the two questions that are about this
		// machine.** A published list on its own could not say *where* a name
		// lives, which is the question with two origins configured - so the
		// catalogue tabs come first, in priority order, with the merged view in
		// front of them and the engine's own beside it. `raw/` still answers
		// "did my file get in" and the gallery answers "what is actually used",
		// and neither is a place content can be fetched from.
		if (ImGui::BeginTabBar("##store")) {
			// **The ids are built with the tabs, not with the frame.** They are a
			// function of the catalogue alone, and building them here made a
			// string per tab per frame - past the small-string limit for most
			// titles, so a heap allocation each.
			if (AssetTabLabels.size() != AssetTabs.size() || AssetTabLabelsRevision != AssetTabsRevision) {
				AssetTabLabels.clear();
				AssetTabLabels.reserve(AssetTabs.size());
				for (size_t index = 0; index < AssetTabs.size(); index++) {
					// **The id is pinned to the position and the label is not.**
					// Two origins may be called the same thing - nothing stops
					// somebody naming two rows "cdn" - and two tabs sharing an
					// id is one tab that flickers between two contents.
					AssetTabLabels.push_back(
						AssetTabs[index].Title + "###asset-place-" + std::to_string(index)
					);
				}
				AssetTabLabelsRevision = AssetTabsRevision;
			}

			for (size_t index = 0; index < AssetTabs.size(); index++) {
				const CatalogueTab &tab = AssetTabs[index];

				if (ImGui::BeginTabItem(AssetTabLabels[index].c_str())) {
					DrawCatalogueList(tab);
					ImGui::EndTabItem();
				}
			}

			if (ImGui::BeginTabItem("Raw")) {
				DrawRawList();
				ImGui::EndTabItem();
			}

			// **A third question, and the only panel that can answer it.** The
			// other two tabs say what the *store* holds; this says what the
			// worlds actually use, which is what somebody needs before changing
			// or deleting an asset.
			if (ImGui::BeginTabItem("Used")) {
				DrawGallery();
				ImGui::EndTabItem();
			}
			ImGui::EndTabBar();
		}

		ImGui::End();
	}

	void Editor::DrawPreview(const std::string &name, float side, engine::assets::AssetKind kind) {
		// **The space is reserved with an item and the picture is painted into
		// it**, which is the split `PaintPreview` exists for: a list row paints
		// over a selectable it must not add a second hit target to, and a panel
		// that draws a preview on its own still needs the layout to know it is
		// there. One painter, two ways in - `studio/AssetRow.hpp` carries why
		// the row cannot use this one.
		const ImVec2 corner = ImGui::GetCursorScreenPos();
		ImGui::Dummy(ImVec2(side, side));
		PaintPreview(corner.x, corner.y, side, name, kind);
	}

	void Editor::PaintPreview(
		float cornerX, float cornerY, float side, const std::string &name, engine::assets::AssetKind kind
	) {
		ImDrawList *const draw = ImGui::GetWindowDrawList();
		const ImVec2 corner(cornerX, cornerY);
		const ImVec2 far(cornerX + side, cornerY + side);

		// **A mesh has no bitmap and gets the live slot instead.** A picture of a
		// mesh is a render - `Thumbnails.cpp` opens with why there is no cached
		// one - so a mesh row resolves to `Unavailable` and would draw an `M`
		// forever. The hovered row is the one the preview slot is already
		// rendering for the panel beside the cursor, and drawing that same
		// texture here costs nothing: it is one image, already made, this frame.
		//
		// So exactly one mesh row is alive at a time and it is the one being
		// pointed at, which is the row somebody wants to see.
		if (PreviewIsRendered(kind) && !name.empty() && name == PreviewShowing) {
			if (void *const slot = Renderer.SceneTexture(PreviewSlot()); slot != nullptr) {
				const engine::render::SceneExtent extent = Renderer.SceneTextureExtent(PreviewSlot());

				// **Sampled to its extent rather than whole.** The target is
				// rounded up to a block and the render fills the corner, so
				// drawing all of it would show the unwritten border down two
				// edges - the same correction the viewport panel and the hover
				// panel both make.
				draw->AddImage(
					reinterpret_cast<ImTextureID>(slot),
					corner,
					far,
					ImVec2(0.0f, 0.0f),
					ImVec2(extent.U, extent.V)
				);
				return;
			}
		}

		PreviewState state = PreviewState::Pending;
		void *const handle = ThumbnailFor(name, state);

		if (handle != nullptr) {
			draw->AddImage(reinterpret_cast<ImTextureID>(handle), corner, far);
			return;
		}

		// **Nothing to draw and its picture is a render: ask for one.** Until
		// v0.12 only the cursor asked, so a store full of materials drew a grid
		// of dashes until somebody swept the mouse across it. Recorded here
		// rather than requested here because there is one slot and this runs
		// per row - `PumpRenderedPreviews` takes one of these after the whole
		// list has been drawn.
		if (PreviewIsRendered(kind) && !name.empty() &&
			std::find(PreviewQueue.begin(), PreviewQueue.end(), name) == PreviewQueue.end()) {
			PreviewQueue.push_back(name);
		}

		// **A box of the same size rather than nothing**, so a row does not
		// change height when its picture arrives - a list that reflows while it
		// loads is one nobody can click in.
		draw->AddRect(corner, far, ImGui::GetColorU32(ImGuiCol_Border));

		// **A mark rather than a sentence, at this size.** Thirty-two pixels
		// holds no words; the hover preview is where the reason is spelled out -
		// which is the split `explorer-plus` makes too, its rows carrying icons
		// and its hover panel carrying "Preview unavailable".
		//
		// A cross for refused, a dash for nothing to show, and nothing at all
		// while it is still pending: a mark that appeared and was replaced two
		// frames later reads as a flicker.
		const char *mark = state == PreviewState::TooLarge			  ? "!"
						   : state == PreviewState::Pending			  ? nullptr
						   : kind == engine::assets::AssetKind::Mesh  ? "M"
						   : kind == engine::assets::AssetKind::Audio ? "S"
																	  : "-";
		if (mark != nullptr) {
			const ImVec2 text = ImGui::CalcTextSize(mark);
			draw->AddText(
				ImVec2(corner.x + (side - text.x) * 0.5f, corner.y + (side - text.y) * 0.5f),
				ImGui::GetColorU32(ImGuiCol_TextDisabled),
				mark
			);
		}
	}

	bool Editor::BakeRawAsset(const std::string &relative, std::string &baked) {
		const cdn::LocalPaths paths = cdn::DefaultLocalPaths();

		// **One source, through the whole baker.** `assetc::Settings::Only`
		// filters the walk rather than skipping it, so a material picked here
		// gets its colour map rewritten through `BakedName` exactly as a
		// whole-tree bake would - two spellings of that rule is a material that
		// resolves to nothing on a machine nobody tested.
		assetc::Settings settings;
		settings.Input = paths.Raw;
		settings.Output = paths.Baked;
		settings.Only = relative;

		// **A unit box, matching `contentimport` exactly.** The two bake into the
		// same store and a mesh has to behave the same whichever produced it -
		// one baked at authored scale would draw at a different size and, worse,
		// be culled against a `Bounds` ten times too small. `contentimport`
		// carries the whole argument.
		settings.ModelSize = 1.0f;

		// **The log, for the same reason `contentimport` passes it**: `raw/` is
		// flat, so a model's `tex/skin.png` cannot be followed through the folder
		// and only the import record still knows where the two came from. A bake
		// without this produces a mesh whose sheets nothing can fetch.
		settings.ResolveTexture = cdn::StoreTextureResolver(paths);

		std::string failure;
		const assetc::Report report = assetc::Bake(settings, failure);
		if (!failure.empty() || report.Assets.empty()) {
			AssetStatus = failure.empty() ? "nothing to bake" : failure;
			ENGINE_WARN("assets: bake {}: {}", relative, AssetStatus);
			return false;
		}

		const assetc::Baked &one = report.Assets.front();
		if (!one.Failure.empty()) {
			AssetStatus = one.Failure;
			ENGINE_WARN("assets: bake {}: {}", relative, one.Failure);
			return false;
		}

		baked = one.Output;

		// **Registered into this editor's own renderer immediately.** The point
		// of baking on demand is that the thing appears now; waiting for a
		// publish would make the picker's whole reason for existing a four-minute
		// round trip. What a publish is still needed for is a *client* - nothing
		// outside this process can fetch an asset no manifest names, and the tab
		// says so.
		RegisterBakedAsset(paths.Baked / baked, baked);

		AssetStatus = "baked " + baked + " - publish to share it";
		ENGINE_INFO("assets: {}", AssetStatus);

		// The picker lists the manifest, and this file is not in one yet. Its
		// row appears under Published after the next publish.
		return true;
	}

	bool Editor::LoadRawAsset(const std::filesystem::path &folder, const std::string &relative) {
		// **The tree is the truth here, so no resolver is passed.** A model in a
		// raw folder still sits beside its `tex/` directory - the flattening
		// that made `cdn::StoreTextureResolver` necessary is what `ImportFile`
		// does, and nothing has imported this.
		assetc::Settings settings;
		settings.Input = folder;
		settings.Only = relative;

		// **A unit box, matching `contentimport` and `BakeRawAsset` exactly.**
		// The three feed one renderer and a mesh has to be the same size
		// whichever produced it - one baked at authored scale would draw
		// differently and be culled against the wrong bounds.
		settings.ModelSize = 1.0f;

		// **Empty output is the memory-only case**, which is the default and
		// the reason this exists: looking at somebody's art folder must not
		// write a baked copy of it anywhere.
		const cdn::LocalPaths paths = cdn::DefaultLocalPaths();
		if (!Content.MemoryOnly) {
			settings.Output = paths.Baked;
		}

		std::string failure;
		const assetc::Report report = assetc::Bake(settings, failure);
		if (!failure.empty() || report.Assets.empty()) {
			AssetStatus = failure.empty() ? "nothing to bake" : failure;
			ENGINE_WARN("assets: raw bake {}: {}", relative, AssetStatus);
			return false;
		}

		const assetc::Baked &one = report.Assets.front();
		if (!one.Failure.empty()) {
			AssetStatus = one.Failure;
			ENGINE_WARN("assets: raw bake {}: {}", relative, one.Failure);
			return false;
		}

		if (Content.MemoryOnly) {
			RegisterBakedAsset(one.Payload, one.Output);
			AssetStatus = "loaded " + one.Output + " - in this editor only, nothing was written";
		} else {
			RegisterBakedAsset(paths.Baked / one.Output, one.Output);
			AssetStatus = "baked " + one.Output + " into the store - publish to share it";
		}

		ENGINE_INFO("assets: {}", AssetStatus);
		return true;
	}

	void Editor::RegisterBakedAsset(const std::filesystem::path &path, const std::string &name) {
		std::ifstream file(path, std::ios::binary);
		if (!file) {
			return;
		}
		const std::vector<char> raw((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
		if (raw.empty()) {
			return;
		}

		RegisterBakedAsset({reinterpret_cast<const std::byte *>(raw.data()), raw.size()}, name);
	}

	void Editor::RegisterBakedAsset(std::span<const std::byte> bytes, const std::string &name) {
		if (bytes.empty()) {
			return;
		}

		engine::core::ByteReader reader(bytes);
		const engine::core::Name interned(name);

		// **Only the two kinds this editor can show.** A sound or a script baked
		// on demand is a real thing to have done and there is nothing here to
		// hand it to - the publish is what delivers those, and pretending
		// otherwise would be a status line that lied.
		if (engine::assets::KindOfName(name) == engine::assets::AssetKind::Texture) {
			engine::assets::TextureData image;
			if (engine::assets::Texture::Read(reader, image)) {
				Renderer.AddTexture(interned, image);
			}
			return;
		}

		if (engine::assets::KindOfName(name) == engine::assets::AssetKind::Mesh) {
			engine::assets::MeshData mesh;
			if (engine::assets::Mesh::Read(reader, mesh)) {
				Renderer.AddMesh(interned, mesh);
			}
		}
	}

	void Editor::DrawCatalogueList(const CatalogueTab &tab) {
		// **Where it is, above what it holds.** A tab titled with somebody's
		// name for an origin says nothing about which folder or host that is,
		// and "why is this list empty" is almost always answered by the address.
		if (!tab.Location.empty()) {
			ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());
			ImGui::TextUnformatted(tab.Location.c_str());
			ImGui::PopStyleColor();
		}

		ImGui::Text("%zu asset(s)", tab.Entries.size());

		if (!tab.Note.empty()) {
			ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());
			ImGui::TextWrapped("%s", tab.Note.c_str());
			ImGui::PopStyleColor();
		}

		if (tab.Entries.empty()) {
			return;
		}

		// **Baking everything is one button and one confirmation-by-count**,
		// because a raw folder is somebody's whole art directory and a click
		// that decoded four thousand files without warning is a click nobody
		// forgives. The count is on the button, which is the warning.
		if (tab.Origin == CatalogueOrigin::Raw) {
			const std::string all = "Bake and load all " + std::to_string(tab.Entries.size());
			if (ImGui::Button(all.c_str())) {
				size_t loaded = 0;
				for (const CatalogueEntry &entry : tab.Entries) {
					loaded += LoadRawAsset(tab.Location, entry.Unbaked) ? 1 : 0;
				}
				AssetStatus = std::to_string(loaded) + " of " + std::to_string(tab.Entries.size()) +
							  " loaded from " + tab.Title;
			}

			ImGui::SameLine();
			ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());
			ImGui::TextUnformatted(
				Content.MemoryOnly ? "memory-only - nothing is written" : "writing into the store's baked/"
			);
			ImGui::PopStyleColor();
		}

		// **One column more in the merged tab.** Everywhere else the source is
		// the tab somebody is looking at, and a column repeating it would be the
		// same word on every row.
		const bool showSource = tab.Origin == CatalogueOrigin::All;
		const int columns = showSource ? 5 : 4;

		ImGui::SetNextItemWidth(-1.0f);
		TextField("##catalogue-filter", AssetFilter, "filter");

		if (!ImGui::BeginTable(
				"catalogue",
				columns,
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
		if (showSource) {
			ImGui::TableSetupColumn("Where", ImGuiTableColumnFlags_WidthFixed, engine::ui::Scaled(110.0f));
		}
		ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, engine::ui::Scaled(90.0f));
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableHeadersRow();

		// **Rebuilt only when the answer can have changed.** Filtering and
		// sorting the whole catalogue is proportional to the store - two
		// thousand assets on this repository's own - and it used to run every
		// frame to produce a list identical to the last one. `ImGuiListClipper`
		// below bounds what is *drawn*; it cannot bound building the list it
		// clips. See `Editor::AssetRows`.
		//
		// imgui sets `SpecsDirty` when somebody clicks a header and expects the
		// application to clear it, which is exactly the "the order changed"
		// signal this needs. It is read before the early-outs so a click is
		// never swallowed by a cache hit.
		bool stale = AssetRowsTab != static_cast<const void *>(&tab) ||
					 AssetRowsRevision != AssetTabsRevision || AssetRowsFilter != AssetFilter;

		ImGuiTableSortSpecs *specs = ImGui::TableGetSortSpecs();
		if (specs != nullptr && specs->SpecsDirty) {
			specs->SpecsDirty = false;
			stale = true;
		}

		if (stale) {
			// Filtered first, so the sort below orders what is actually drawn.
			AssetRows.clear();
			AssetRows.reserve(tab.Entries.size());
			for (const CatalogueEntry &entry : tab.Entries) {
				int score = 0;
				if (FuzzyMatch(AssetFilter, entry.Name, score)) {
					AssetRows.push_back(&entry);
				}
			}

			// **The view is sorted and the catalogue is not.** `AssetTabs` is
			// what each place said it holds, in name order, and the merged tab
			// is built from those vectors; a click on a header must not reorder
			// them.
			if (specs != nullptr) {
				SortCatalogue(AssetRows, specs, showSource);
			}

			AssetRowsTab = &tab;
			AssetRowsRevision = AssetTabsRevision;
			AssetRowsFilter = AssetFilter;
		}

		const std::vector<const CatalogueEntry *> &shown = AssetRows;

		ImGuiListClipper clipper;
		clipper.Begin(static_cast<int>(shown.size()));

		while (clipper.Step()) {
			for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
				const CatalogueEntry &entry = *shown[static_cast<size_t>(row)];

				ImGui::TableNextRow();

				ImGui::TableNextColumn();

				// **Only the rows the clipper drew ask for a picture**, which is
				// the bound that makes previews affordable over a store of
				// hundreds - Thumbnails.cpp.
				DrawPreview(entry.Name, engine::ui::Scaled(32.0f), entry.Kind);
				HoverPreview(entry.Name, entry.Kind);

				ImGui::TableNextColumn();

				// **The name is what a scene writes**, so it is the thing worth
				// copying - one click rather than reading it off and retyping.
				ImGui::AlignTextToFramePadding();
				ImGui::TextUnformatted(entry.Name.c_str());
				HoverPreview(entry.Name, entry.Kind);
				if (ImGui::IsItemClicked()) {
					ImGui::SetClipboardText(entry.Name.c_str());
				}

				ImGui::TableNextColumn();
				ImGui::TextUnformatted(KindName(entry.Kind));

				if (showSource) {
					ImGui::TableNextColumn();
					ImGui::TextUnformatted(entry.Source.c_str());
				}

				ImGui::TableNextColumn();

				// **The address column is where an unbaked row is acted on.**
				// It has no address to show - nothing has hashed it, because
				// nothing has baked it - and a per-row button is what turns
				// "this folder holds a crate" into a crate in the viewport.
				//
				// **Only on the folder's own tab**, because the folder is the
				// tab's location and a merged row does not carry it. A button
				// that guessed which of several raw folders a name came from
				// would bake the wrong file the first time two folders held one.
				if (!entry.Unbaked.empty()) {
					if (tab.Origin != CatalogueOrigin::Raw) {
						ImGui::TextDisabled("unbaked");
						continue;
					}

					ImGui::PushID(row);
					if (ImGui::SmallButton("Load")) {
						LoadRawAsset(tab.Location, entry.Unbaked);
					}
					ImGui::PopID();
					if (ImGui::IsItemHovered()) {
						ImGui::SetTooltip("%s", entry.Unbaked.c_str());
					}
					continue;
				}

				// **A generated asset says so rather than showing zeros.** Its
				// address is all-zero because it has none - nothing fetches a
				// built-in - and eight zeros in an address column reads as a
				// publish that went wrong.
				if (entry.Root.IsZero()) {
					ImGui::TextDisabled("generated");
					continue;
				}

				const std::string hex = entry.Root.ToHex();
				ImGui::TextUnformatted(hex.substr(0, 8).c_str());
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("%s", hex.c_str());
				}
			}
		}

		ImGui::EndTable();
	}

	void Editor::SortCatalogue(
		std::vector<const CatalogueEntry *> &rows, const ImGuiTableSortSpecs *specs, bool showSource
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
				rows.begin(), rows.end(), [&](const CatalogueEntry *left, const CatalogueEntry *right) {
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
						less =
							std::string_view(KindName(left->Kind)) < std::string_view(KindName(right->Kind));
						break;
					case 3:
						// **Column three is two different columns.** The merged
						// tab draws "Where" here and the address after it; every
						// other tab has no "Where" at all, so the same index is
						// its address column. imgui reports a position and not a
						// name, so the caller's layout is what disambiguates -
						// sorting by the wrong field is the kind of thing that
						// looks like an unstable sort rather than a bug.
						less = showSource ? left->Source < right->Source
										  : left->Root.ToHex() < right->Root.ToHex();
						break;
					case 4:
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
			ImGui::TextDisabled("nothing imported yet - drop files on the window, or use the buttons above");
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
		// drawn - a clipper over the unfiltered list would leave gaps.
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
				// this file is called under `raw/` - which is the path
				// `ThumbnailFor` resolves. The two differ: `Original` is what it
				// was called before it came in.
				const std::string stored = entry.Path.filename().string();
				const engine::assets::AssetKind kind = engine::assets::KindOfName(stored);

				DrawPreview(stored, engine::ui::Scaled(32.0f), kind);
				HoverPreview(stored, kind);

				ImGui::TableNextColumn();
				ImGui::AlignTextToFramePadding();
				ImGui::TextUnformatted(entry.Original.c_str());
				HoverPreview(stored, kind);

				ImGui::TableNextColumn();
				ImGui::TextUnformatted(Readable(entry.Bytes).c_str());

				ImGui::TableNextColumn();

				// The hash-named file this actually is, which is what somebody
				// looking in the folder with a terminal will see.
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

		// Least significant column first, stably - `SortPublished` carries why.
		for (int index = specs->SpecsCount - 1; index >= 0; index--) {
			const ImGuiTableColumnSortSpecs &spec = specs->Specs[index];
			const bool ascending = spec.SortDirection == ImGuiSortDirection_Ascending;

			std::stable_sort(
				rows.begin(), rows.end(), [&](const cdn::RawEntry *left, const cdn::RawEntry *right) {
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

		// **Built from the configured sources rather than from this machine's
		// store alone.** The store is one of those sources - the default list's
		// first row points at exactly this folder - so it needs no special case
		// here, and an editor pointed at somebody else's published tree lists
		// that one the same way.
		//
		// **This is where an HTTP origin is asked what it holds, and the wait is
		// why it is only here.** `MakeOriginLister` blocks with a ceiling, and
		// every caller of this function is somebody having asked - opening the
		// panel, pressing Refresh, importing, publishing. `RebuildContentClients`
		// deliberately does not ask, because it runs at start-up.
		const std::unique_ptr<OriginLister> origins = MakeOriginLister();
		AssetTabs = BuildCatalogue(Content, origins.get());
		AssetTabsRevision++;
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
		size_t baked = 0;
		const uint64_t now = NowSeconds();

		const auto take = [&](const std::filesystem::path &file) {
			const auto report = cdn::ImportFile(paths, file, now);
			if (!report.has_value()) {
				failures++;
				return;
			}
			if (report->Duplicate) {
				duplicates++;
				return;
			}
			imported++;

			// **Baked as well as copied, because an import that stops at `raw/`
			// puts nothing in the store anything can read.** This did three
			// things - ensure the store, copy the bytes, refresh the list - and
			// baking was not one of them, so a dropped file was invisible in the
			// viewport, silently absent from the next Publish, and on a fresh
			// store made `PublishLocal` refuse the whole thing. That refusal's
			// own message reads "bake before publishing - `contentimport
			// --publish` and the studio both do", which was true of one of them.
			//
			// `BakeRawAsset` is the same one-file bake the asset picker's per-row
			// button uses: the same `assetc` settings, the same unit-box scale,
			// the same `StoreTextureResolver`, and it registers the result with
			// this editor's renderer on the way out.
			//
			// **Not fatal when it fails.** Plenty of what a person drags into a
			// content store is not bakeable - a licence, a `.txt`, a source file
			// beside the model it belongs to - and `ImportFile` accepted those
			// deliberately. A count is the honest report; a failure here would
			// make dragging a folder in an error.
			std::string name;
			if (BakeRawAsset(report->Stored.filename().generic_string(), name)) {
				baked++;
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

		AssetStatus = std::to_string(imported) + " imported, " + std::to_string(baked) + " baked, " +
					  std::to_string(duplicates) + " already there, " + std::to_string(failures) + " failed";
		ENGINE_INFO("assets: {}", AssetStatus);

		// The list on screen is now wrong, and this is the only place that knows
		// it changed.
		RefreshStoreContents();
	}

	void Editor::DropAssetPath(const std::string &path) {
		// **Opened rather than only imported.** A person dropping a file on a
		// closed panel gets no feedback at all otherwise - the import happens,
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
			AssetStatus = "the publish failed - see the output panel";
			return;
		}

		AssetStatus = std::to_string(report->Assets) + " asset(s) in " + std::to_string(report->Bundles) +
					  " bundle(s), root " + report->Root.ToHex().substr(0, 8);
		ENGINE_INFO("assets: {}", AssetStatus);

		// **Refreshed here rather than left for the next open.** Publishing is
		// what fills the picker, and a picker that still said "nothing
		// published" straight after would read as the publish having failed.
		RefreshStoreContents();
	}
}
