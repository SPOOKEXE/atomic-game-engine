// The QuickJS adapter, used without the factory.
//
// **`scriptluau/tests/Runtime.cpp`'s twin, and for its reason.** Nothing here
// links Luau, so an edge that crossed from this adapter to the other one is a
// link error rather than a coupling that hides behind `MakeRuntime`. The
// behaviour of a JavaScript runtime is asserted in `engine.scripthost.*`, beside
// Luau's.

#include <engine/assets/ContentHash.hpp>
#include <engine/ecs/Schema.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/script/DataScriptExecutor.hpp>
#include <engine/script/Instances.hpp>
#include <engine/script/SourceCache.hpp>
#include <engine/scriptjs/Runtime.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

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

TEST_CASE(
	"javascript script side survives interleaved callbacks and async jobs", "[scriptjs][client-local]"
) {
	using namespace engine;
	RegisterClasses();
	script::ScriptClass();
	Store store("scriptjs_shared_roles");
	const ecs::FieldSpec flag{"Enabled", ecs::PropertyType::Bool};
	REQUIRE(ecs::Schemas::Register("scriptjs.RoleProbe", {&flag, 1}).Why == ecs::Schemas::Status::Ok);
	const auto reserved = store.Create("reserved");
	const auto predictedReserved = store.CreatePredicted("reserved-predicted");
	const auto authority = store.CreateInstance(scene::PartClass(), "Authority");
	const auto server = store.CreateInstance(script::ScriptClass(), "server");
	const auto client = store.CreateInstance(script::LocalScriptClass(), "client");
	const core::Name path("shared-side.js");
	for (const auto entity : {server, client}) {
		store.Set(entity, script::JavaScriptSourceContainer{path});
		store.Set(entity, script::CodeSourceContainerSelector{Language::JavaScript});
	}
	script::SourceCache cache;
	cache.Set(path, R"(
const side = script.Name;
const run = game.GetService('RunService');
if (World.DefineComponent('scriptjs.RoleProbe', {Enabled: 'bool'}))
    throw new Error('existing type was recreated');
const raw = World.CreateEntity(side + '.raw');
raw.SetComponent('scriptjs.RoleProbe', {Enabled: true});
raw.RemoveComponent('scriptjs.RoleProbe');
raw.SetComponent('scriptjs.RoleProbe', {Enabled: true});
if (!raw.GetComponent('scriptjs.RoleProbe').Enabled) throw new Error('owned ECS write failed');
function make(stage) {
    const part = Instance.new('Part', workspace);
    part.Name = side + '.' + stage;
    part.Color = Color3.new(0.25, 0.5, 0.75);
}
make('initial');
Promise.resolve().then(() => make('promise'));
task.defer(() => make('defer'));
task.spawn(async () => { await task.wait(0); make('await'); });
let fired = false;
run.Heartbeat.Connect(() => {
    if (fired) return;
    fired = true;
    make('event');
    if (run.IsClient() !== (side === 'client') || run.IsServer() !== (side === 'server'))
        throw new Error('callback side drifted');
    const authority = workspace.FindFirstChild('Authority');
    if (side === 'client') {
        let refused = false;
        try { authority.Color = Color3.new(1, 0, 0); } catch (_) { refused = true; }
        if (!refused) throw new Error('client wrote authority');
        for (const mutate of [
            () => authority.SetComponent('scriptjs.RoleProbe', {Enabled: true}),
            () => authority.RemoveComponent('scriptjs.RoleProbe'),
            () => raw.SetComponent('ecs.ClientLocal'),
            () => raw.RemoveComponent('ecs.ClientLocal'),
            () => raw.SetComponent('ecs.Hierarchy', {}),
            () => World.CreateEntity('reserved'),
            () => World.CreateEntity('reserved-predicted'),
            () => World.DefineComponent('scriptjs.ClientForbidden', {Enabled: 'bool'}),
            () => World.DefineComponent('scriptjs.RoleProbe', {Enabled: 'float'}),
            () => World.SetComponentTags('scriptjs.RoleProbe', ['private']),
            () => World.SetComponentFieldTags('scriptjs.RoleProbe', 'Enabled', ['private']),
            () => World.ExposeComponentField('scriptjs.RoleProbe', 'Enabled', false)
        ]) {
            let denied = false;
            try { mutate(); } catch (_) { denied = true; }
            if (!denied) throw new Error('client crossed ECS ownership boundary');
        }
        const bypass = globalThis.Instance.new('Part', workspace);
        bypass.Name = 'client.global-constructor';
    } else {
        authority.Color = Color3.new(0, 1, 0);
    }
});
)");
	store.SetResource(cache);
	const auto runtime = MakeJavaScriptRuntime(store, {.Role = script::HostRole::OfBoth()});
	const auto workspace = store.FindFirstRoot("Workspace");
	REQUIRE(workspace != ecs::NULL_ENTITY);
	REQUIRE(store.SetParent(authority, workspace));
	const bool clientFirst = GENERATE(false, true);
	INFO("client first=" << clientFirst);
	REQUIRE(runtime->RunInstance(clientFirst ? client : server));
	INFO(runtime->LastError());
	REQUIRE(runtime->RunInstance(clientFirst ? server : client));
	for (int tick = 0; tick < 3; ++tick) {
		store.AdvanceTick(1.0f / 60);
		INFO(runtime->LastError());
		REQUIRE(runtime->Heartbeat(1.0f / 60));
	}
	for (const auto side : {"server", "client"}) {
		const auto raw = store.Find(std::string(side) + ".raw");
		REQUIRE(raw != ecs::NULL_ENTITY);
		CHECK(ecs::Store::IsPredicted(raw) == (std::string_view(side) == "client"));
		CHECK(store.Has<ecs::ClientLocal>(raw) == (std::string_view(side) == "client"));
		for (const auto stage : {"initial", "promise", "defer", "await", "event"}) {
			const std::string name = std::string(side) + '.' + stage;
			const auto entity = store.FindFirstChild(workspace, name);
			INFO(name);
			REQUIRE(entity != ecs::NULL_ENTITY);
			CHECK(ecs::Store::IsPredicted(entity) == (std::string_view(side) == "client"));
			CHECK(store.Has<ecs::ClientLocal>(entity) == (std::string_view(side) == "client"));
		}
	}
	const auto bypass = store.FindFirstChild(workspace, "client.global-constructor");
	REQUIRE(bypass != ecs::NULL_ENTITY);
	CHECK(store.Has<ecs::ClientLocal>(bypass));
	CHECK(ecs::Store::IsPredicted(bypass));
	CHECK_FALSE(store.Has<ecs::ClientLocal>(reserved));
	CHECK_FALSE(store.Has<ecs::ClientLocal>(predictedReserved));
	CHECK(ecs::Schemas::Find(core::Name("scriptjs.ClientForbidden")) == nullptr);
	const auto *colour = store.Get<scene::Visual>(authority);
	REQUIRE(colour != nullptr);
	CHECK(colour->Tint.G == 1);
	CHECK(colour->Tint.R == 0);
}
