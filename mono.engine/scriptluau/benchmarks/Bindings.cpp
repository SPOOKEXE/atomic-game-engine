// Costs at the C++ to Luau boundary.
//
// The body of each row is the script-visible call, not a direct C++ helper.
// That keeps VM dispatch, argument conversion and the binding itself together,
// which is the cost a game pays. Setup is deliberately outside the script loop
// only where the loop would otherwise measure world construction instead.

#include <engine/ecs/Store.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/script/Runtime.hpp>
#include <engine/scriptluau/Runtime.hpp>
#include <engine/testing/Bench.hpp>

#include <string_view>

TEST_SUITE_ID("engine.scriptluau.bench.bindings")

using engine::ecs::Store;
using engine::script::MakeLuauRuntime;
using engine::testing::Consume;

namespace {

	// The vocabulary a binding resolves through belongs to the class tree, not
	// the benchmark fixture. Calling this repeatedly is idempotent and keeps
	// each benchmark independently runnable.
	void Prepare() {
		engine::scene::EnsureClassTree();
		engine::scene::RegisterSceneComponents();
	}

	bool Run(Store &store, std::string_view source) {
		const auto runtime = MakeLuauRuntime(store);
		return runtime != nullptr && runtime->Run(source);
	}

	// A complete async lifecycle: submit from Luau, poll at a heartbeat, then
	// resume into Luau. Serial is intentional here. It measures the binding's
	// lifecycle without scheduling noise from a worker machine.
	bool RunNoiseGrid(Store &store) {
		const auto runtime = MakeLuauRuntime(store);
		if (runtime == nullptr || !runtime->Run(R"(
			local marker = Instance.new('Part')
			marker.Name = 'Pending'
			marker.Parent = workspace
			local values = ComputeService:NoiseGridAsync(32, 16, 0.125, 0.75, 0.03125, 'Serial', -0.375)
			assert(#values == 512)
			marker.Name = 'Complete'
		)")) {
			return false;
		}

		for (int tick = 0; tick < 3; tick++) {
			store.ClearChanges();
			store.AdvanceTick(store.Time().Delta);
			if (!runtime->Heartbeat(store.Time().Delta)) {
				return false;
			}
			store.FlushSignals();
		}

		const engine::ecs::Entity workspace = engine::scene::WorkspaceOf(store);
		return workspace != engine::ecs::NULL_ENTITY &&
			   store.FindFirstChild(workspace, "Complete") != engine::ecs::NULL_ENTITY;
	}
}

BENCH("C++ to Luau · 10000 Instance.new calls", 20) {
	Prepare();
	for (int pass = 0; pass < 20; pass++) {
		Store store("bench.luau.new");
		Consume(Run(store, "for index = 1, 10000 do Instance.new('Part') end") ? 1 : 0);
	}
}

BENCH("C++ to Luau · 10000 property reads", 20) {
	Prepare();
	for (int pass = 0; pass < 20; pass++) {
		Store store("bench.luau.read");
		Consume(
			Run(store, R"(
			local part = Instance.new('Part')
			local total = 0
			for index = 1, 10000 do total += part.Position.X end
			assert(total == 0)
		)")
				? 1
				: 0
		);
	}
}

BENCH("C++ to Luau · 10000 property writes", 20) {
	Prepare();
	for (int pass = 0; pass < 20; pass++) {
		Store store("bench.luau.write");
		Consume(
			Run(store, R"(
			local part = Instance.new('Part')
			for index = 1, 10000 do part.Position = Vector3.new(index, 0, 0) end
		)")
				? 1
				: 0
		);
	}
}

BENCH("C++ to Luau · 10000 shared service calls", 20) {
	Prepare();
	for (int pass = 0; pass < 20; pass++) {
		Store store("bench.luau.service");
		Consume(
			Run(store, R"(
			local run = game:GetService('RunService')
			local calls = 0
			for index = 1, 10000 do
				if run:IsServer() then calls += 1 end
			end
			assert(calls == 10000)
		)")
				? 1
				: 0
		);
	}
}

BENCH("C++ to Luau · 10000 service property reads and writes", 20) {
	Prepare();
	for (int pass = 0; pass < 20; pass++) {
		Store store("bench.luau.service_property");
		Consume(
			Run(store, R"(
			local input = game:GetService('UserInputService')
			for index = 1, 10000 do
				input.MouseIconEnabled = index % 2 == 0
				local enabled = input.MouseIconEnabled
			end
		)")
				? 1
				: 0
		);
	}
}

BENCH("C++ to Luau · 10000 signal connection lifecycles", 20) {
	Prepare();
	for (int pass = 0; pass < 20; pass++) {
		Store store("bench.luau.signal");
		Consume(
			Run(store, R"(
			for index = 1, 10000 do
				local connection = RunService.Heartbeat:Connect(function() end)
				connection:Disconnect()
			end
		)")
				? 1
				: 0
		);
	}
}

BENCH("C++ to Luau · NoiseGridAsync start poll end", 20) {
	Prepare();
	for (int pass = 0; pass < 20; pass++) {
		Store store("bench.luau.compute");
		Consume(RunNoiseGrid(store) ? 1 : 0);
	}
}
