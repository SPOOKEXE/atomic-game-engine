#include <engine/scene/Enums.hpp>

namespace engine::scene {

	const char *Describe(BodyKind kind) {
		switch (kind) {
		case BodyKind::Static:
			return "static";
		case BodyKind::Kinematic:
			return "kinematic";
		case BodyKind::Dynamic:
			return "dynamic";
		}
		// No default label, so adding a kind is a compiler warning here rather
		// than a log line reading "?" that nobody traces back.
		return "?";
	}

	const char *Describe(ShapeKind kind) {
		switch (kind) {
		case ShapeKind::Box:
			return "box";
		case ShapeKind::Sphere:
			return "sphere";
		case ShapeKind::Cylinder:
			return "cylinder";
		case ShapeKind::Hull:
			return "hull";
		case ShapeKind::Mesh:
			return "mesh";
		case ShapeKind::Capsule:
			return "capsule";
		}
		return "?";
	}

	const char *Describe(SurfaceEffect effect) {
		switch (effect) {
		case SurfaceEffect::None:
			return "None";
		case SurfaceEffect::NightVision:
			return "NightVision";
		case SurfaceEffect::Thermal:
			return "Thermal";
		case SurfaceEffect::Cctv:
			return "Cctv";
		case SurfaceEffect::Swirl:
			return "Swirl";
		}
		// No default label, so adding one is a warning here rather than a name
		// nobody registered.
		return "None";
	}

	const char *Describe(NormalId face) {
		// **Capitalised, unlike the two above, because these are not
		// diagnostics.** `Describe(BodyKind)` produces "dynamic" for a log line;
		// these are the members `ecs::EnumTable` registers and the strings a
		// script assigns to `Face`, so they are Roblox's spelling exactly.
		switch (face) {
		case NormalId::Right:
			return "Right";
		case NormalId::Top:
			return "Top";
		case NormalId::Back:
			return "Back";
		case NormalId::Left:
			return "Left";
		case NormalId::Bottom:
			return "Bottom";
		case NormalId::Front:
			return "Front";
		}
		return "?";
	}
}
