#pragma once

// Dependency-light feature factories for a control surface.
//
// A product states its MCP vocabulary as one ordered list. Features in this
// header need only the control module itself. Features that expose worlds or
// scripting live in their own headers so a content origin can omit them from
// both its list and its link closure.
//
// @tier shared

#include <engine/control/Surface.hpp>

#include <utility>

namespace engine::control::features {

	// The checked module graph and layer rules.
	inline Feature Architecture() {
		return Feature{"architecture", [](Surface &surface) { surface.AddArchitectureTools(); }};
	}

	// Process logs, metric counters, and their controls.
	inline Feature Diagnostics() {
		return Feature{"diagnostics", [](Surface &surface) { surface.AddDiagnosticTools(); }};
	}

	// The asynchronous test runner tools.
	inline Feature Build() {
		return Feature{"build", [](Surface &surface) { surface.AddBuildTools(); }};
	}

	// Architecture and checkout-backed context resources.
	inline Feature Resources() {
		return Feature{"resources", [](Surface &surface) { surface.AddStandardResources(); }};
	}

	// Repository workflow prompts.
	inline Feature Prompts() {
		return Feature{"prompts", [](Surface &surface) { surface.AddStandardPrompts(); }};
	}

	// A product-owned extension. One installer may add or replace any number of
	// tools, resources, and prompts through the same registry as engine features.
	inline Feature Custom(std::string name, std::function<void(Surface &)> install) {
		return Feature{std::move(name), std::move(install)};
	}
}
