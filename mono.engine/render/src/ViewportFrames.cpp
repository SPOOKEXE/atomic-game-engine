#include <engine/ecs/Store.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/DrawList.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/render/ViewportFrames.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/DrawInstance.hpp>

#include <algorithm>
#include <cmath>

namespace engine::render {

	namespace {
		constexpr uint32_t MAX_VIEWPORT_EDGE = 2048;
		constexpr int MAX_TREE_DEPTH = 256;

		void CollectViewportInstances(
			const ecs::Store &store, ecs::Entity parent, int depth, std::vector<scene::DrawInstance> &out
		) {
			if (depth > MAX_TREE_DEPTH) {
				return;
			}

			store.EachChild(parent, [&](ecs::Entity child) {
				const auto *placement = store.Get<scene::Transform>(child);
				const auto *bounds = store.Get<scene::Bounds>(child);
				const auto *visual = store.Get<scene::Visual>(child);
				if (placement != nullptr && bounds != nullptr && visual != nullptr) {
					out.push_back(
						scene::MakeDrawInstance(
							placement->Frame,
							*bounds,
							*visual,
							store.Get<scene::SurfaceAppearance>(child),
							store.Get<scene::Tags>(child),
							child.Id,
							nullptr,
							store.Get<scene::CharacterLimb>(child)
						)
					);
				}

				CollectViewportInstances(store, child, depth + 1, out);
			});
		}
	}

	size_t ViewportFrames::Render(
		Renderer &renderer, ecs::Store &store, const gui::DrawList &list, size_t firstSlot
	) {
		Entries.clear();
		std::vector<std::vector<scene::DrawInstance>> instances;
		std::vector<SceneTarget> targets;
		std::vector<View> views;
		instances.reserve(list.Commands.size());
		targets.reserve(list.Commands.size());
		views.reserve(list.Commands.size());
		Entries.reserve(list.Commands.size());
		const scene::WorldLighting baseLighting = renderer.CurrentLighting();

		for (const gui::DrawCommand &command : list.Commands) {
			if (command.Kind != gui::DrawKind::Viewport ||
				std::any_of(Entries.begin(), Entries.end(), [&](const Entry &entry) {
					return entry.Instance == command.Source;
				})) {
				continue;
			}

			const auto *viewport = store.Get<gui::Viewport>(command.Source);
			const auto *placement =
				viewport != nullptr ? store.Get<scene::Transform>(viewport->CurrentCamera) : nullptr;
			const auto *lens =
				viewport != nullptr ? store.Get<scene::Camera>(viewport->CurrentCamera) : nullptr;
			if (viewport == nullptr || placement == nullptr || lens == nullptr) {
				continue;
			}

			const uint32_t width = std::clamp(
				static_cast<uint32_t>(std::ceil(std::max(command.Bounds.Width(), 1.0f))),
				1u,
				MAX_VIEWPORT_EDGE
			);
			const uint32_t height = std::clamp(
				static_cast<uint32_t>(std::ceil(std::max(command.Bounds.Height(), 1.0f))),
				1u,
				MAX_VIEWPORT_EDGE
			);
			const uint32_t imageWidth =
				lens->ImageWidth > 0 && lens->ImageHeight > 0 ? lens->ImageWidth : width;
			const uint32_t imageHeight =
				lens->ImageWidth > 0 && lens->ImageHeight > 0 ? lens->ImageHeight : height;
			const uint32_t maxWidth = lens->MaxImageWidth == 0 ? MAX_VIEWPORT_EDGE : lens->MaxImageWidth;
			const uint32_t maxHeight = lens->MaxImageHeight == 0 ? MAX_VIEWPORT_EDGE : lens->MaxImageHeight;

			const size_t slot = firstSlot + Entries.size();
			instances.emplace_back();
			CollectViewportInstances(store, command.Source, 0, instances.back());
			targets.push_back({std::clamp(imageWidth, 1u, maxWidth), std::clamp(imageHeight, 1u, maxHeight)});

			View view;
			view.CameraFrame = placement->Frame;
			view.Camera = *lens;
			view.Instances = instances.back();
			view.Target = &targets.back();
			view.Slot = slot;
			// Each ViewportFrame owns a miniature scene rooted at itself. Two
			// frames in the same Store are not two cameras on one world, so their
			// world-scoped shadow work must not be shared.
			view.World = command.Source.Id;
			view.WorldName = core::Name("render.viewport-frame");
			view.Lighting = baseLighting;
			view.Lighting.Direction = viewport->LightDirection;
			view.Lighting.Ambient = viewport->Ambient;
			view.Lighting.Direct = viewport->LightColor;
			view.OverrideLighting = true;
			views.push_back(view);

			Entries.push_back(
				Entry{
					command.Source,
					nullptr,
					core::Vector2{1.0f, 1.0f},
					width,
					height,
				}
			);
		}

		(void)renderer.Render(views, EmptyOverlay, nullptr, false);
		for (size_t index = 0; index < Entries.size(); index++) {
			const size_t slot = firstSlot + index;
			const SceneExtent extent = renderer.SceneTextureExtent(slot);
			Entries[index].Texture = renderer.SceneTexture(slot);
			Entries[index].UVMax = core::Vector2{extent.U, extent.V};
		}
		return Entries.size();
	}

	InterfaceImage ViewportFrames::Resolve(ecs::Entity instance) const {
		const auto found = std::find_if(Entries.begin(), Entries.end(), [&](const Entry &entry) {
			return entry.Instance == instance;
		});
		if (found == Entries.end()) {
			return {};
		}

		InterfaceImage image;
		image.Texture = found->Texture;
		image.UVMax = found->UVMax;
		image.Width = found->Width;
		image.Height = found->Height;
		return image;
	}
}
