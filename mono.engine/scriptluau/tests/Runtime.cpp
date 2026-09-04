// The Luau adapter, used without the factory.
//
// **What this suite is for is the module boundary itself.** Every other
// scripting case reaches a VM through `scripthost::MakeRuntime`, which links
// both adapters - so a Luau adapter that had quietly come to need something out
// of `scriptjs` would still pass all of them. Here nothing but `scriptluau` and
// what it sits on is linked, so an edge that crossed to the other VM is a link
// error rather than a coupling nobody notices.
//
// The behaviour of a Luau runtime is asserted in `engine.scripthost.*`, where it
// is asserted against JavaScript's at the same time. This is deliberately thin.

#include <engine/core/FrameGraph.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/Shaders.hpp>
#include <engine/script/Instances.hpp>
#include <engine/script/SourceCache.hpp>
#include <engine/scriptluau/Runtime.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.scriptluau.runtime")
// A script's vocabulary is the class tree, so a change to it has to re-run this.
TEST_DEPENDS("engine.scene.part")
TEST_DEPENDS("engine.scene.shaders")

using engine::ecs::Store;
using engine::script::Language;
using engine::script::MakeLuauRuntime;

namespace {
	// Registers the class tree, which is what `Instance.new` resolves against.
	// A `Store` is not movable, so this is a call rather than a factory.
	void RegisterClasses() {
		engine::scene::EnsureClassTree();
	}

	engine::ecs::Entity
	StageModule(Store &store, std::string_view path, std::string_view name, std::string_view source) {
		engine::scene::InstallServices(store);
		engine::script::RegisterScriptComponents();
		if (store.Resource<engine::script::SourceCache>() == nullptr) {
			store.SetResource(engine::script::SourceCache{});
		}
		store.ResourceMutable<engine::script::SourceCache>()->Set(engine::core::Name(path), source);
		const engine::ecs::Entity module = engine::script::MakeModule(store, path, name);
		REQUIRE(module != engine::ecs::NULL_ENTITY);
		REQUIRE(store.SetParent(module, engine::scene::WorkspaceOf(store)));
		return module;
	}
}

TEST_CASE("the luau adapter opens a runtime on its own", "[scriptluau]") {
	RegisterClasses();
	Store store("scriptluau_test");

	const auto runtime = MakeLuauRuntime(store);
	REQUIRE(runtime != nullptr);
	CHECK(runtime->Which() == Language::Luau);
	CHECK(runtime->Run("local x = 1 + 1 assert(x == 2, 'arithmetic')"));
}

TEST_CASE("the luau adapter builds into the world it was handed", "[scriptluau]") {
	RegisterClasses();
	Store store("scriptluau_test");

	const auto runtime = MakeLuauRuntime(store);
	REQUIRE(runtime->Run("Instance.new('Part').Name = 'FromLuau'"));

	// Found by walking the world rather than by being handed a handle, because
	// what is being checked is that the adapter reached `script`'s object model
	// with no factory and no second VM in the process.
	engine::ecs::Entity part = engine::ecs::NULL_ENTITY;
	store.Each<const engine::scene::Transform, const engine::scene::Bounds>(
		[&](engine::ecs::Entity entity, const engine::scene::Transform &, const engine::scene::Bounds &) {
			part = entity;
		}
	);

	REQUIRE(part != engine::ecs::NULL_ENTITY);
	CHECK(store.InstanceNameOf(part).Text() == "FromLuau");
}

TEST_CASE("the luau adapter records a completed native binding", "[scriptluau][profile]") {
	RegisterClasses();
	Store store("scriptluau_profile_test");
	const auto runtime = MakeLuauRuntime(store);

	struct Collecting {
		Collecting() {
			engine::core::FrameGraph::SetEnabled(true);
		}
		~Collecting() {
			engine::core::FrameGraph::SetEnabled(false);
		}
	} collecting;

	engine::core::FrameGraph::BeginFrame();
	REQUIRE(runtime->Run(
		"local part = Instance.new('Part') part.Name = 'Profiled' local value = Vector3.new(1, 2, 3) "
		"assert(RunService:IsServer())"
	));
	engine::core::FrameGraph::EndFrame();

	int found = 0;
	bool foundSharedSurface = false;
	for (const engine::core::FrameSpan &span : engine::core::FrameGraph::Spans()) {
		if (span.Name == "binding.luau.new") {
			found++;
			CHECK(span.Category == engine::core::ProfileCategory::Script);
			CHECK_FALSE(span.Reported);
		}
		foundSharedSurface = foundSharedSurface || span.Name == "binding.luau.IsServer";
	}
	CHECK(found >= 2);
	CHECK(foundSharedSurface);
}

TEST_CASE("the luau adapter records source profile samples and yields", "[scriptluau][profile]") {
	RegisterClasses();
	Store store("scriptluau_source_profile_test");
	const auto runtime = MakeLuauRuntime(store);
	REQUIRE(runtime != nullptr);
	runtime->SetScriptProfiling(true);

	REQUIRE(runtime->Run(
		R"(
		local function counted()
			local total = 0
			for index = 1, 128 do
				total += index
			end
			return total
		end
		assert(counted() == 8256)
		task.wait()
		local function resumed()
			local total = 0
			for index = 1, 64 do total += index end
			return total
		end
		assert(resumed() == 2080)
	)",
		"source-profile.luau"
	));
	store.AdvanceTick(store.Time().Delta);
	REQUIRE(runtime->Heartbeat(store.Time().Delta));

	bool foundFunction = false;
	bool foundResumed = false;
	bool foundYield = false;
	bool foundYieldingBinding = false;
	for (const engine::script::ScriptProfileNode &node : runtime->Profile().Nodes()) {
		foundFunction =
			foundFunction || (node.Source == "source-profile.luau" && node.Function == "counted" &&
							  node.Samples > 0 && node.SelfNanoseconds > 0);
		foundResumed = foundResumed || (node.Source == "source-profile.luau" && node.Function == "resumed" &&
										node.Samples > 0 && node.SelfNanoseconds > 0);
		foundYield = foundYield || node.Yields > 0;
		for (const engine::script::ScriptProfileNode::Binding &binding : node.Bindings) {
			foundYieldingBinding =
				foundYieldingBinding || (binding.Name == "binding.luau.wait" && binding.Yields == 1);
		}
	}
	CHECK(foundFunction);
	CHECK(foundResumed);
	CHECK(foundYield);
	CHECK(foundYieldingBinding);
}

TEST_CASE("the source profile nests required modules under their caller", "[scriptluau][profile]") {
	RegisterClasses();
	Store store("scriptluau_required_profile_test");
	StageModule(
		store,
		"profiled-module.luau",
		"ProfiledModule",
		R"(
			local function moduleWork()
				local total = 0
				for index = 1, 128 do total += index end
				return total
			end
			return { Work = moduleWork }
		)"
	);
	const auto runtime = MakeLuauRuntime(store);
	REQUIRE(runtime != nullptr);
	runtime->SetScriptProfiling(true);
	REQUIRE(runtime->Run(
		"local module = require(workspace.ProfiledModule) assert(module.Work() == 8256)", "caller.luau"
	));

	bool nestedModule = false;
	for (const engine::script::ScriptProfileNode &node : runtime->Profile().Nodes()) {
		if (node.Source != "profiled-module.luau" || node.Parent == UINT32_MAX) {
			continue;
		}
		const engine::script::ScriptProfileNode &parent = runtime->Profile().Nodes()[node.Parent];
		nestedModule = parent.Source == "caller.luau";
		if (nestedModule) {
			break;
		}
	}
	CHECK(nestedModule);
}

TEST_CASE("luau refuses virtual classes and still creates their leaves", "[scriptluau]") {
	RegisterClasses();
	Store store("scriptluau_virtual_classes");

	const auto runtime = MakeLuauRuntime(store);
	CHECK_FALSE(runtime->Run("Instance.new('BasePart')"));
	CHECK(runtime->Run("assert(Instance.new('Part'):IsA('BasePart'))"));
}

TEST_CASE("luau creates shader scripts and writes their source", "[scriptluau][shaders]") {
	RegisterClasses();
	engine::scene::ShaderScriptClass();
	Store store("scriptluau_shader_test");

	const auto runtime = MakeLuauRuntime(store);
	REQUIRE(runtime->Run(R"(
		local shader = Instance.new("ShaderScript")
		shader.Name = "Generated"
		local before = shader.Revision
		shader.Source = "#version 450\nvoid main() {}"
		assert(shader.Source == "#version 450\nvoid main() {}", "source did not round-trip")
		assert(shader.Revision > before, "source write did not move the revision")
	)"));

	const engine::scene::ShaderText shader =
		engine::scene::ShaderTextOf(store, engine::core::Name("Generated"));
	REQUIRE(shader.Found);
	CHECK(shader.Code == "#version 450\nvoid main() {}");
	CHECK(shader.Revision > 0);
}
