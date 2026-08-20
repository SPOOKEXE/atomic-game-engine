#include <engine/core/types/TweenInfo.hpp>

namespace engine::core {

	const char *Describe(EasingStyle value) {
		switch (value) {
		case EasingStyle::Linear:
			return "Linear";
		case EasingStyle::Quad:
			return "Quad";
		case EasingStyle::Cubic:
			return "Cubic";
		case EasingStyle::Quart:
			return "Quart";
		case EasingStyle::Quint:
			return "Quint";
		case EasingStyle::Sine:
			return "Sine";
		case EasingStyle::Exponential:
			return "Exponential";
		case EasingStyle::Circular:
			return "Circular";
		case EasingStyle::Back:
			return "Back";
		case EasingStyle::Elastic:
			return "Elastic";
		case EasingStyle::Bounce:
			return "Bounce";
		}
		// No default label, so adding a curve is a warning here.
		return "Linear";
	}

	const char *Describe(EasingDirection value) {
		switch (value) {
		case EasingDirection::In:
			return "In";
		case EasingDirection::Out:
			return "Out";
		case EasingDirection::InOut:
			return "InOut";
		}
		return "Out";
	}
}
