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
							store.Get<scene::Tags>(child)
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
		const core::Vector3 previousSun = renderer.SunDirection();
		const core::Color3 previousAmbient = renderer.SunAmbient();
		const core::Color3 previousDirect = renderer.SunColor();

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

			Instances.clear();
			CollectViewportInstances(store, command.Source, 0, Instances);
			renderer.SetSun(viewport->LightDirection, viewport->Ambient, viewport->LightColor);

			const size_t slot = firstSlot + Entries.size();
			const SceneTarget target{width, height};
			(void)renderer.Render(
				placement->Frame,
				*lens,
				Instances,
				EmptyOverlay,
				{},
				nullptr,
				&target,
				slot,
				{},
				{},
				{},
				{},
				{},
				{},
				false
			);

			const SceneExtent extent = renderer.SceneTextureExtent(slot);
			Entries.push_back(
				Entry{
					command.Source,
					renderer.SceneTexture(slot),
					core::Vector2{extent.U, extent.V},
					width,
					height,
				}
			);
		}

		renderer.SetSun(previousSun, previousAmbient, previousDirect);
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
