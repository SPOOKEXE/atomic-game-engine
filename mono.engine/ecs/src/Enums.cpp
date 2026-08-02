#include <engine/ecs/Enums.hpp>

namespace engine::ecs {

	const char *Describe(SnapshotStatus status) {
		switch (status) {
		case SnapshotStatus::Ok:
			return "ok";
		case SnapshotStatus::NotASnapshot:
			return "not a snapshot";
		case SnapshotStatus::WrongVersion:
			return "wrong snapshot version";
		case SnapshotStatus::UnknownComponent:
			return "unknown component";
		case SnapshotStatus::Truncated:
			return "truncated";
		case SnapshotStatus::Unserialisable:
			return "component has no serialisation";
		}
		// Not a default label, so adding a value to the enum is a compiler
		// warning here rather than a string nobody notices is missing.
		return "?";
	}

	const char *Describe(ApplyMode mode) {
		switch (mode) {
		case ApplyMode::Overlay:
			return "overlay";
		case ApplyMode::Authoritative:
			return "authoritative";
		}
		return "?";
	}

	const char *Describe(ComponentKind kind) {
		switch (kind) {
		case ComponentKind::Data:
			return "data";
		case ComponentKind::Tag:
			return "tag";
		}
		return "?";
	}
}
