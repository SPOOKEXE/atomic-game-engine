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

#include <engine/assets/ContentHash.hpp>
#include <engine/core/FrameGraph.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/Shaders.hpp>
#include <engine/script/DataScriptExecutor.hpp>
#include <engine/script/Instances.hpp>
#include <engine/script/SourceCache.hpp>
#include <engine/scriptluau/Runtime.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <string_view>

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

	engine::script::DataScriptPackageRunResult RunFreshPackage(std::string_view source) {
		Store store("scriptluau_package_fresh");
		engine::scene::InstallServices(store);
		const auto runtime = MakeLuauRuntime(
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
		return runtime->RunDataScriptPackage(context, source, "package.luau");
	}

	void CheckPackageRefusal(std::string_view source, std::string_view reason) {
		const auto result = RunFreshPackage(source);
		CHECK(result.Terminal == engine::script::DataScriptPackageRunResult::State::Failed);
		CHECK(result.Error.find(reason) != std::string::npos);
	}
}

TEST_CASE("the luau adapter opens a runtime on its own", "[scriptluau]") {
	RegisterClasses();
	Store store("scriptluau_test");

	const auto runtime = MakeLuauRuntime(store);
	REQUIRE(runtime != nullptr);
	CHECK(runtime->Which() == Language::Luau);
	CHECK(runtime->CanDiscardForWorldSwap());
	CHECK(runtime->Heartbeat(1.0f / 60.0f));
	CHECK(runtime->CanDiscardForWorldSwap());
	CHECK(runtime->Run("local x = 1 + 1 assert(x == 2, 'arithmetic')"));
	CHECK_FALSE(runtime->CanDiscardForWorldSwap());
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

TEST_CASE("luau package runtime exposes only immutable package data", "[scriptluau][data-script-package]") {
	RegisterClasses();
	Store store("scriptluau_package");
	const auto runtime = MakeLuauRuntime(
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
	package.Seed = UINT64_MAX;
	package.Parameters.push_back({
		"count",
		{
			.Type = engine::script::DataScriptScalar::Kind::Integer,
			.Boolean = false,
			.Integer = INT64_MIN,
			.Number = 0.0,
			.String = {},
		},
	});
	std::vector<engine::script::DataScriptAssetInput> assets{{"input.bin", {std::byte{1}, std::byte{2}}}};
	package.Assets.push_back({"input.bin", engine::assets::Hasher::Of(assets.front().Bytes)});
	const engine::script::DataScriptPackageContext context(package, assets);
	const auto result = runtime->RunDataScriptPackage(
		context,
		"-- task.wait Promise Debris:AddItem in a harmless comment\nlocal text = 'task.wait TweenService "
		"ContextActionService RunService' assert(Package.parameters.count == '-9223372036854775808') "
		"assert(Package.seed == '18446744073709551615') assert(#Package.asset('input.bin') == 2) "
		"assert(Package.seedStream('same') == Package.seedStream('same')) "
		"local ok = pcall(function() Package.parameters.count = 5 end) assert(not ok)",
		"package.luau"
	);
	CHECK(result.Terminal == engine::script::DataScriptPackageRunResult::State::Completed);
}

TEST_CASE(
	"luau package source checking refuses syntax and strict type errors", "[scriptluau][data-script-package]"
) {
	std::string error;
	CHECK(
		engine::script::CheckLuauDataScriptPackageSource(
			"local count: number = 4\nlocal bytes: string = Package.asset('input.bin')\n"
			"local part = Instance.new('Part')\npart.Parent = workspace\nreturn count",
			"package.luau",
			error
		)
	);
	CHECK(error.empty());
	CHECK(
		engine::script::CheckLuauDataScriptPackageSource(
			"local lighting: Lighting = game:GetService('Lighting')\n"
			"lighting.Ambient = Color3.new(0.03, 0.04, 0.06)\n"
			"lighting.Brightness = 2.5\nlighting.ClockTime = 14",
			"package.luau",
			error
		)
	);
	CHECK(error.empty());

	CHECK_FALSE(engine::script::CheckLuauDataScriptPackageSource("local =", "package.luau", error));
	CHECK(error.starts_with("data-script package syntax error at "));
	CHECK(error.size() <= 512);

	CHECK_FALSE(
		engine::script::CheckLuauDataScriptPackageSource(
			"--!nonstrict\nlocal count: number = 'wrong'", "package.luau", error
		)
	);
	CHECK(error.starts_with("data-script package type error at "));
	CHECK(error.size() <= 512);
}

TEST_CASE(
	"luau package source checking matches the package-only vocabulary", "[scriptluau][data-script-package]"
) {
	std::string error;
	CHECK(
		engine::script::CheckLuauDataScriptPackageSource(
			"local axis: EnumItem = Enum.Axis.X\n"
			"local two: Vector2 = Vector2.new(1, 2)\n"
			"local vector: Vector3 = Vector3.new(1, 2, 3) + Vector3.one\n"
			"local dot: number = vector:Dot(Vector3.xAxis)\n"
			"local frame: CFrame = CFrame.new(vector)\n"
			"local size: UDim2 = UDim2.fromScale(1, 1)\n"
			"local range: NumberRange = NumberRange.new(1, 2)\n"
			"local sequence: NumberSequence = NumberSequence.new({NumberSequenceKeypoint.new(0, 1), "
			"NumberSequenceKeypoint.new(1, 0)})\n"
			"local colours: ColorSequence = ColorSequence.new(Color3.new())\n"
			"local ray: Ray = Ray.new(Vector3.zero, Vector3.one)\n"
			"local random: Random = Random.new(4)\n"
			"local date = DateTime.fromSimulated()\nlocal timestamp: number = date.UnixTimestamp\n"
			"local part = Instance.new('Part')\npart.Size = vector\npart.Anchored = true\npart.Parent = "
			"workspace\nlocal viaGame: Workspace = game:GetService('Workspace')\n"
			"return random:NextInteger(1, 2) + dot",
			"package.luau",
			error
		)
	);
	CHECK(error.empty());

	for (const std::string_view source :
		 {"return require('x')",
		  "return os.clock()",
		  "return debug.traceback()",
		  "return Scope.new()",
		  "return task.wait()",
		  "return task.cancel(nil)",
		  "return wait()",
		  "return spawn(function() end)",
		  "return delay(1, function() end)",
		  "return game:GetService('RunService')",
		  "return RunService:IsServer()"}) {
		CHECK_FALSE(engine::script::CheckLuauDataScriptPackageSource(source, "package.luau", error));
		CHECK(error.starts_with("data-script package type error at "));
	}

	std::string oversized(engine::script::DATA_SCRIPT_PACKAGE_MAX_SOURCE_BYTES + 1, ' ');
	CHECK_FALSE(engine::script::CheckLuauDataScriptPackageSource(oversized, "package.luau", error));
	CHECK(error == "data-script package source exceeds the static-check byte limit");
}

TEST_CASE(
	"luau packages refuse every deferred boundary in a fresh runtime", "[scriptluau][data-script-package]"
) {
	RegisterClasses();
	const auto lighting = RunFreshPackage(
		"local lighting = game:GetService('Lighting')\n"
		"lighting.Ambient = Color3.new(0.03, 0.04, 0.06)\n"
		"lighting.Brightness = 2.5\nlighting.ClockTime = 14\n"
		"assert(lighting.Brightness == 2.5 and lighting.ClockTime == 14)"
	);
	CHECK(lighting.Terminal == engine::script::DataScriptPackageRunResult::State::Completed);
	CheckPackageRefusal("task.wait()", "data-script packages may not schedule deferred work");
	CheckPackageRefusal("task.defer(function() end)", "data-script packages may not schedule deferred work");
	CheckPackageRefusal("task.spawn(function() end)", "data-script packages may not schedule deferred work");
	CheckPackageRefusal(
		"task.delay(1, function() end)", "data-script packages may not schedule deferred work"
	);
	CheckPackageRefusal("workspace:WaitForChild('never', 1)", "data-script packages may not suspend");
	CheckPackageRefusal(
		"workspace.ChildAdded:Connect(function() end)", "data-script packages may not retain callbacks"
	);
	CheckPackageRefusal(
		"game:GetService('Debris'):AddItem(Instance.new('Part'), 1)",
		"data-script packages may not use services"
	);
	CheckPackageRefusal(
		"game:GetService('TweenService'):Create(Instance.new('Part'), TweenInfo.new(), {})",
		"data-script packages may not use services"
	);
	CheckPackageRefusal(
		"local part = Instance.new('Part')\npart:TweenSize(Vector3.new(1, 1, 1))",
		"data-script packages may not create tweens"
	);
	CheckPackageRefusal(
		"local part = Instance.new('Part')\npart:TweenPosition(Vector3.new(1, 1, 1))",
		"data-script packages may not create tweens"
	);
	CheckPackageRefusal(
		"game:GetService('ContextActionService'):BindAction('jump', function() end, false)",
		"data-script packages may not use services"
	);
	CheckPackageRefusal(
		"game:GetService('RunService').Heartbeat:Connect(function() end)",
		"data-script packages may not use services"
	);
	const auto scope = RunFreshPackage("assert(Scope == nil)");
	CHECK(scope.Terminal == engine::script::DataScriptPackageRunResult::State::Completed);
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
