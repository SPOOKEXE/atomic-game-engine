#include <engine/ecs/Store.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/LocalLight.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace engine::scene {
	LocalLightRejection ResolveLocalLight(
		const ecs::Store &store, ecs::Entity entity, const Light &light, ResolvedLocalLight &resolved
	) {
		if (!light.Enabled) return LocalLightRejection::Disabled;
		if (light.Brightness <= 0.0f) return LocalLightRejection::NonPositiveBrightness;
		if (light.Range <= 0.0f) return LocalLightRejection::NonPositiveRange;

		const ecs::Entity parent = store.ParentOf(entity);
		if (parent == ecs::NULL_ENTITY) return LocalLightRejection::MissingParent;

		core::CFrame frame;
		if (const auto *attachment = store.Get<Attachment>(parent))
			frame = attachment->WorldFrame;
		else if (const auto *transform = store.Get<Transform>(parent))
			frame = transform->Frame;
		else
			return LocalLightRejection::ParentHasNoPlacement;

		resolved.Position = frame.Position;
		resolved.Range = light.Range;
		resolved.Colour = {
			light.Colour.R * light.Brightness,
			light.Colour.G * light.Brightness,
			light.Colour.B * light.Brightness,
		};
		resolved.ConeCosine = -1.0f;
		if (light.Kind != LightKind::Point) {
			resolved.Direction = frame.VectorToWorldSpace(NormalOf(light.Face));
			resolved.ConeCosine =
				std::cos(std::clamp(light.Angle, 0.0f, 180.0f) * 0.5f * std::numbers::pi_v<float> / 180.0f);
		}
		return LocalLightRejection::None;
	}

	const char *Describe(LocalLightRejection rejection) {
		switch (rejection) {
		case LocalLightRejection::None:
			return "";
		case LocalLightRejection::Disabled:
			return "disabled";
		case LocalLightRejection::NonPositiveBrightness:
			return "non_positive_brightness";
		case LocalLightRejection::NonPositiveRange:
			return "non_positive_range";
		case LocalLightRejection::MissingParent:
			return "missing_parent";
		case LocalLightRejection::ParentHasNoPlacement:
			return "parent_has_no_placement";
		}
		return "unknown";
	}
}
