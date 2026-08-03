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
		}
		return "?";
	}
}
