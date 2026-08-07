// Choosing a mesh or a texture from what the content store actually holds.
//
// **The panel this replaces was a text field, and the failure mode was
// silence.** `MeshPart.Mesh` takes the name a publisher wrote — rule 4, an id
// does not cross — so authoring one meant knowing the string, spelling it
// exactly, and finding out it was wrong by looking at a part that had not
// changed. Nothing warns: an unknown mesh name is a part the renderer draws with
// the missing-mesh marker, which is also what a mesh that has not streamed in
// yet looks like.
//
// **The list is the store's published manifest and nothing else.** Not the
// world's `MeshCatalogue` — that holds what this session happened to load, so
// the list would grow as you played and be empty on a fresh editor. Not a
// directory walk of `raw/` — those are hash-named and cannot say what anything
// is called. The manifest is the one thing that knows both the name and the
// kind, which is exactly the pair a picker needs.
//
// **And it only ever lists `~/Documents/atomic-game-engine/cdn`.** One store,
// the one every program in this repo agrees on — `cdn::DefaultLocalPaths`. A
// picker that browsed the filesystem would offer paths that mean nothing to a
// manifest, and a name that is not in one is a name no client can fetch.

#include <engine/assets/AssetKind.hpp>
#include <engine/ui/Metrics.hpp>
#include <engine/ui/Theme.hpp>

#include <algorithm>
#include <cdn/LocalStore.hpp>
#include <imgui.h>
#include <string>
#include <string_view>
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

		static constexpr Row ROWS[] = {
			// `BasePart.Mesh` — what a `MeshPart` is made of.
			{"Mesh", AssetKind::Mesh},

			// `MeshPart.TextureID`. Roblox's spelling, kept because a scene
			// written against Roblox should load.
			{"TextureID", AssetKind::Texture},

			// `ParticleEmitter`, `Beam` and `Trail` all spell it this way, and
			// all three mean the same thing — which is why the key is the
			// property and not the class.
			{"Texture", AssetKind::Texture},

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
		// while somebody reads a list would pin a disk for no benefit — the
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
		// exact match sorts above something that merely contains the letters —
		// `FuzzyMatch`'s reason: typing "fox" should not put `foxglove_bark`
		// above `fox_dance`.
		struct Candidate {
			const cdn::PublishedEntry *Entry;
			int Score;
		};
		std::vector<Candidate> shown;

		for (const cdn::PublishedEntry &entry : PickerContents) {
			if (entry.Kind != kind) {
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

		if (ImGui::BeginChild("##rows", ImVec2(0.0f, -footer), ImGuiChildFlags_Borders)) {
			if (PickerContents.empty()) {
				// **The two empty cases are different and are said
				// differently.** Nothing published at all is a pipeline that has
				// not been run; nothing *of this kind* is a store that has
				// content and none of it is a mesh. Somebody acts on those
				// differently, and one message for both sends them to the wrong
				// place.
				ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());
				ImGui::TextWrapped(
					"nothing published — import files below and press Publish in the Assets panel"
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

			for (const Candidate &candidate : shown) {
				const cdn::PublishedEntry &entry = *candidate.Entry;
				ImGui::PushID(entry.Name.c_str());

				const bool selected = entry.Name == chosen;
				if (ImGui::Selectable(entry.Name.c_str(), selected, ImGuiSelectableFlags_AllowDoubleClick)) {
					chosen = entry.Name;

					// Double-click confirms, which is what a person tries first
					// — `FilePrompt` does the same and for the same reason.
					if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
						confirmed = true;
					}
				}

				// The content address, for somebody comparing two stores.
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("%s\n%s", entry.Name.c_str(), entry.Root.ToHex().substr(0, 16).c_str());
				}

				ImGui::PopID();
			}
		}
		ImGui::EndChild();

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
		// a legitimate thing to want — a part with no mesh is a plain part — and
		// a blank `Selectable` at the top of a list is a row people click by
		// accident.
		if (ImGui::Button("Clear", button)) {
			chosen.clear();
			confirmed = true;
		}

		if (confirmed) {
			ImGui::CloseCurrentPopup();
		}

		ImGui::EndPopup();
		return confirmed;
	}

	void Editor::RefreshPickerContents() {
		PickerContents = cdn::PublishedContents(cdn::DefaultLocalPaths());
	}
}
