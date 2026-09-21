// The QuickJS adapter, used without the factory.
//
// **`scriptluau/tests/Runtime.cpp`'s twin, and for its reason.** Nothing here
// links Luau, so an edge that crossed from this adapter to the other one is a
// link error rather than a coupling that hides behind `MakeRuntime`. The
// behaviour of a JavaScript runtime is asserted in `engine.scripthost.*`, beside
// Luau's.

#include <engine/assets/ContentHash.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/script/DataScriptExecutor.hpp>
#include <engine/scriptjs/Runtime.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.scriptjs.runtime")
// A script's vocabulary is the class tree, so a change to it has to re-run this.
TEST_DEPENDS("engine.scene.part")

using engine::ecs::Store;
using engine::script::Language;
using engine::script::MakeJavaScriptRuntime;

namespace {
	// Registers the class tree, which is what `Instance.new` resolves against.
	// A `Store` is not movable, so this is a call rather than a factory.
	void RegisterClasses() {
		engine::scene::EnsureClassTree();
	}
}

TEST_CASE("the javascript adapter opens a runtime on its own", "[scriptjs]") {
	RegisterClasses();
	Store store("scriptjs_test");

	const auto runtime = MakeJavaScriptRuntime(store);
	REQUIRE(runtime != nullptr);
	CHECK(runtime->Which() == Language::JavaScript);
	CHECK(runtime->CanDiscardForWorldSwap());
	CHECK(runtime->Heartbeat(1.0f / 60.0f));
	CHECK(runtime->CanDiscardForWorldSwap());
	CHECK(runtime->Run("if (1 + 1 !== 2) throw new Error('arithmetic');"));
	CHECK_FALSE(runtime->CanDiscardForWorldSwap());
}

TEST_CASE("the javascript adapter builds into the world it was handed", "[scriptjs]") {
	RegisterClasses();
	Store store("scriptjs_test");

	const auto runtime = MakeJavaScriptRuntime(store);
	REQUIRE(runtime->Run("Instance.new('Part').Name = 'FromJavaScript';"));
}

TEST_CASE("javascript package runtime refuses source before execution", "[scriptjs][data-script-package]") {
	RegisterClasses();
	Store store("scriptjs_package");
	const auto runtime = MakeJavaScriptRuntime(
		store,
		{
			.DataCapture = {},
			.DataLifecycle = {},
			.MemoryBytes = 64u * 1024u * 1024u,
			.StepBudget = 200u * 1000u * 1000u,
			.JobBudget = 100u * 1000u,
			.Role = engine::script::HostRole::OfServer(),
			.Origin = engine::script::ScriptOrigin::Game,
			.Capabilities = engine::script::ScriptCapabilities::None,
			.PackageOnly = true,
		}
	);
	engine::script::DataScriptPackage package;
	const engine::script::DataScriptPackageContext context(package, {});
	const auto result =
		runtime->RunDataScriptPackage(context, "throw new Error('must not run');", "package.js");
	CHECK(result.Terminal == engine::script::DataScriptPackageRunResult::State::Failed);
	CHECK(result.Error == "javascript data-script packages are unsupported");
	CHECK(runtime->LastError().empty());
	CHECK_FALSE(runtime->CanDiscardForWorldSwap());
}

TEST_CASE("javascript refuses virtual classes and still creates their leaves", "[scriptjs]") {
	RegisterClasses();
	Store store("scriptjs_virtual_classes");

	const auto runtime = MakeJavaScriptRuntime(store);
	CHECK_FALSE(runtime->Run("Instance.new('BasePart');"));
	CHECK(runtime->Run("if (!Instance.new('Part').IsA('BasePart')) throw new Error('hierarchy');"));
}

TEST_CASE("the javascript adapter refuses a non-instance parent before creation", "[scriptjs]") {
	RegisterClasses();
	Store store("scriptjs_test");

	const auto runtime = MakeJavaScriptRuntime(store);
	CHECK_FALSE(runtime->Run("Instance.new('Part', 7);"));

	int parts = 0;
	store.EachEntity([&](engine::ecs::Entity entity) {
		if (store.ClassOf(entity) == engine::scene::PartClass()) {
			++parts;
		}
	});
	CHECK(parts == 0);
}
