#pragma once

// Host-only mutation of the frozen case inventory. Public consumers read it
// through Suite.hpp without depending on Catch or its mutable configuration.

#include <engine/testing/Suite.hpp>

namespace engine::testing::detail {
	void FreezeSelectedCases(std::vector<SelectedCase> selected);
}
