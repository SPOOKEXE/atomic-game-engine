#pragma once

// MCP adapter for a verified Luau data-script package.

#include <engine/control/Surface.hpp>
#include <engine/script/DataScriptExecutor.hpp>

#include <functional>

namespace engine::control {

	// Product callback that runs a verified package after this adapter validates MCP input.
	using DataScriptPackageExecutor =
		std::function<engine::script::DataScriptResult(const engine::script::DataScriptRequest &)>;

	// Adds the host Luau package row. The executor owns the driver-thread and
	// runtime lifetime rules, while this adapter owns untrusted JSON and replay.
	void AddDataScriptPackageTool(engine::control::Surface &surface, DataScriptPackageExecutor execute);
}
