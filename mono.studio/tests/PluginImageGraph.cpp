// A plugin authors a graph in Luau, hosts the node editor and receives its edits.

#include "PluginImageGraph.hpp"

#include <engine/ecs/Store.hpp>
#include <engine/imagegraph/Document.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/scene/ImageGraph.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <imgui.h>
#include <studio/Config.hpp>
#include <studio/Editor.hpp>

TEST_SUITE_ID("studio.plugins.imagegraph")
TEST_DEPENDS("engine.imagegraph.nodes")

namespace {
	using engine::core::Name;
	using engine::ecs::Store;

	// The acceptance graph: a regionUV attribute of a Luau-built mesh drives voronoi
	// pores, a ramp tints them and a multiply blend lays them over a base colour. The
	// result is assigned to MeshPart.TextureID, the part's SurfaceAppearance colour map.
	constexpr const char *PLUGIN = R"LUAU(
local mesh = Instance.new("EditableMesh")
mesh.Name = "Body"
mesh.Parent = workspace
local corners = { Vector2.new(0.05, 0.05), Vector2.new(0.95, 0.05), Vector2.new(0.95, 0.95), Vector2.new(0.05, 0.95) }
local ids = {}
for index, uv in corners do
	ids[index] = mesh:AddVertex(Vector3.new(uv.X, uv.Y, 0), Vector3.new(0, 0, 1), uv)
	assert(mesh:SetVertexAttribute(ids[index], 1, { (uv.X - 0.05) / 0.9, (uv.Y - 0.05) / 0.9, 0, 0 }))
end
mesh:AddTriangle(ids[1], ids[2], ids[3])
mesh:AddTriangle(ids[1], ids[3], ids[4])

local document = string.gsub([[{"version": 2, "parameters": [], "bindings": [], "nodes": [
	{"id": "uv", "kind": "image.mesh_attribute", "inputs": [], "position": [0, 0], "properties": {
		"width": 128, "height": 128, "mesh": "MESH", "attribute": 1, "components": "xy",
		"tile_u": 0, "tile_v": 0, "minimum": 0, "maximum": 1, "padding": 2}},
	{"id": "pores", "kind": "image.voronoi", "inputs": ["uv"], "position": [240, 0], "properties": {
		"width": 128, "height": 128, "scale": 12, "randomness": 1, "feature": "f1", "seed": 4}},
	{"id": "tint", "kind": "image.ramp", "inputs": ["pores"], "position": [480, 0], "properties": {
		"factor": "red", "interpolation": "linear", "stops": [
			{"position": 0, "colour": [90, 40, 40, 255]}, {"position": 1, "colour": [255, 255, 255, 255]}]}},
	{"id": "base", "kind": "image.solid", "inputs": [], "position": [480, 240], "properties": {
		"width": 128, "height": 128, "colour": [220, 170, 150, 255]}},
	{"id": "skin", "kind": "image.blend", "inputs": ["base", "tint"], "position": [720, 0], "properties": {
		"opacity": 1, "mode": "multiply"}}],
	"outputs": [{"name": "albedo", "node": "skin", "space": "srgb"}]}]], "MESH", tostring(mesh.ContentId))

local graph = Instance.new("ImageGraph")
graph.Name = "Skin"
graph.InstanceKey = "skin"
graph.Parent = workspace
graph:SetGraphDocument(document)

local part = Instance.new("MeshPart")
part.Name = "Skinned"
part.TextureID = graph:GetImage("albedo")
part.Parent = workspace

local info = DockWidgetPluginGuiInfo.new(Enum.InitialDockState.Float, true, false, 900, 500, 400, 300)
local widget = plugin:CreateDockWidgetPluginGui("SkinGraph", info)
widget.Title = "Skin graph"
widget:SetImageGraph(graph:GetGraphDocument())
assert(widget:GetImageGraph() ~= nil)
assert(not pcall(function() widget:SetImageGraph('{"version": 2}') end))
widget.ImageGraphChanged:Connect(function(json)
	graph:SetGraphDocument(json)
end)
)LUAU";

	struct Folder {
		std::filesystem::path Root =
			std::filesystem::temp_directory_path() / "atomic-studio-plugin-imagegraph";
		Folder() {
			std::filesystem::remove_all(Root);
			const auto plugin = Root / "config" / "plugins" / "skin-graph";
			std::filesystem::create_directories(plugin);
			std::ofstream(plugin / "plugin.json") << R"({"name":"Skin Graph"})";
			std::ofstream(plugin / "main.luau") << PLUGIN;
			studio::SetConfigRoot(Root / "config");
		}
		~Folder() {
			studio::SetConfigRoot({});
			std::filesystem::remove_all(Root);
		}
	};
	struct Context {
		ImGuiContext *Handle = ImGui::CreateContext();
		Context() {
			ImGuiIO &io = ImGui::GetIO();
			io.DisplaySize = ImVec2(1280, 720);
			io.DeltaTime = 1.0f / 60.0f;
			io.IniFilename = nullptr;
			io.Fonts->AddFontDefault();
			io.Fonts->Build();
		}
		~Context() {
			ImGui::DestroyContext(Handle);
		}
	};
	studio::PluginImageGraphEditor *HostedEditor(studio::Editor &editor) {
		REQUIRE(editor.ScriptPlugins.size() == 1);
		INFO(editor.ScriptPlugins.front().Error);
		REQUIRE(editor.ScriptPlugins.front().Running);
		for (auto &widget : editor.ScriptPlugins.front().Widgets)
			if (widget.ImageGraph) return widget.ImageGraph.get();
		FAIL("the plugin hosts no image graph editor");
		return nullptr;
	}
	std::string Document(studio::Editor &editor) {
		std::string document;
		editor.Universe->Enter(editor.Active, [&](Store &store) {
			const auto skin = store.FindFirstChild(engine::scene::WorkspaceOf(store), "Skin");
			REQUIRE(skin != engine::ecs::NULL_ENTITY);
			document = store.Get<engine::scene::ImageGraph>(skin)->Document;
		});
		return document;
	}
}

TEST_CASE("a plugin-hosted node editor reports edits as graph documents", "[studio][plugins][imagegraph]") {
	engine::scene::RegisterSceneClasses();
	Folder folder;
	Context context;
	studio::Editor editor;
	editor.Universe = std::make_unique<engine::world::Universe>();
	editor.Active = editor.Universe->Create({.Name = Name("plugin-imagegraph")});
	editor.Universe->Enter(editor.Active, [](Store &store) { engine::scene::InstallServices(store); });
	editor.LoadPlugins();
	auto *hosted = HostedEditor(editor);
	const std::string authored = Document(editor);
	REQUIRE_FALSE(authored.empty());
	CHECK(hosted->Document.find("image.voronoi") != std::string::npos);
	CHECK(hosted->State.Graph.Nodes().size() == 5);

	// Drawing an unchanged canvas fires nothing.
	ImGui::NewFrame();
	editor.DrawPluginWidgets();
	ImGui::Render();
	CHECK(Document(editor) == authored);

	// A parameter edit on the canvas reaches the plugin, which stores the new document.
	std::vector<std::string> voronoi;
	for (const auto &node : hosted->State.Graph.Nodes())
		if (node.Def == "image.voronoi") voronoi.push_back(node.Id);
	for (const auto &id : voronoi)
		REQUIRE(
			hosted->State.Graph.Apply(engine::ang::edit::SetParam{id, "seed", engine::ang::Value(9.0)}).Ok
		);
	ImGui::NewFrame();
	editor.DrawPluginWidgets();
	ImGui::Render();
	const std::string edited = Document(editor);
	CHECK(edited != authored);
	CHECK(edited.find("\"seed\": 9") != std::string::npos);
	CHECK(edited == hosted->Document);

	// An incomplete canvas is reported in the panel and not sent to the plugin.
	const auto blend = std::find_if(
		hosted->State.Graph.Nodes().begin(), hosted->State.Graph.Nodes().end(), [](const auto &node) {
			return node.Def == "image.blend";
		}
	);
	REQUIRE(blend != hosted->State.Graph.Nodes().end());
	const std::string blendId = blend->Id;
	const auto wire = std::find_if(
		hosted->State.Graph.Links().begin(), hosted->State.Graph.Links().end(), [&](const auto &link) {
			return link.ToNode == blendId && link.ToPort == "Foreground";
		}
	);
	REQUIRE(wire != hosted->State.Graph.Links().end());
	REQUIRE(hosted->State.Graph.Apply(engine::ang::edit::RemoveLink{*wire}).Ok);
	ImGui::NewFrame();
	editor.DrawPluginWidgets();
	ImGui::Render();
	CHECK(Document(editor) == edited);
	CHECK(hosted->Error.find("Foreground") != std::string::npos);
}

TEST_CASE(
	"a Luau-authored graph textures a MeshPart in Studio edit without a cook",
	"[studio][plugins][imagegraph][gpu][.]"
) {
	engine::scene::RegisterSceneClasses();
	Folder folder;
	studio::Editor editor;
	studio::Options options;
	options.Headless = true;
	REQUIRE(editor.Initialise(options));
	if (editor.ScriptPlugins.empty()) editor.LoadPlugins();
	(void)HostedEditor(editor);
	const Name owner = editor.Universe->NameOf(editor.Active);
	const Name output("imagegraph-instance://skin#albedo");
	// No content origin is configured: the slots are found and the graph evaluated anyway.
	for (int frame = 0; frame < 4 && editor.Renderer.TextureHandle(output, owner) == nullptr; ++frame)
		editor.PumpContent(1.0 / 60.0);
	REQUIRE(editor.Renderer.TextureHandle(output, owner) != nullptr);
	uint32_t width = 0, height = 0;
	REQUIRE(editor.Renderer.TextureSize(output, width, height, owner));
	CHECK(width == 128);
	CHECK(height == 128);
}
