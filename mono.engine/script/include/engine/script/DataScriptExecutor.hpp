#pragma once

#include <engine/script/Runtime.hpp>
#include <engine/world/DataFactory.hpp>

#include <functional>
#include <string>
#include <string_view>

namespace engine::script {
	struct DataScriptRequest {
		std::string InstanceId;
		std::string Source;
		std::string SourceHash;
		std::string Name = "mcp";
		uint64_t ExpectedTick = 0;
		uint64_t ExpectedEpoch = 0;
		uint64_t ExpectedVersion = 0;
	};

	struct DataScriptResult {
		world::DataFactoryReply Lifecycle;
		bool Ran = false;
		bool Atomic = false;
		std::string Error;
	};

	using DataScriptRuntimeResolver = std::function<Runtime *(world::WorldId)>;

	DataScriptResult ExecuteDataScript(
		world::Universe &universe,
		world::DataFactorySession &session,
		const DataScriptRequest &request,
		const DataScriptRuntimeResolver &runtimeOf
	);
}
