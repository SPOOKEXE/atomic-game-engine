// The asynchronous compute surface through both VMs and all three contexts.

#include <engine/core/Paths.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/parallel/ProcessChannel.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/script/ComputeJobs.hpp>
#include <engine/script/Runtime.hpp>
#include <engine/scripthost/Runtime.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>

TEST_SUITE_ID("engine.scripthost.computeservice")
TEST_DEPENDS("engine.script.computejobs")

using engine::ecs::Store;
using engine::script::Language;
using engine::script::MakeRuntime;

namespace {
	std::filesystem::path Self() {
		return engine::core::Paths::Base() / engine::core::Paths::Program("test_scripthost");
	}

	bool HasNamed(Store &store, const std::string &name) {
		const engine::ecs::Entity workspace = engine::scene::WorkspaceOf(store);
		return workspace != engine::ecs::NULL_ENTITY &&
			   store.FindFirstChild(workspace, name) != engine::ecs::NULL_ENTITY;
	}

	void AwaitNamed(Store &store, engine::script::Runtime &runtime, const std::string &name) {
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
		while (!HasNamed(store, name) && std::chrono::steady_clock::now() < deadline) {
			store.ClearChanges();
			store.AdvanceTick(store.Time().Delta);
			INFO(runtime.LastError());
			REQUIRE(runtime.Heartbeat(store.Time().Delta));
			store.FlushSignals();
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		REQUIRE(HasNamed(store, name));
	}
}

TEST_CASE("compute service worker child", "[.child]") {
	if (!engine::parallel::HasInheritedChannel()) {
		return;
	}
	CHECK(engine::script::RunComputeWorker() == 0);
}

TEST_CASE("noise grids resume at a heartbeat in every context and language", "[scripting][compute]") {
	engine::scene::EnsureClassTree();
	engine::scene::RegisterSceneComponents();
	engine::script::ConfigureComputeWorkerProgram(Self(), {"compute service worker child"});

	for (const Language language : {Language::Luau, Language::JavaScript}) {
		for (const char *context : {"Serial", "Threaded", "Processed"}) {
			CAPTURE(context, static_cast<int>(language));
			Store store("compute_service");
			const auto runtime = MakeRuntime(store, language);
			REQUIRE(runtime != nullptr);
			const std::string name =
				std::string(context) + (language == Language::Luau ? "Luau" : "JavaScript");

			std::string source;
			if (language == Language::Luau) {
				source = "local marker = Instance.new('Part'); marker.Name = 'Pending'; marker.Parent = "
						 "workspace; "
						 "local values = ComputeService:NoiseGridAsync(32, 16, 0.125, 0.75, 0.03125, '" +
						 std::string(context) +
						 "', -0.375); assert(#values == 512); assert(values[1] == math.noise(0.125, -0.375, "
						 "0.75)); assert(math.abs(values[18]) > 0.001); marker.Name = '" +
						 name + "'";
			} else {
				source =
					"const marker = Instance.new('Part'); marker.Name = 'Pending'; marker.Parent = "
					"workspace; "
					"(async () => { const values = await ComputeService.NoiseGridAsync(32, 16, 0.125, 0.75, "
					"0.03125, '" +
					std::string(context) +
					"', -0.375); if (values.length !== 512 || Math.abs(values[17]) <= 0.001) throw new "
					"Error('bad noise grid'); marker.Name = '" +
					name + "'; })();";
			}

			INFO(source);
			const bool ran = runtime->Run(source);
			INFO(runtime->LastError());
			REQUIRE(ran);
			CHECK_FALSE(HasNamed(store, name));
			AwaitNamed(store, *runtime, name);
		}
	}

	engine::script::ConfigureComputeWorkerProgram({});
}
