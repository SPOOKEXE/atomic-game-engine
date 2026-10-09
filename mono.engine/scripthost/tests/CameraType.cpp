// Both adapters project camera control onto the same world-local resource.

#include <engine/core/Name.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Instance.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Controls.hpp>
#include <engine/scene/Input.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/SurfaceCameras.hpp>
#include <engine/script/Instances.hpp>
#include <engine/script/SourceCache.hpp>
#include <engine/scripthost/Runtime.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>

TEST_SUITE_ID("engine.scripthost.camera-type")
TEST_DEPENDS("engine.scene.part")

TEST_CASE(
	"the current camera's Scriptable mode survives controller updates in both VMs", "[script][camera]"
) {
	using engine::core::Name;
	using engine::ecs::Store;
	using engine::scene::CameraController;
	using engine::scene::CameraMode;
	using engine::script::Language;
	const Language language = GENERATE(Language::Luau, Language::JavaScript);
	const bool replica = GENERATE(false, true);
	INFO(static_cast<int>(language));
	INFO(replica);
	engine::scene::RegisterSceneClasses();
	Store store("camera-type");
	engine::scene::InstallServices(store);
	const auto player = engine::scene::AddPlayer(store, "Player", true);
	REQUIRE(engine::scene::LoadCharacter(store, player) != engine::ecs::NULL_ENTITY);
	const auto inactive = store.CreateInstance(engine::scene::CameraClass(), "InactiveCamera");
	const auto authored =
		store.CreateInstance(engine::ecs::Classes::Find(Name("SurfaceCamera")), "AuthoredCamera");
	const auto camera = store.CreatePredictedInstance(engine::scene::CameraClass(), "Viewer");
	REQUIRE(camera != engine::ecs::NULL_ENTITY);
	store.Set(camera, engine::ecs::ClientLocal{});
	store.SetResource(engine::scene::ActiveCamera{camera});
	store.SetResource(CameraController{});
	store.SetResource(engine::scene::InputState{});
	store.SetAdoptOnly(replica);
	const auto properties = engine::ecs::Classes::Describe(engine::scene::CameraClass()).Properties;
	const auto property =
		std::ranges::find(properties, Name("CameraType"), &engine::ecs::PropertyDescriptor::Name);
	REQUIRE(property != properties.end());
	CHECK(property->Kind == engine::ecs::PropertyKind::Resource);
	CHECK(property->Type == engine::ecs::PropertyType::Enum);
	CHECK(property->PredictedWritable);

	const auto runtime = engine::script::MakeRuntime(store, language);
	store.SetResource(engine::script::SourceCache{});
	size_t sourceNumber = 0;
	const auto runLocal = [&](std::string_view program) {
		const Name path("camera-type-" + std::to_string(sourceNumber++));
		store.ResourceMutable<engine::script::SourceCache>()->Set(path, std::string(program));
		const auto instance =
			store.CreatePredictedInstance(engine::script::LocalScriptClass(), "CameraControl");
		store.Set(instance, engine::ecs::ClientLocal{});
		store.Set(instance, engine::script::CodeSourceContainerSelector{language});
		if (language == Language::Luau)
			store.Set(instance, engine::script::LuaSourceContainer{path});
		else
			store.Set(instance, engine::script::JavaScriptSourceContainer{path});
		return runtime->RunInstance(instance);
	};
	const std::string_view scripted = language == Language::Luau ? R"(
local camera = workspace.CurrentCamera
assert(camera.CameraType == Enum.CameraType.Custom)
camera.CameraType = Enum.CameraType.Scriptable
camera.CFrame = CFrame.new(80, 90, 100)
camera.FieldOfView = 55
assert(camera.CameraType == Enum.CameraType.Scriptable)
assert(math.abs(camera.FieldOfView - 55) < .0001)
)"
																 : R"(
const camera = workspace.CurrentCamera;
if (camera.CameraType.Name !== 'Custom') throw new Error('initial camera type');
camera.CameraType = Enum.CameraType.Scriptable;
camera.CFrame = CFrame.new(80, 90, 100);
camera.FieldOfView = 55;
if (camera.CameraType.Name !== 'Scriptable') throw new Error('scripted camera type');
if (Math.abs(camera.FieldOfView - 55) > .0001) throw new Error('scripted field of view');
)";
	const bool scriptedStarted = runLocal(scripted);
	INFO(runtime->LastError());
	REQUIRE(scriptedStarted);
	CHECK(store.Resource<CameraController>()->Mode == CameraMode::Scriptable);
	CHECK_FALSE(engine::scene::UpdateCameraControl(store));
	CHECK_FALSE(engine::scene::FollowOwnCharacter(store));
	CHECK_FALSE(engine::scene::PlaceCamera(store));
	CHECK(store.Get<engine::scene::Transform>(camera)->Frame.Position == engine::core::Vector3{80, 90, 100});

	const Name scriptedMode("Scriptable");
	CHECK_FALSE(store.SetProperty(inactive, Name("CameraType"), &scriptedMode, sizeof(scriptedMode)));
	Name inactiveMode;
	REQUIRE(store.GetProperty(inactive, Name("CameraType"), &inactiveMode, sizeof(inactiveMode)));
	CHECK(inactiveMode == Name("Custom"));
	CHECK(store.Resource<CameraController>()->Mode == CameraMode::Scriptable);
	const std::string_view wrongEnum =
		language == Language::Luau ? "workspace.CurrentCamera.CameraType = Enum.MouseBehavior.Default"
								   : "workspace.CurrentCamera.CameraType = Enum.MouseBehavior.Default;";
	CHECK_FALSE(runLocal(wrongEnum));
	CHECK(store.Resource<CameraController>()->Mode == CameraMode::Scriptable);

	const std::string_view resumed = language == Language::Luau
										 ? "workspace.CurrentCamera.CameraType = Enum.CameraType.Custom"
										 : "workspace.CurrentCamera.CameraType = Enum.CameraType.Custom;";
	REQUIRE(runLocal(resumed));
	CHECK(store.Resource<CameraController>()->Mode == CameraMode::Classic);
	CHECK(engine::scene::FollowOwnCharacter(store));
	CHECK(engine::scene::CameraSubjectRoot(store, camera) != engine::ecs::NULL_ENTITY);
	CHECK(engine::scene::PlaceCamera(store));
	CHECK(store.Get<engine::scene::Transform>(camera)->Frame.Position != engine::core::Vector3{80, 90, 100});
	if (replica) {
		store.SetResource(engine::scene::ActiveCamera{authored});
		CHECK_FALSE(store.SetProperty(authored, Name("CameraType"), &scriptedMode, sizeof(scriptedMode)));
		const engine::core::CFrame placement(engine::core::Vector3{80, 90, 100});
		CHECK_FALSE(store.SetProperty(authored, Name("CFrame"), &placement, sizeof(placement)));
		CHECK(store.Resource<CameraController>()->Mode == CameraMode::Classic);
	}
}

TEST_CASE("server and local scripts keep CurrentCamera viewer ownership", "[script][camera][client-local]") {
	using namespace engine;
	const auto language = GENERATE(script::Language::Luau, script::Language::JavaScript);
	scene::RegisterSceneClasses();
	ecs::Store store("camera-viewer-ownership");
	const auto workspace = scene::InstallServices(store);
	const auto camera = store.CreateInstance(scene::CameraClass(), "Camera");
	REQUIRE(store.SetParent(camera, workspace));
	store.SetResource(scene::ActiveCamera{camera, 1.6f});
	store.SetResource(script::SourceCache{});
	const auto runtime = script::MakeRuntime(store, language);
	for (const bool client : {false, true}) {
		const core::Name path(client ? "camera-local" : "camera-server");
		const auto instance = client
								  ? store.CreatePredictedInstance(script::LocalScriptClass(), "CameraClient")
								  : store.CreateInstance(script::ScriptClass(), "CameraServer");
		if (client) store.Set(instance, ecs::ClientLocal{});
		store.Set(instance, script::CodeSourceContainerSelector{language});
		if (language == script::Language::Luau)
			store.Set(instance, script::LuaSourceContainer{path});
		else
			store.Set(instance, script::JavaScriptSourceContainer{path});
		const std::string_view program = language == script::Language::Luau ? (client ? R"(
local camera = workspace.CurrentCamera
assert(camera and camera.Name == 'Camera')
workspace.CurrentCamera = nil
assert(workspace.CurrentCamera == nil)
workspace.CurrentCamera = camera
assert(workspace.CurrentCamera == camera)
)"
																					  : R"(
assert(workspace.CurrentCamera == nil)
assert(workspace:FindFirstChild('Camera') == nil)
assert(not pcall(function() workspace.CurrentCamera = nil end))
)")
																			: (client ? R"(
const camera = workspace.CurrentCamera;
if (!camera || camera.Name !== 'Camera') throw new Error('missing local camera');
workspace.CurrentCamera = null;
if (workspace.CurrentCamera !== null) throw new Error('camera did not detach');
workspace.CurrentCamera = camera;
if (!workspace.CurrentCamera.Equals(camera)) throw new Error('camera did not restore');
)"
																					  : R"(
if (workspace.CurrentCamera !== null) throw new Error('server saw local camera');
if (workspace.FindFirstChild('Camera') !== null) throw new Error('server tree exposed local camera');
let refused = false;
try { workspace.CurrentCamera = null; } catch (_) { refused = true; }
if (!refused) throw new Error('server cleared local camera');
)");
		store.ResourceMutable<script::SourceCache>()->Set(path, std::string(program));
		const bool accepted = runtime->RunInstance(instance);
		INFO(static_cast<int>(language));
		INFO(client);
		INFO(runtime->LastError());
		REQUIRE(accepted);
		CHECK(store.Resource<scene::ActiveCamera>()->Entity == camera);
		CHECK(store.Resource<scene::ActiveCamera>()->AspectRatio == 1.6f);
	}
}
