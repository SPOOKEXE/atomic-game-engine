#include <engine/world/Enums.hpp>

namespace engine::world {

	const char *Describe(WorldState state) {
		switch (state) {
		case WorldState::Active:
			return "active";
		case WorldState::Idle:
			return "idle";
		case WorldState::Suspended:
			return "suspended";
		case WorldState::Remote:
			return "remote";
		case WorldState::Faulted:
			return "faulted";
		}
		// No default label, so adding a state is a compiler warning here rather
		// than a log line reading "?" that nobody traces back.
		return "?";
	}

	const char *Describe(Isolation isolation) {
		switch (isolation) {
		case Isolation::Shared:
			return "shared";
		case Isolation::Dedicated:
			return "dedicated";
		}
		return "?";
	}

	const char *Describe(ExecutionMode mode) {
		switch (mode) {
		case ExecutionMode::WorldParallel:
			return "world-parallel";
		case ExecutionMode::WorldSerial:
			return "world-serial";
		}
		return "?";
	}

	const char *Describe(WorldStatus status) {
		switch (status) {
		case WorldStatus::Ok:
			return "ok";
		case WorldStatus::NoSuchWorld:
			return "no such world";
		case WorldStatus::NameTaken:
			return "name already taken";
		case WorldStatus::NoName:
			return "world has no name";
		case WorldStatus::WrongThread:
			return "wrong thread";
		}
		return "?";
	}
}
