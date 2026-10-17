#include <engine/core/Bytes.hpp>
#include <engine/ecs/Attributes.hpp>
#include <engine/ecs/Instance.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/PlayerGui.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/script/Instances.hpp>
#include <engine/script/SourceCache.hpp>
#include <engine/scripthost/Runtime.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

TEST_SUITE_ID("engine.scripthost.clientinstances")
TEST_DEPENDS("engine.script.instanceshim")
TEST_DEPENDS("engine.scene.services")

namespace {
	engine::ecs::Entity StageScript(
		engine::ecs::Store &store, std::string_view source, engine::script::Language language, bool local
	) {
		using namespace engine;
		const std::string path = language == script::Language::Luau ? "ownership.luau" : "ownership.js";
		if (store.Resource<script::SourceCache>() == nullptr) store.SetResource(script::SourceCache{});
		store.ResourceMutable<script::SourceCache>()->Set(core::Name(path), source);
		const auto instance = script::MakeScript(store, path, local ? "Client" : "Server", local);
		REQUIRE(instance != ecs::NULL_ENTITY);
		REQUIRE(store.SetParent(instance, scene::WorkspaceOf(store)));
		return instance;
	}
}

TEST_CASE(
	"a cached builtin Workspace resolves the scene arriving after an empty replica VM opens",
	"[scripting][client][workspace-arrival]"
) {
	using namespace engine;
	const auto language = GENERATE(script::Language::Luau, script::Language::JavaScript);
	scene::RegisterSceneClasses();
	gui::RegisterGuiClasses();
	ecs::Store replica("workspace.empty-replica");
	replica.SetAdoptOnly(true);
	const auto runtime = script::MakeRuntime(replica, language, {.Role = script::HostRole::OfClient()});
	REQUIRE(runtime != nullptr);
	const bool cached = runtime->Run(
		language == script::Language::Luau ? R"(
local cachedWorkspace = game:GetService('Workspace')
assert(cachedWorkspace == workspace and cachedWorkspace == game.Workspace)
local phase = 0
local cachedArrival
game:GetService('RunService').Heartbeat:Connect(function()
	phase += 1
	assert(cachedWorkspace.Name == 'Workspace')
	assert(cachedWorkspace == workspace and cachedWorkspace == game:GetService('Workspace'))
	if phase == 1 then
		cachedArrival = cachedWorkspace:FindFirstChild('Arrival')
		assert(cachedArrival ~= nil and cachedArrival.Name == 'Arrival')
		local proof = Instance.new('Part', cachedWorkspace)
		proof.Name = 'LiveProof'
	elseif phase == 2 then
		assert(cachedArrival ~= cachedWorkspace)
		assert(cachedArrival:FindFirstChild('LiveProof') == nil)
		assert(cachedWorkspace:FindFirstChild('Replacement') ~= nil)
		local proof = cachedWorkspace:FindFirstChild('LiveProof')
		assert(proof ~= nil)
		proof:SetAttribute('CorrectionVerified', true)
	elseif phase == 3 then
		assert(cachedWorkspace == game.Workspace)
		assert(cachedWorkspace:FindFirstChild('RemintedChild') ~= nil)
		local proof = Instance.new('Part', cachedWorkspace)
		proof.Name = 'RemintProof'
	end
end)
)"
										   : R"(
const cachedWorkspace = game.GetService('Workspace');
if (cachedWorkspace !== workspace || cachedWorkspace !== game.Workspace) throw new Error('builtin identity differs');
let phase = 0;
let cachedArrival;
game.GetService('RunService').Heartbeat.Connect(function() {
	++phase;
	if (cachedWorkspace.Name !== 'Workspace') throw new Error('cached builtin did not arrive');
	if (cachedWorkspace !== workspace || cachedWorkspace !== game.GetService('Workspace')) throw new Error('builtin identity changed');
	if (phase === 1) {
		cachedArrival = cachedWorkspace.FindFirstChild('Arrival');
		if (cachedArrival === null || cachedArrival.Name !== 'Arrival') throw new Error('authored child missing');
		const proof = Instance.new('Part', cachedWorkspace);
		proof.Name = 'LiveProof';
	} else if (phase === 2) {
		if (cachedArrival.Equals(cachedWorkspace)) throw new Error('ordinary dead handle rebound to builtin');
		if (cachedArrival.FindFirstChild('LiveProof') !== null) throw new Error('dead handle found live workspace child');
		if (cachedWorkspace.FindFirstChild('Replacement') === null) throw new Error('correction missing');
		const proof = cachedWorkspace.FindFirstChild('LiveProof');
		if (proof === null) throw new Error('local child lost');
		proof.SetAttribute('CorrectionVerified', true);
	} else if (phase === 3) {
		if (cachedWorkspace !== game.Workspace) throw new Error('remint changed builtin identity');
		if (cachedWorkspace.FindFirstChild('RemintedChild') === null) throw new Error('reminted child missing');
		const proof = Instance.new('Part', cachedWorkspace);
		proof.Name = 'RemintProof';
	}
});
)"
	);
	INFO(runtime->LastError());
	REQUIRE(cached);
	CHECK(scene::WorkspaceOf(replica) == ecs::NULL_ENTITY);
	ecs::Store authority("workspace.authority");
	const auto workspace = scene::InstallServices(authority);
	const auto arrival = authority.CreateInstance(scene::PartClass(), "Arrival");
	REQUIRE(authority.SetParent(arrival, workspace));
	core::ByteWriter writer;
	REQUIRE(authority.Save(writer));
	core::ByteReader reader(writer.Bytes());
	REQUIRE(replica.Apply(reader, ecs::ApplyMode::Authoritative, ecs::ApplyClock::PreserveLocal));
	REQUIRE(scene::WorkspaceOf(replica) == workspace);
	const bool resolved = runtime->Heartbeat(1.0f / 60.0f);
	INFO(runtime->LastError());
	REQUIRE(resolved);
	const auto proof = replica.FindFirstChild(workspace, "LiveProof");
	REQUIRE(proof != ecs::NULL_ENTITY);
	CHECK(ecs::Store::IsPredicted(proof));
	CHECK(replica.Has<ecs::ClientLocal>(proof));
	authority.DestroyInstance(arrival);
	const auto replacement = authority.CreateInstance(scene::PartClass(), "Replacement");
	REQUIRE(authority.SetParent(replacement, workspace));
	writer.Clear();
	REQUIRE(authority.Save(writer));
	core::ByteReader correction(writer.Bytes());
	REQUIRE(replica.Apply(correction, ecs::ApplyMode::Authoritative, ecs::ApplyClock::PreserveLocal));
	const bool generationPreserved = runtime->Heartbeat(1.0f / 60.0f);
	INFO(runtime->LastError());
	REQUIRE(generationPreserved);
	ecs::AttributeValue verified;
	REQUIRE(ecs::GetAttribute(replica, proof, core::Name("CorrectionVerified"), verified));
	CHECK(verified.Bool);
	ecs::Store reminted("workspace.reminted-authority");
	const auto decoy = reminted.CreateInstance(scene::PartClass(), "DifferentInstance");
	const auto remintedWorkspace = scene::InstallServices(reminted);
	REQUIRE(decoy == workspace);
	REQUIRE(remintedWorkspace != workspace);
	const auto remintedChild = reminted.CreateInstance(scene::PartClass(), "RemintedChild");
	REQUIRE(reminted.SetParent(remintedChild, remintedWorkspace));
	writer.Clear();
	REQUIRE(reminted.Save(writer));
	core::ByteReader remintCorrection(writer.Bytes());
	REQUIRE(replica.Apply(remintCorrection, ecs::ApplyMode::Authoritative, ecs::ApplyClock::PreserveLocal));
	REQUIRE(replica.Alive(workspace));
	REQUIRE(replica.IsA(workspace, scene::PartClass()));
	const bool builtinReminted = runtime->Heartbeat(1.0f / 60.0f);
	INFO(runtime->LastError());
	REQUIRE(builtinReminted);
	CHECK(replica.FindFirstChild(remintedWorkspace, "RemintProof") != ecs::NULL_ENTITY);
}

TEST_CASE("script ownership survives callbacks sharing one VM and source path", "[scripting][client]") {
	using namespace engine;
	const auto language = GENERATE(script::Language::Luau, script::Language::JavaScript);
	scene::RegisterSceneClasses();
	ecs::Store store("script.ownership");
	scene::InstallServices(store);
	const auto workspace = scene::WorkspaceOf(store);
	const auto authority = store.CreateInstance(scene::PartClass(), "Authority");
	REQUIRE(store.SetParent(authority, workspace));
	const std::string_view source = language == script::Language::Luau ? R"(
local authority = workspace:FindFirstChild('Authority')
local server = game:GetService('RunService'):IsServer()
local label = server and 'ServerOwned' or 'ClientOwned'
local part = Instance.new('Part', workspace)
part.Name = label
part.Anchored = true
part.CFrame = CFrame.new(1, 2, 3)
part:SetAttribute('owner', label)
local copy = part:Clone()
copy.Name = label .. 'Clone'
copy.Parent = workspace
local connection
connection = game:GetService('RunService').Heartbeat:Connect(function()
    assert(game:GetService('RunService'):IsServer() == server)
    part.CFrame = CFrame.new(4, 5, 6)
    local later = Instance.new('Part', workspace)
    later.Name = label .. 'Later'
    if not server then
        assert(not pcall(function() authority.Name = 'Forbidden' end))
        assert(not pcall(function() workspace.Authority:SetAttribute('owner', label) end))
        assert(not pcall(function() workspace.Authority:Destroy() end))
    end
    connection:Disconnect()
end)
)"
																	   : R"(
const authority = workspace.FindFirstChild('Authority');
const server = game.GetService('RunService').IsServer();
const label = server ? 'ServerOwned' : 'ClientOwned';
const part = Instance.new('Part', workspace);
part.Name = label;
part.Anchored = true;
part.CFrame = CFrame.new(1, 2, 3);
part.SetAttribute('owner', label);
const copy = part.Clone();
copy.Name = label + 'Clone';
copy.Parent = workspace;
let connection;
connection = game.GetService('RunService').Heartbeat.Connect(function() {
    if (game.GetService('RunService').IsServer() !== server) throw new Error('callback side lost');
    part.CFrame = CFrame.new(4, 5, 6);
    const later = Instance.new('Part', workspace);
    later.Name = label + 'Later';
    if (!server) {
        let refused = 0;
        try { authority.Name = 'Forbidden'; } catch (_) { ++refused; }
        try { authority.SetAttribute('owner', label); } catch (_) { ++refused; }
        try { authority.Destroy(); } catch (_) { ++refused; }
        if (refused !== 3) throw new Error('authority write escaped');
    }
    connection.Disconnect();
});
)";
	const auto client = StageScript(store, source, language, true);
	const auto server = StageScript(store, source, language, false);
	const auto runtime = script::MakeRuntime(store, language, {.Role = script::HostRole::OfBoth()});
	REQUIRE(runtime != nullptr);
	const bool clientStarted = runtime->RunInstance(client);
	INFO(runtime->LastError());
	REQUIRE(clientStarted);
	const bool serverStarted = runtime->RunInstance(server);
	INFO(runtime->LastError());
	REQUIRE(serverStarted);
	const bool heartbeat = runtime->Heartbeat(1.0f / 60.0f);
	INFO(runtime->LastError());
	REQUIRE(heartbeat);
	for (const std::string_view suffix : {"", "Clone", "Later"}) {
		const auto ownedClient =
			store.FindFirstChild(workspace, std::string("ClientOwned") + std::string(suffix));
		const auto ownedServer =
			store.FindFirstChild(workspace, std::string("ServerOwned") + std::string(suffix));
		REQUIRE(ownedClient != ecs::NULL_ENTITY);
		REQUIRE(ownedServer != ecs::NULL_ENTITY);
		CHECK(ecs::Store::IsPredicted(ownedClient));
		CHECK(store.Has<ecs::ClientLocal>(ownedClient));
		CHECK_FALSE(ecs::Store::IsPredicted(ownedServer));
		CHECK_FALSE(store.Has<ecs::ClientLocal>(ownedServer));
	}
	CHECK(store.Alive(authority));
	CHECK(store.InstanceNameOf(authority) == core::Name("Authority"));
	CHECK(
		store.Get<scene::Transform>(store.FindFirstChild(workspace, "ClientOwned"))->Frame.Position ==
		core::Vector3{4, 5, 6}
	);
}

TEST_CASE(
	"a replica creates local images and GUI without writing another player's GUI", "[scripting][client]"
) {
	using namespace engine;
	const auto language = GENERATE(script::Language::Luau, script::Language::JavaScript);
	scene::RegisterSceneClasses();
	gui::RegisterGuiClasses();
	ecs::Store store("script.localgui");
	scene::InstallServices(store);
	const auto localPlayer = scene::AddPlayer(store, "Local", true);
	const auto otherPlayer = scene::AddPlayer(store, "Other", false);
	for (const auto player : {localPlayer, otherPlayer}) {
		const auto label = store.CreateInstance(gui::GuiClass("TextLabel"), "Status");
		REQUIRE(store.SetParent(label, store.FindFirstChild(player, "PlayerGui")));
	}
	store.SetAdoptOnly(true);
	REQUIRE(gui::RefreshPlayerGuiProjection(store, localPlayer, ecs::NULL_ENTITY) == 1);
	const auto runtime = script::MakeRuntime(store, language, {.Role = script::HostRole::OfClient()});
	const auto source = language == script::Language::Luau ? R"(
local gui = game:GetService('Players').LocalPlayer.PlayerGui
local status = gui:FindFirstChild('Status')
status.Text = 'mine'
assert(not pcall(function() game:GetService('Players').Other.PlayerGui.Status.Text = 'theirs' end))
assert(not pcall(function() game:GetService('Players').Other.PlayerGui.Status.Parent = workspace end))
status.Parent = workspace
status.Parent = gui
local image = Instance.new('EditableImage', workspace)
image.Name = 'LocalImage'
assert(image:Resize(2, 2))
assert(image:DrawRectangle(Vector2.new(0, 0), Vector2.new(2, 2), Color3.new(1, 0, 0)))
local label = Instance.new('ImageLabel', gui)
label.Name = 'LocalLabel'
label.Image = image.ContentId
)"
														   : R"(
const gui = game.GetService('Players').LocalPlayer.PlayerGui;
const status = gui.FindFirstChild('Status');
status.Text = 'mine';
let refused = 0;
try { game.GetService('Players').FindFirstChild('Other').PlayerGui.FindFirstChild('Status').Text = 'theirs'; } catch (_) { ++refused; }
try { game.GetService('Players').FindFirstChild('Other').PlayerGui.FindFirstChild('Status').Parent = workspace; } catch (_) { ++refused; }
if (refused !== 2) throw new Error('GUI ownership escaped');
status.Parent = workspace;
status.Parent = gui;
const image = Instance.new('EditableImage', workspace);
image.Name = 'LocalImage';
if (!image.Resize(2, 2)) throw new Error('local image resize refused');
if (!image.DrawRectangle(Vector2.new(0, 0), Vector2.new(2, 2), Color3.new(1, 0, 0))) throw new Error('paint refused');
const label = Instance.new('ImageLabel', gui);
label.Name = 'LocalLabel';
label.Image = image.ContentId;
)";
	const bool ran = runtime->Run(source, "local-visuals");
	INFO(runtime->LastError());
	REQUIRE(ran);
	const auto ownedGui = store.FindFirstChild(localPlayer, "PlayerGui");
	const auto otherGui = store.FindFirstChild(otherPlayer, "PlayerGui");
	const auto sourceStatus = store.FindFirstChild(ownedGui, "Status");
	const auto statusCopy = gui::FindPlayerGuiCopy(store, sourceStatus);
	REQUIRE(statusCopy != ecs::NULL_ENTITY);
	CHECK(store.Get<gui::Label>(statusCopy)->Text == "mine");
	CHECK(store.Get<gui::Label>(sourceStatus)->Text.empty());
	CHECK(store.Get<gui::Label>(store.FindFirstChild(otherGui, "Status"))->Text != "theirs");
	CHECK(store.Has<ecs::ClientLocal>(store.FindFirstChild(ownedGui, "LocalLabel")));
}

TEST_CASE("shared server scripts cannot inspect or mutate client-owned scene trees", "[client][ownership]") {
	using namespace engine;
	const auto language = GENERATE(script::Language::Luau, script::Language::JavaScript);
	scene::RegisterSceneClasses();
	ecs::Store store("script.private-scene");
	scene::InstallServices(store);
	const auto runtime = script::MakeRuntime(store, language, {.Role = script::HostRole::OfBoth()});
	const auto clientSource = language == script::Language::Luau ? R"(
local part = Instance.new('Part', workspace)
part.Name = 'ClientOnly'
local camera = Instance.new('Camera', workspace)
camera.Name = 'ClientCamera'
local child = Instance.new('Part', camera)
child.Name = 'CameraChild'
)"
																 : R"(
const part = Instance.new('Part', workspace);
part.Name = 'ClientOnly';
const camera = Instance.new('Camera', workspace);
camera.Name = 'ClientCamera';
const child = Instance.new('Part', camera);
child.Name = 'CameraChild';
)";
	const auto client = StageScript(store, clientSource, language, true);
	REQUIRE(runtime->RunInstance(client));
	const auto serverSource = language == script::Language::Luau ? R"(
local probe = Instance.new('Folder', workspace)
probe.Name = 'ServerProbe'
local private = workspace:FindFirstChild('ClientOnly')
probe:SetAttribute('named', private ~= nil)
local function seesPrivate(items)
    for _, item in ipairs(items) do
        if item.Name == 'ClientOnly' or item.Name == 'ClientCamera' or item.Name == 'CameraChild' then
            return true
        end
    end
    return false
end
probe:SetAttribute('children', seesPrivate(workspace:GetChildren()))
probe:SetAttribute('descendants', seesPrivate(workspace:GetDescendants()))
probe:SetAttribute('query', seesPrivate(World:Query('scene.Transform')))
probe:SetAttribute('count', World:Count('ecs.ClientLocal') ~= 0)
probe:SetAttribute('filtered', seesPrivate(World:QueryFiltered({'scene.Transform'}, {})))
local propertyWrite, methodWrite = false, false
if private then
    propertyWrite = pcall(function() private.CFrame = CFrame.new(7, 8, 9) end)
    methodWrite = pcall(function() private:SetAttribute('escaped', true) end)
end
probe:SetAttribute('property_write', propertyWrite)
probe:SetAttribute('method_write', methodWrite)
)"
																 : R"(
const probe = Instance.new('Folder', workspace);
probe.Name = 'ServerProbe';
const privatePart = workspace.FindFirstChild('ClientOnly');
probe.SetAttribute('named', privatePart !== null);
function seesPrivate(items) {
    return items.some(item => item.Name === 'ClientOnly' || item.Name === 'ClientCamera' || item.Name === 'CameraChild');
}
probe.SetAttribute('children', seesPrivate(workspace.GetChildren()));
probe.SetAttribute('descendants', seesPrivate(workspace.GetDescendants()));
probe.SetAttribute('query', seesPrivate(World.Query('scene.Transform')));
probe.SetAttribute('count', World.Count('ecs.ClientLocal') !== 0);
probe.SetAttribute('filtered', seesPrivate(World.QueryFiltered(['scene.Transform'], [])));
let propertyWrite = false, methodWrite = false;
if (privatePart !== null) {
    try { privatePart.CFrame = CFrame.new(7, 8, 9); propertyWrite = true; } catch (_) {}
    try { privatePart.SetAttribute('escaped', true); methodWrite = true; } catch (_) {}
}
probe.SetAttribute('property_write', propertyWrite);
probe.SetAttribute('method_write', methodWrite);
)";
	const auto server = StageScript(store, serverSource, language, false);
	const bool started = runtime->RunInstance(server);
	INFO(runtime->LastError());
	REQUIRE(started);
	const auto probe = store.FindFirstChild(scene::WorkspaceOf(store), "ServerProbe");
	REQUIRE(probe != ecs::NULL_ENTITY);
	for (const auto field :
		 {"named",
		  "children",
		  "descendants",
		  "query",
		  "count",
		  "filtered",
		  "property_write",
		  "method_write"}) {
		ecs::AttributeValue value;
		INFO(field);
		REQUIRE(ecs::GetAttribute(store, probe, core::Name(field), value));
		REQUIRE(value.Type == ecs::PropertyType::Bool);
		CHECK_FALSE(value.Bool);
	}
}
