// Choosing a mesh or a texture from what the content store actually holds.
//
// **The panel this replaces was a text field, and the failure mode was
// silence.** `MeshPart.Mesh` takes the name a publisher wrote - rule 4, an id
// does not cross - so authoring one meant knowing the string, spelling it
// exactly, and finding out it was wrong by looking at a part that had not
// changed. Nothing warns: an unknown mesh name is a part the renderer draws with
// the missing-mesh marker, which is also what a mesh that has not streamed in
// yet looks like.
//
// **The list is the store's published manifest and nothing else.** Not the
// world's `MeshCatalogue` - that holds what this session happened to load, so
// the list would grow as you played and be empty on a fresh editor. Not a
// directory walk of `raw/` - those are hash-named and cannot say what anything
// is called. The manifest is the one thing that knows both the name and the
// kind, which is exactly the pair a picker needs.
//
// **And it only ever lists `~/Documents/atomic-game-engine/cdn`.** One store,
// the one every program in this repo agrees on - `cdn::DefaultLocalPaths`. A
// picker that browsed the filesystem would offer paths that mean nothing to a
// manifest, and a name that is not in one is a name no client can fetch.

#include <engine/assets/AssetKind.hpp>
#include <engine/assets/Builtin.hpp>
#include <engine/assets/ContentPolicy.hpp>
#include <engine/ui/Metrics.hpp>
#include <engine/ui/Theme.hpp>

#include <algorithm>
#include <cdn/LocalStore.hpp>
#include <filesystem>
#include <imgui.h>
#include <string>
#include <string_view>
#include <studio/AssetRow.hpp>
#include <studio/Assets.hpp>
#include <studio/Editor.hpp>
#include <studio/Widgets.hpp>
#include <vector>

namespace studio {

	using engine::assets::AssetKind;

	AssetKind ContentKindOfProperty(std::string_view property) {
		// **A short flat table walked linearly**, because it has five rows and
		// the caller is a panel drawing one property. A map here would be a hash
		// per property per frame to save four comparisons.
		struct Row {
			std::string_view Property;
			AssetKind Kind;
		};

		// **Both spellings of every aliased property, and the aliases are why
		// this list was wrong the first time.** `Visual::Mesh` is bound twice -
		// as `BasePart.Mesh` and as `MeshPart.MeshId`, Roblox's name - and
		// `SurfaceAppearance::ColourMap` is bound twice as `BasePart.ColorMap`
		// and `MeshPart.TextureID`. The first version of this table had one of
		// each pair, so selecting a `MeshPart` showed a picker on the two
		// spellings nobody uses and a bare text field on the two they do.
		//
		// That is the exact cost `Assets.hpp` warns about, and it landed on the
		// most obvious property in the editor. The rule it leaves behind: an
		// alias is a row here too, because a person picks whichever name their
		// class shows them.
		static constexpr Row ROWS[] = {
			// **`MeshPart.MeshId` and `MeshPart.TextureID`, and no aliases.**
			// These were declared on `BasePart` as `Mesh` and `ColorMap` too,
			// and the pair of spellings is what made this table's own warning
			// come true: the first version had one of each, so selecting a
			// `MeshPart` showed a plain text field on the two names it displays.
			// v0.10 removed the aliases - geometry from a file is not something
			// a plain `Part` has - so there is one name for each and no way for
			// the halves to drift.
			{"MeshId", AssetKind::Mesh},
			{"TextureID", AssetKind::Texture},

			// `ParticleEmitter`, `Beam` and `Trail` all spell it this way, and
			// all three mean the same thing - which is why the key is the
			// property and not the class.
			{"Texture", AssetKind::Texture},

			// `Material.MaterialId`, the one property of the instance that
			// replaced `Enum.Material`. **The only row naming an
			// `AssetKind::Material`**, and the reason the kind stopped being one
			// nothing wrote - `scene/Materials.hpp`.
			{"MaterialId", AssetKind::Material},

			// `Sound.SoundId`.
			{"SoundId", AssetKind::Audio},

			// `ImageLabel.Image`, in the interface tree.
			{"Image", AssetKind::Texture},
		};

		for (const Row &row : ROWS) {
			if (row.Property == property) {
				return row.Kind;
			}
		}
		return AssetKind::Unknown;
	}

	engine::game::PropertyValue ChosenContentValue(engine::ecs::PropertyType type, std::string_view chosen) {
		engine::game::PropertyValue value;
		value.Type = type;
		value.Name = chosen.empty() ? engine::core::Name{} : engine::core::Name(chosen);
		return value;
	}

	bool Editor::DrawAssetPicker(const char *title, AssetKind kind, std::string &chosen) {
		bool confirmed = false;

		const ImGuiViewport *main = ImGui::GetMainViewport();
		ImGui::SetNextWindowPos(main->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
		ImGui::SetNextWindowSize(
			ImVec2(engine::ui::Scaled(560.0f), engine::ui::Scaled(420.0f)), ImGuiCond_Appearing
		);

		if (!ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_NoSavedSettings)) {
			return false;
		}

		// **Read when the popup opens, not every frame.** Reading a manifest is
		// opening a file and parsing it, and doing that sixty times a second
		// while somebody reads a list would pin a disk for no benefit - the
		// store cannot change while a modal is up unless somebody publishes
		// from another process, and the Refresh button below is for them.
		if (ImGui::IsWindowAppearing()) {
			RefreshPickerContents();
			PickerFilter.clear();
		}

		ImGui::TextDisabled("%s", cdn::DefaultLocalPaths().Root.string().c_str());

		ImGui::SetNextItemWidth(-engine::ui::Scaled(90.0f));

		// Focused on open, because somebody who knows the name wants to type it
		// and a list of three hundred is faster to filter than to scroll.
		if (ImGui::IsWindowAppearing()) {
			ImGui::SetKeyboardFocusHere();
		}
		TextField("##filter", PickerFilter, "filter");

		ImGui::SameLine();
		if (ImGui::Button("Refresh", ImVec2(engine::ui::Scaled(80.0f), 0.0f))) {
			RefreshPickerContents();
		}

		ImGui::Separator();

		// The rows that survive the kind and the filter, scored so that an
		// exact match sorts above something that merely contains the letters -
		// `FuzzyMatch`'s reason: typing "fox" should not put `foxglove_bark`
		// above `fox_dance`.
		struct Candidate {
			const cdn::PublishedEntry *Entry;
			int Score;
		};
		std::vector<Candidate> shown;

		// **The engine's own meshes first, and they are not published.** See
		// `RefreshPickerContents`: six meshes exist in every process before any
		// content is fetched, and leaving them out left this list empty on a
		// machine with no store - a picker that appears not to work rather than
		// a store that is not there. Sorted with everything else below, so a
		// filter still finds what it matches.
		for (const cdn::PublishedEntry &entry : PickerBuiltins) {
			if (entry.Kind != kind) {
				continue;
			}
			int score = 0;
			if (FuzzyMatch(PickerFilter, entry.Name, score)) {
				// **`+1`, so an equal match on a built-in wins.** Nothing is
				// riding on this beyond a stable order somebody can predict:
				// with no filter typed, the six that always resolve are the six
				// at the top.
				shown.push_back(Candidate{.Entry = &entry, .Score = score + 1});
			}
		}

		for (const cdn::PublishedEntry &entry : PickerContents) {
			if (entry.Kind != kind) {
				continue;
			}

			// **Source forms are not offered, because choosing one does
			// nothing.** A `.pmx` and a `.amesh` are both `AssetKind::Mesh`, and
			// only the second is something a runtime reads - so a picker
			// listing both offered a choice that silently left the part looking
			// exactly as it did. Until v0.10 the local store published `raw/`
			// straight through, so *most* of this list was that.
			//
			// `assets::IsRuntimeReadable` is the one place the two halves of the
			// extension table are told apart; a second opinion here would be a
			// picker that disagreed with the loader about what works.
			if (!engine::assets::IsRuntimeReadable(entry.Name)) {
				continue;
			}

			// **Nor a form this deployment turned off**, for the same reason one
			// step out: `RequestContentAsset` will refuse to fetch it, so
			// offering it is offering a choice that leaves the part looking
			// exactly as it did.
			if (!engine::assets::ContentPolicy::Process(engine::assets::ContentVerb::Handle)
					 .AllowsName(entry.Name)) {
				continue;
			}
			int score = 0;
			if (!FuzzyMatch(PickerFilter, entry.Name, score)) {
				continue;
			}
			shown.push_back(Candidate{.Entry = &entry, .Score = score});
		}

		std::stable_sort(shown.begin(), shown.end(), [](const Candidate &left, const Candidate &right) {
			return left.Score > right.Score;
		});

		const float footer = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;

		// **Two tabs, because the store has two halves and only one of them is
		// selectable as it stands.** `baked/` is what a publisher published and
		// what a runtime reads; `raw/` is what somebody dragged in five seconds
		// ago and has not been through a baker. Before this the second was simply
		// invisible, so a file you had just imported could not be chosen until
		// the whole store had been republished - which on this repository's own
		// store is four minutes.
		if (ImGui::BeginTabBar("##halves")) {
			if (ImGui::BeginTabItem("Published")) {
				PickerShowRaw = false;
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("Raw")) {
				PickerShowRaw = true;
				ImGui::EndTabItem();
			}
			ImGui::EndTabBar();
		}

		if (ImGui::BeginChild("##rows", ImVec2(0.0f, -footer), ImGuiChildFlags_Borders)) {
			if (PickerShowRaw) {
				DrawRawPickerRows(kind, chosen, confirmed);
				ImGui::EndChild();
				return FinishAssetPicker(chosen, confirmed);
			}

			if (PickerContents.empty()) {
				// **The two empty cases are different and are said
				// differently.** Nothing published at all is a pipeline that has
				// not been run; nothing *of this kind* is a store that has
				// content and none of it is a mesh. Somebody acts on those
				// differently, and one message for both sends them to the wrong
				// place.
				ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());
				ImGui::TextWrapped(
					"nothing published - import files below and press Publish in the Assets panel"
				);
				ImGui::PopStyleColor();
			} else if (shown.empty()) {
				ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());
				ImGui::TextWrapped(
					"no %s in the store%s",
					kind == AssetKind::Mesh	   ? "meshes"
					: kind == AssetKind::Audio ? "sounds"
											   : "textures",
					PickerFilter.empty() ? "" : " matching that"
				);
				ImGui::PopStyleColor();
			}

			// **Rows are a picture and a name, and the row is as tall as the
			// picture.** A store's names are hashes - a person importing
			// `diffuse.png` gets `<64 hex>.png` - so a list of names alone is a
			// list of identifiers nobody can choose between. The thumbnail is
			// what makes this a picker rather than a lookup table.
			//
			// **One item per row and everything else painted**, which is
			// `studio/AssetRow.hpp`'s rule and the reason this loop no longer
			// aborts the editor on the frame it opens.
			const float side = engine::ui::Scaled(48.0f);
			const float spacing = ImGui::GetStyle().ItemSpacing.x;

			// **Clipped, and the absence of this was three separate bugs.** The
			// list is every texture in the store - 1,637 of them here - and
			// without a clipper every one submitted a `Selectable` and asked for
			// a thumbnail on every frame the modal was open. That is a visible
			// stall on a list nobody can read all of, and it is why the previews
			// looked broken: `ThumbnailFor` queues what it is asked for, the
			// cache holds 256, and asking for 1,637 a frame evicts each one long
			// before its turn to be built came round. The pictures were not
			// failing, they were being thrown away.
			//
			// `DrawPublishedList` in the assets panel has had one since it was
			// written; the picker was small when it was written and stopped
			// being small when the store filled up.
			//
			// **Uniform row height is what makes it exact** - every row is
			// `side` tall by construction, so the clipper needs no measuring
			// pass and scrolling lands where the scrollbar says.
			ImGuiListClipper clipper;
			clipper.Begin(static_cast<int>(shown.size()), side + ImGui::GetStyle().ItemSpacing.y);

			while (clipper.Step()) {
				for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
					const cdn::PublishedEntry &entry = *shown[static_cast<size_t>(row)].Entry;
					ImGui::PushID(entry.Name.c_str());

					const RowAction action = DrawAssetRow(entry.Name == chosen, side, [&](ImVec2 corner) {
						PaintPreview(corner.x, corner.y, side, entry.Name, entry.Kind);

						// Centred against the picture rather than sitting on its
						// top edge, which is what makes a tall row read as one
						// row.
						const float baseline = corner.y + (side - ImGui::GetTextLineHeight()) * 0.5f;
						ImGui::GetWindowDrawList()->AddText(
							ImVec2(corner.x + side + spacing, baseline),
							ImGui::GetColorU32(ImGuiCol_Text),
							entry.Name.c_str()
						);
					});

					if (action != RowAction::None) {
						chosen = entry.Name;
						confirmed = action == RowAction::Confirmed;
					}

					// **The big preview, on the row rather than as a tooltip.** A
					// picker is exactly where somebody needs to see the thing before
					// choosing it, and 48 pixels is not enough to recognise art.
					// Reads `IsItemHovered()`, which is still the row's - see
					// `DrawAssetRow`.
					HoverPreview(entry.Name, entry.Kind);

					ImGui::PopID();
				}
			}
		}
		ImGui::EndChild();
		return FinishAssetPicker(chosen, confirmed);
	}

	bool Editor::FinishAssetPicker(std::string &chosen, bool confirmed) {
		// **The footer, shared by both tabs.** Two copies of Use/Cancel/Clear is
		// two places for "Clear" to stop clearing, and the bug would only show on
		// whichever tab somebody used less.
		const ImVec2 button(engine::ui::Scaled(110.0f), 0.0f);

		ImGui::BeginDisabled(chosen.empty());
		if (ImGui::Button("Use", button)) {
			confirmed = true;
		}
		ImGui::EndDisabled();

		ImGui::SameLine();
		if (ImGui::Button("Cancel", button) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
			ImGui::CloseCurrentPopup();
		}

		ImGui::SameLine();

		// **"Clear" and not an empty row in the list.** Emptying the property is
		// a legitimate thing to want - a part with no mesh is a plain part - and
		// a blank `Selectable` at the top of a list is a row people click by
		// accident.
		if (ImGui::Button("Clear", button)) {
			chosen.clear();
			confirmed = true;
		}

		if (!AssetStatus.empty()) {
			ImGui::SameLine();
			ImGui::TextDisabled("%s", AssetStatus.c_str());
		}

		if (confirmed) {
			ImGui::CloseCurrentPopup();
		}

		ImGui::EndPopup();
		return confirmed;
	}

	void Editor::DrawRawPickerRows(AssetKind kind, std::string &chosen, bool &confirmed) {
		const float side = engine::ui::Scaled(48.0f);
		const float spacing = ImGui::GetStyle().ItemSpacing.x;

		ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());
		ImGui::TextWrapped(
			"unbaked sources - choosing one bakes it now. Publish before a client can fetch it."
		);
		ImGui::PopStyleColor();

		// **Filtered into a list first, so the clipper below has a count.** The
		// raw half of this store is 1,974 files; drawing all of them to find out
		// how many matched is the stall the clipper exists to prevent.
		//
		// **Classified by the label and not by the file on disk**, because
		// `ImportFile` renames to `<hash><extension>` and the extension is the
		// half that survives. `RawEntry::Original` is the name somebody gave it,
		// which is also what a person is scanning the list for.
		std::vector<const cdn::RawEntry *> shown;
		shown.reserve(PickerRaw.size());
		for (const cdn::RawEntry &entry : PickerRaw) {
			if (engine::assets::KindOfName(entry.Original) != kind) {
				continue;
			}
			int score = 0;
			if (FuzzyMatch(PickerFilter, entry.Original, score)) {
				shown.push_back(&entry);
			}
		}

		ImGuiListClipper clipper;
		clipper.Begin(static_cast<int>(shown.size()), side + ImGui::GetStyle().ItemSpacing.y);

		while (clipper.Step()) {
			for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
				const cdn::RawEntry &entry = *shown[static_cast<size_t>(row)];

				const std::string relative = RawRelativePath(entry);
				ImGui::PushID(relative.c_str());

				const RowAction action = DrawAssetRow(false, side, [&](ImVec2 corner) {
					PaintPreview(corner.x, corner.y, side, relative, kind);

					const float baseline = corner.y + (side - ImGui::GetTextLineHeight()) * 0.5f;
					ImGui::GetWindowDrawList()->AddText(
						ImVec2(corner.x + side + spacing, baseline),
						ImGui::GetColorU32(ImGuiCol_Text),
						entry.Original.c_str()
					);
				});

				if (action != RowAction::None) {
					// **Baked here and not on confirm**, so the name written into
					// the property is one that exists - a picker that handed back a
					// raw name would put a `.png` on a `ColorMap`, which is the
					// exact thing this store spent four versions doing.
					if (std::string baked; BakeRawAsset(relative, baked)) {
						chosen = baked;
						confirmed = action == RowAction::Confirmed;
					}
				}

				HoverPreview(relative, kind);
				ImGui::PopID();
			}
		}

		if (shown.empty()) {
			ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());
			ImGui::TextWrapped("nothing of that kind in raw/");
			ImGui::PopStyleColor();
		}
	}

	std::string Editor::RawRelativePath(const cdn::RawEntry &entry) {
		std::error_code failure;
		const std::filesystem::path relative =
			std::filesystem::relative(entry.Path, cdn::DefaultLocalPaths().Raw, failure);
		return failure ? entry.Path.filename().generic_string() : relative.generic_string();
	}

	void Editor::RefreshPickerContents() {
		const cdn::LocalPaths paths = cdn::DefaultLocalPaths();
		PickerContents = cdn::PublishedContents(paths);
		PickerRaw = cdn::RawContents(paths);

		// **The engine's own assets, at the top, and they are not in any
		// manifest.** `assets::MakeBuiltin` generates them in every process -
		// six shapes and the checker sheet - and `MeshTable::Initialise` and
		// `TextureTable::Initialise` register them before a single byte of
		// content has been fetched. They are the only names an editor is
		// guaranteed to be able to resolve.
		//
		// Leaving them out made the mesh picker *empty* on a machine with no
		// published store, which reads as a picker that does not work rather
		// than as a store that is not there - and it made the six meshes every
		// `Part` in the engine is already drawn with unreachable from the one
		// panel whose job is to choose geometry.
		//
		// **Kept apart from `PickerContents` rather than mixed into it**, because
		// that vector answers "what has this store published" - the empty-state
		// message reads it, and rows that are always there would make "nothing
		// published" a sentence that could never be shown.
		//
		// **The list itself is `EngineAssets`', not a second walk of the same
		// enums.** The assets panel's engine tab draws that vector, and two
		// enumerations of one set is how the picker ends up offering a sheet the
		// panel does not list, or the reverse. `Root` stays at its default
		// there: a built-in has no content address because it has no content.
		PickerBuiltins.clear();
		PickerBuiltins.reserve(EngineAssets().size());
		for (const CatalogueEntry &entry : EngineAssets()) {
			PickerBuiltins.push_back(
				cdn::PublishedEntry{
					.Name = entry.Name,
					.Kind = entry.Kind,
					.Root = entry.Root,
				}
			);
		}
	}
}
