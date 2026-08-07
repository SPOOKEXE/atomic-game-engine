// Every image in the world at once, and what is using it.
//
// **Taken from `explorer-plus`'s gallery, including the two decisions that make
// it worth having.** The first is that a tile is one *asset* rather than one
// instance — a texture used by forty parts is one tile with `x40` on it, not
// forty identical tiles. The second is what the tile is labelled with:
//
// > Four tiles reading "SurfaceAppearance" tell you nothing; "ColorMap" and
// > "NormalMap" tell you everything.
//
// So the caption is the *property* doing the using. In this engine that is
// `Mesh`, `TextureID`, `Texture`, `ColorMap`, `SoundId` or `Image` — the same
// table the property picker reads, which is what keeps the two from disagreeing
// about what counts as content.
//
// **It answers a question no other panel can.** The assets panel says what the
// store holds and the explorer says what the tree holds; neither says *where is
// this texture actually used*, which is the question somebody has when they are
// about to change or delete one. Clicking a tile selects every instance using
// it, which is the reference's answer and the right one.
//
// **It walks the world and not the store.** A store's manifest lists what could
// be used; this lists what is. The two differ in both directions and the
// difference is the point: an asset in the store that no tile mentions is
// content nothing references, and a tile whose asset is not in the store is a
// name that will never resolve.

#include <engine/assets/AssetKind.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/ui/Metrics.hpp>
#include <engine/ui/Theme.hpp>

#include <algorithm>
#include <imgui.h>
#include <studio/Assets.hpp>
#include <studio/Editor.hpp>
#include <studio/Preview.hpp>
#include <studio/Widgets.hpp>

namespace studio {

	namespace {
		using engine::ecs::ClassId;
		using engine::ecs::Store;
		using engine::ecs::Classes;
		using engine::ecs::PropertyDescriptor;
		using engine::ecs::PropertyType;

		// How big a tile is. Bigger than a row's thumbnail on purpose: the
		// gallery exists to be *looked* at, and the reference's own note is that
		// a tile should be as big as it needs to be to recognise the art.
		constexpr float TILE_WIDTH = 108.0f;
		constexpr float TILE_HEIGHT = 136.0f;
		constexpr float TILE_IMAGE = 72.0f;
	}

	void Editor::RebuildGallery() {
		Gallery.clear();
		GalleryScanned = true;

		if (!Universe) {
			return;
		}

		// **Every world, not just the selected one.** An image used only by a
		// world nobody has open is exactly the one somebody is hunting for.
		for (const WorldId world : Universe->Worlds()) {
			Universe->Enter(world, [&](Store &store) {
				store.EachEntity([&](Entity instance) {
					const ClassId klass = store.ClassOf(instance);
					if (!klass.IsValid()) {
						return;
					}

					for (const PropertyDescriptor &descriptor : Classes::Describe(klass).Properties) {
						if (descriptor.Type != PropertyType::Name) {
							continue;
						}

						// **The picker's own table decides what content is.** A
						// second list here would be a second opinion, and the two
						// would disagree the first time one grew a row.
						const engine::assets::AssetKind kind = ContentKindOfProperty(descriptor.Spelling);
						if (kind == engine::assets::AssetKind::Unknown) {
							continue;
						}

						engine::core::Name value;
						if (descriptor.Get == nullptr ||
							!descriptor.Get(store, instance, &value) || !value.IsValid()) {
							continue;
						}

						GalleryEntry *found = nullptr;
						for (GalleryEntry &candidate : Gallery) {
							if (candidate.Asset == value) {
								found = &candidate;
								break;
							}
						}

						if (found == nullptr) {
							Gallery.push_back(
								GalleryEntry{
									.Asset = value,
									.Kind = kind,
									.Property = std::string(descriptor.Spelling),
									.World = world,
									.First = instance,
									.Uses = 0,
								}
							);
							found = &Gallery.back();
						}
						found->Uses++;
					}
				});
			});
		}

		// **By use count, most-used first.** The tile somebody is looking for is
		// usually the one that matters, and "which texture is on everything" is
		// a question the order can answer for free.
		std::sort(Gallery.begin(), Gallery.end(), [](const GalleryEntry &left, const GalleryEntry &right) {
			if (left.Uses != right.Uses) {
				return left.Uses > right.Uses;
			}
			return std::string_view(left.Asset.Text()) < std::string_view(right.Asset.Text());
		});
	}

	void Editor::SelectGalleryUsers(const GalleryEntry &entry) {
		// **Selects everything using the asset, which is the whole gesture.**
		// The reference's note: "Clicking a tile selects everything using that
		// image, which is the question the gallery is usually being asked."
		std::vector<Entity> found;

		Universe->Enter(entry.World, [&](Store &store) {
			store.EachEntity([&](Entity instance) {
				const ClassId klass = store.ClassOf(instance);
				if (!klass.IsValid()) {
					return;
				}
				for (const PropertyDescriptor &descriptor : Classes::Describe(klass).Properties) {
					if (descriptor.Type != PropertyType::Name || descriptor.Get == nullptr) {
						continue;
					}
					if (ContentKindOfProperty(descriptor.Spelling) == engine::assets::AssetKind::Unknown) {
						continue;
					}

					engine::core::Name value;
					if (descriptor.Get(store, instance, &value) && value == entry.Asset) {
						found.push_back(instance);
						return;
					}
				}
			});
		});

		SelectionWorld = entry.World;
		Selection = std::move(found);
	}

	void Editor::DrawGallery() {
		// **Rebuilt when the tab opens and on demand, not per frame.** Walking
		// every entity of every world reading every `Name` property is a real
		// cost — it is the one thing in this panel that scales with the scene
		// rather than with the store.
		//
		// **The gate is "have I scanned", not "is the result empty", and the
		// difference was seconds of frozen editor.** An empty result is a
		// *result*: a fresh place names no assets at all, so `Gallery.empty()`
		// stayed true, so the full walk ran again on the next frame, and the
		// next — for as long as the panel was open. Inserting anything made it
		// worse by adding an entity to a scan that was already running every
		// frame, which is why it showed up as "the studio greys out when I
		// insert something".
		//
		// A flag rather than a timestamp: what makes the scan stale is somebody
		// changing a world, and `Rescan` is the honest answer until there is a
		// change signal worth hanging it on.
		if (ImGui::IsWindowAppearing() || !GalleryScanned) {
			RebuildGallery();
		}

		ImGui::Text("%zu image(s) in use", Gallery.size());
		ImGui::SameLine();
		if (ImGui::SmallButton("Rescan")) {
			RebuildGallery();
		}

		if (Gallery.empty()) {
			ImGui::TextDisabled("nothing in these worlds names a mesh, texture or sound");
			return;
		}

		ImGui::SetNextItemWidth(-1.0f);
		TextField("##gallery-filter", AssetFilter, "filter");

		if (!ImGui::BeginChild("##tiles", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders)) {
			ImGui::EndChild();
			return;
		}

		const float tileWidth = engine::ui::Scaled(TILE_WIDTH);
		const float tileHeight = engine::ui::Scaled(TILE_HEIGHT);
		const float image = engine::ui::Scaled(TILE_IMAGE);

		const float available = ImGui::GetContentRegionAvail().x;
		const auto columns = static_cast<int>(std::max(1.0f, std::floor(available / tileWidth)));

		int column = 0;
		for (const GalleryEntry &entry : Gallery) {
			const std::string name(entry.Asset.Text());

			int score = 0;
			if (!FuzzyMatch(AssetFilter, name, score)) {
				continue;
			}

			if (column > 0) {
				ImGui::SameLine();
			}

			ImGui::PushID(name.c_str());
			ImGui::BeginGroup();

			const ImVec2 start = ImGui::GetCursorPos();

			// The whole tile is the target, drawn first and then written over —
			// the picker's arrangement, for its reason: a click on the picture
			// is what a person does.
			if (ImGui::Selectable("##tile", false, ImGuiSelectableFlags_None, ImVec2(tileWidth, tileHeight))) {
				SelectGalleryUsers(entry);
			}
			HoverPreview(name, entry.Kind);

			ImGui::SetCursorPos(ImVec2(start.x + (tileWidth - image) * 0.5f, start.y + 4.0f));
			DrawPreview(name, image, entry.Kind);

			// **What it is used as, which is the caption that carries the
			// information.** See the header.
			ImGui::SetCursorPos(ImVec2(start.x + 4.0f, start.y + image + 8.0f));
			ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::AccentColour());
			ImGui::TextUnformatted(entry.Property.c_str());
			ImGui::PopStyleColor();

			// The asset's own name under it, elided — a store's names are
			// hashes and a tile is a hundred pixels wide.
			ImGui::SetCursorPos(ImVec2(start.x + 4.0f, start.y + image + 8.0f + ImGui::GetTextLineHeight()));
			ImGui::PushStyleColor(ImGuiCol_Text, engine::ui::MutedColour());
			ImGui::TextUnformatted(name.size() > 14 ? (name.substr(0, 12) + "..").c_str() : name.c_str());
			ImGui::PopStyleColor();

			// **Only worth saying when the asset is shared; "1" is just
			// noise.** The reference's rule, unchanged.
			if (entry.Uses > 1) {
				char badge[16];
				std::snprintf(badge, sizeof(badge), "x%zu", entry.Uses);
				const ImVec2 size = ImGui::CalcTextSize(badge);
				ImGui::SetCursorPos(ImVec2(start.x + tileWidth - size.x - 6.0f, start.y + 4.0f));
				ImGui::TextUnformatted(badge);
			}

			ImGui::SetCursorPos(ImVec2(start.x, start.y + tileHeight));
			ImGui::EndGroup();
			ImGui::PopID();

			column = (column + 1) % columns;
		}

		ImGui::EndChild();
	}
}
