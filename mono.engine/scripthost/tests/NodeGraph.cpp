// The NodeGraph script library, through both VMs: define, build, run, hooks,
// refusals, limits, the read-only guard and views.

#include <engine/ecs/Attributes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/script/NodeGraphService.hpp>
#include <engine/script/Runtime.hpp>
#include <engine/scripthost/Runtime.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

TEST_SUITE_ID("engine.scripthost.nodegraph")
TEST_DEPENDS("engine.ang.runner")

using engine::ecs::Store;
using engine::script::Language;
using engine::script::MakeRuntime;
using engine::script::Runtime;

namespace {
	// Crosses one simulation tick, so waiting script threads resume.
	void Beat(Store &store, Runtime &runtime) {
		store.ClearChanges();
		store.AdvanceTick(store.Time().Delta);
		runtime.Heartbeat(store.Time().Delta);
		store.FlushSignals();
	}

	void MustRun(Runtime &runtime, const std::string &source) {
		INFO(source);
		const bool ok = runtime.Run(source.c_str());
		INFO(runtime.LastError());
		REQUIRE(ok);
	}

	std::unique_ptr<Runtime> Make(Store &store, Language language) {
		engine::scene::EnsureClassTree();
		engine::gui::RegisterGuiClasses();
		return MakeRuntime(store, language);
	}

	constexpr const char *LUAU_SETUP = R"(
local NG = NodeGraph
local reg = NG.Registry.new()
reg:DefineType("text", { Wire = Color3.new(1, 0, 0) })
reg:DefineNode({
	Id = "math.add",
	Inputs = { { Name = "a", Type = "number" }, { Name = "b", Type = "number", Default = 0 } },
	Outputs = { { Name = "sum", Type = "number" } },
	Evaluate = function(ctx, i) return { sum = i.a + i.b } end,
})
reg:DefineNode({
	Id = "text.make",
	Outputs = { { Name = "out", Type = "text" } },
	Evaluate = function() return { out = "hi" } end,
})
reg:DefineNode({
	Id = "boom",
	Outputs = { { Name = "out", Type = "number" } },
	Evaluate = function() error("kaboom") end,
})
reg:DefineNode({
	Id = "writer",
	Outputs = { { Name = "out", Type = "number" } },
	Evaluate = function() Instance.new("Folder").Parent = workspace return { out = 1 } end,
})
reg:DefineNode({
	Id = "impure.writer",
	Pure = false,
	Outputs = { { Name = "out", Type = "number" } },
	Evaluate = function() Instance.new("Folder").Parent = workspace return { out = 1 } end,
})
)";

	constexpr const char *JS_SETUP = R"(
const NG = NodeGraph;
const reg = new NG.Registry();
reg.DefineType("text", {});
reg.DefineNode({
	Id: "math.add",
	Inputs: [{ Name: "a", Type: "number" }, { Name: "b", Type: "number", Default: 0 }],
	Outputs: [{ Name: "sum", Type: "number" }],
	Evaluate: (ctx, i) => ({ sum: i.a + i.b }),
});
reg.DefineNode({ Id: "text.make", Outputs: [{ Name: "out", Type: "text" }], Evaluate: () => ({ out: "hi" }) });
reg.DefineNode({ Id: "boom", Outputs: [{ Name: "out", Type: "number" }], Evaluate: () => { throw new Error("kaboom"); } });
)";
}

TEST_CASE("Luau builds, runs and caches a graph", "[scripthost][nodegraph]") {
	Store store("nodegraph_luau");
	const auto runtime = Make(store, Language::Luau);
	MustRun(*runtime, std::string(LUAU_SETUP) + R"(
local g = NG.Graph.new(reg)
local a = g:AddNode("math.add", { b = 2 })
local b = g:AddNode("math.add")
g:Link(a, "sum", b, "a")
local runner = NG.Runner.new(g, "Tick")
local visits = 0
local connection = runner.PreNode:Connect(function(info) visits += 1 end)
local report = runner:Run({ [a] = { a = 1 } })
assert(runner:Output(b, "sum") == 3, "sum")
assert(report.Ran == 2, "ran " .. report.Ran)
assert(visits == 2, "hooks " .. visits)
local again = runner:Run({ [a] = { a = 1 } })
assert(again.Cached == 2, "cached")
connection:Disconnect()
runner:Run({ [a] = { a = 5 } })
assert(visits == 4, "disconnect")
assert(runner:Output(b, "sum") == 7)
assert(#g:GetNodes() == 2 and #g:GetLinks() == 1)
)");
}

TEST_CASE("Luau refusals, failures and the read-only guard", "[scripthost][nodegraph]") {
	Store store("nodegraph_luau_refusals");
	const auto runtime = Make(store, Language::Luau);
	MustRun(*runtime, std::string(LUAU_SETUP) + R"(
local g = NG.Graph.new(reg)
local t = g:AddNode("text.make")
local s = g:AddNode("math.add")
local ok, err = pcall(function() g:Link(t, "out", s, "a") end)
assert(not ok and string.find(tostring(err), "does not connect"), tostring(err))
local boom = g:AddNode("boom")
local pure = g:AddNode("writer")
local impure = g:AddNode("impure.writer")
local report = NG.Runner.new(g):Run({ [s] = { a = 1 } })
assert(report.Nodes[boom].Status == "failed", "boom")
assert(string.find(report.Nodes[boom].Note, "kaboom"), report.Nodes[boom].Note)
assert(report.Nodes[pure].Status == "failed", "pure writer must fail")
assert(string.find(report.Nodes[pure].Note, "read%-only"), report.Nodes[pure].Note)
assert(report.Nodes[impure].Status == "ran", "impure writer runs")
local okTick, tickErr = pcall(function() NG.Runner.new(g, "Later") end)
assert(not okTick)
local big = NG.Graph.new(reg)
local okLimit = pcall(function() for i = 1, 4100 do big:AddNode("math.add") end end)
assert(not okLimit, "node limit")
)");
}

TEST_CASE("Luau views paint through a function or onto a NodeCanvas", "[scripthost][nodegraph]") {
	Store store("nodegraph_luau_view");
	const auto runtime = Make(store, Language::Luau);
	MustRun(*runtime, std::string(LUAU_SETUP) + R"(
local g = NG.Graph.new(reg)
local a = g:AddNode("math.add", nil, Vector2.new(0, 0))
local b = g:AddNode("math.add", nil, Vector2.new(300, 0))
g:Link(a, "sum", b, "a")
local view = NG.View.new(g, { Theme = "Blueprint" })
local painted = 0
view.Paint = function(commands, graph)
	painted = #commands
	assert(graph == g)
end
assert(painted > 0, "paint")
view.Paint = nil
local canvas = Instance.new("NodeCanvas")
view.Parent = canvas
local nodes, links = 0, 0
for _, child in canvas:GetChildren() do
	if child:IsA("NodeCanvasNode") then nodes += 1 end
	if child:IsA("NodeCanvasLink") then links += 1 end
end
assert(nodes == 2 and links == 1, nodes .. " " .. links)
-- A player removing the wire on the canvas reaches the graph on the next run.
for _, child in canvas:GetChildren() do
	if child:IsA("NodeCanvasLink") then child:Destroy() end
end
NG.Runner.new(g):Run({ [a] = { a = 1 }, [b] = { a = 1 } })
assert(#g:GetLinks() == 0, "pulled unlink")
)");
}

TEST_CASE("JavaScript builds, runs, hooks and refuses", "[scripthost][nodegraph]") {
	Store store("nodegraph_js");
	const auto runtime = Make(store, Language::JavaScript);
	MustRun(*runtime, std::string(JS_SETUP) + R"(
const g = new NG.Graph(reg);
const a = g.AddNode("math.add", { b: 2 });
const b = g.AddNode("math.add");
g.Link(a, "sum", b, "a");
const runner = new NG.Runner(g, "Tick");
let visits = 0;
runner.PostNode.Connect(() => { visits++; });
const report = runner.Run({ [a]: { a: 1 } });
if (runner.Output(b, "sum") !== 3) throw new Error("sum");
if (report.Ran !== 2 || visits !== 2) throw new Error("report");
let refused = false;
const t = g.AddNode("text.make");
try { g.Link(t, "out", b, "b"); } catch (e) { refused = String(e).includes("does not connect"); }
if (!refused) throw new Error("type refusal");
const boom = g.AddNode("boom");
const failed = runner.Run({ [a]: { a: 1 } });
if (failed.Nodes[boom].Status !== "failed" || !failed.Nodes[boom].Note.includes("kaboom")) throw new Error("failure");
const view = new NG.View(g, { Theme: "Minimal" });
let painted = 0;
view.Paint = (commands) => { painted = commands.length; };
if (painted === 0) throw new Error("paint");
)");
}

TEST_CASE("a script run is stopped at its byte limit", "[scripthost][nodegraph]") {
	Store store("nodegraph_limits");
	const auto runtime = Make(store, Language::Luau);
	MustRun(*runtime, std::string(LUAU_SETUP) + R"(
local runs = 0
reg:DefineNode({
	Id = "big",
	Outputs = { { Name = "out", Type = "string" } },
	Evaluate = function() runs += 1 return { out = string.rep("x", 3 * 1024 * 1024) } end,
})
local g = NG.Graph.new(reg)
for _ = 1, 4 do g:AddNode("big", { n = 1 }) end
local ok, err = pcall(function() NG.Runner.new(g):Run() end)
assert(not ok and string.find(tostring(err), "output bytes"), tostring(err))
)");
}

namespace {
	// A host graph of two nodes: an async one that makes an opaque value once
	// `Release` is set, and one that counts what it is given.
	struct HostGraph {
		std::shared_ptr<std::atomic<bool>> Release = std::make_shared<std::atomic<bool>>(true);

		engine::script::RuntimeLimits Limits() const {
			engine::script::RuntimeLimits limits;
			limits.NodeGraphs = std::make_shared<engine::script::NodeGraphHost>(engine::script::NodeGraphHost{
				.Names = [] { return std::vector<std::string>{"pictures"}; },
				.Open =
					[release = Release](
						std::string_view name,
						engine::ang::Registry &types,
						engine::ang::Graph &graph,
						std::string &error
					) {
						if (name != "pictures") {
							error = "no such graph";
							return false;
						}
						types = engine::ang::Registry::WithScalars();
						types.DefineType({.Id = "picture"});
						types.DefineNode(
							{.Id = "make",
							 .Outputs =
								 {{.Name = "out", .Type = "picture", .Dir = engine::ang::Direction::Output}},
							 .Mode = engine::ang::ExecMode::Async,
							 .Evaluate = [release](engine::ang::EvalContext &context) {
								 while (!release->load() && !context.Cancelled()) {
									 std::this_thread::yield();
								 }
								 context.SetOutput(
									 "out",
									 engine::ang::Value::FromBlob(
										 "picture",
										 {.Data = std::make_shared<int>(7), .Bytes = 64, .Identity = 9}
									 )
								 );
								 return engine::ang::NodeResult::Success();
							 }}
						);
						types.DefineNode(
							{.Id = "read",
							 .Inputs = {{.Name = "in", .Type = "picture", .Optional = true}},
							 .Outputs =
								 {{.Name = "value", .Type = "number", .Dir = engine::ang::Direction::Output}},
							 .Evaluate = [](engine::ang::EvalContext &context) {
								 const auto *blob = context.Input("in").Get<engine::ang::Blob>();
								 context.SetOutput(
									 "value",
									 engine::ang::Value(
										 blob != nullptr ? double(*static_cast<const int *>(blob->Data.get()))
														 : -1.0
									 )
								 );
								 return engine::ang::NodeResult::Success();
							 }}
						);
						return graph.Load({{.Id = "make", .Def = "make"}, {.Id = "read", .Def = "read"}}, {})
							.Ok;
					},
			});
			return limits;
		}
	};

	std::unique_ptr<Runtime> Make(Store &store, Language language, const HostGraph &host) {
		engine::scene::EnsureClassTree();
		engine::gui::RegisterGuiClasses();
		return MakeRuntime(store, language, host.Limits());
	}
}

TEST_CASE("RunAsync yields on async work and a host value is an opaque handle", "[scripthost][nodegraph]") {
	for (const Language language : {Language::Luau, Language::JavaScript}) {
		Store store("nodegraph_async");
		HostGraph host;
		host.Release->store(false);
		const auto runtime = Make(store, language, host);
		MustRun(
			*runtime,
			language == Language::Luau ? R"(
assert(NodeGraph.Graph.hostNames()[1] == "pictures")
local g = NodeGraph.Graph.fromHost("pictures")
assert(not pcall(function() g:RemoveNode("make") end), "read-only")
assert(not pcall(function() g.Registry:DefineType("mine") end), "locked")
assert(not pcall(function() NodeGraph.Graph.fromHost("other") end))
assert(not pcall(function() NodeGraph.Runner.new(g, "Tick"):RunAsync() end))
local runner = NodeGraph.Runner.new(g)
local computed = 0
runner.PostCompute:Connect(function(info) if info.Node == "make" then computed += 1 end end)
local marker = Instance.new("Folder")
marker.Name = "Marker"
marker.Parent = workspace
task.spawn(function()
	local report = runner:RunAsync()
	local picture = runner:Output("make", "out")
	assert(picture.Type == "picture" and picture.Bytes == 64, "handle")
	assert(runner:Output("make", "out").NodeGraphBlob == picture.NodeGraphBlob, "stable handle")
	runner:Run({ read = { ["in"] = picture } })
	assert(runner:Output("read", "value") == 7, "handle resolves")
	assert(not pcall(function()
		runner:Run({ read = { ["in"] = { NodeGraphBlob = 99, Type = "picture" } } })
	end), "forged handle")
	assert(computed == 1, "hooks " .. computed)
	marker:SetAttribute("done", report.Complete)
end)
assert(marker:GetAttribute("done") == nil, "yielded")
)"
									   : R"(
if (NodeGraph.Graph.hostNames()[0] !== "pictures") throw new Error("names");
const g = NodeGraph.Graph.fromHost("pictures");
const refused = (body) => { try { body(); return false; } catch (_) { return true; } };
if (!refused(() => g.RemoveNode("make"))) throw new Error("read-only");
if (!refused(() => g.Registry.DefineType("mine"))) throw new Error("locked");
const runner = new NodeGraph.Runner(g);
const marker = Instance.new("Folder");
marker.Name = "Marker";
marker.Parent = workspace;
task.spawn(async () => {
	const report = await runner.RunAsync();
	const picture = runner.Output("make", "out");
	if (picture.Type !== "picture") throw new Error("handle");
	runner.Run({ read: { in: picture } });
	if (runner.Output("read", "value") !== 7) throw new Error("resolves");
	marker.SetAttribute("done", report.Complete);
});
)"
		);
		const auto marker = store.FindFirstChild(engine::scene::WorkspaceOf(store), "Marker");
		REQUIRE(marker != engine::ecs::NULL_ENTITY);
		engine::ecs::AttributeValue done;
		Beat(store, *runtime);
		CHECK(!engine::ecs::GetAttribute(store, marker, engine::core::Name("done"), done));
		host.Release->store(true);
		for (int beat = 0; beat < 500 && runtime->LastError().empty(); ++beat) {
			Beat(store, *runtime);
			if (engine::ecs::GetAttribute(store, marker, engine::core::Name("done"), done)) {
				break;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
		INFO(runtime->LastError());
		CHECK(engine::ecs::GetAttribute(store, marker, engine::core::Name("done"), done));
	}
}

TEST_CASE("a published graph is followed through its instance", "[scripthost][nodegraph]") {
	for (const Language language : {Language::Luau, Language::JavaScript}) {
		Store store("nodegraph_publish");
		const auto runtime = Make(store, language);
		MustRun(
			*runtime,
			language == Language::Luau ? std::string(LUAU_SETUP) + R"(
local carrier = Instance.new("Folder")
carrier.Name = "Carrier"
carrier.Parent = workspace
local g = NG.Graph.new(reg)
local a = g:AddNode("math.add", { a = 3, b = 4 }, Vector2.new(5, 6))
local copy = NG.Graph.load(reg, g:Save())
assert(#copy:GetNodes() == 1 and copy:GetNodes()[1].X == 5)
assert(not pcall(function() copy:Load("nonsense") end))
assert(#copy:GetNodes() == 1)
g:PublishTo(carrier)
local follower = NG.Graph.subscribe(reg, carrier)
assert(#follower:GetNodes() == 1)
local canvas = Instance.new("NodeCanvas")
local view = NG.View.new(follower)
view.Parent = canvas
local b = g:AddNode("math.add")
g:Link(a, "sum", b, "a")
g:SetCollapsed(b, true)
task.defer(function()
	task.wait()
	assert(#follower:GetNodes() == 2 and #follower:GetLinks() == 1, "followed")
	assert(follower:GetNodes()[2].Collapsed)
	local runner = NG.Runner.new(follower)
	runner:Run()
	assert(runner:Output(b, "sum") == 7)
	g:Unlink(a, "sum", b, "a")
	task.wait()
	task.wait()
	-- The canvas the follower drew must not write the old wire back.
	assert(#follower:GetLinks() == 0, "unlinked")
	carrier:SetAttribute("done", true)
end)
)"
									   : std::string(JS_SETUP) + R"(
const carrier = Instance.new("Folder");
carrier.Name = "Carrier";
carrier.Parent = workspace;
const g = new NG.Graph(reg);
const a = g.AddNode("math.add", { a: 3, b: 4 });
g.PublishTo(carrier);
const follower = NG.Graph.subscribe(reg, carrier);
const b = g.AddNode("math.add");
g.Link(a, "sum", b, "a");
task.spawn(async () => {
	await task.wait(0);
	await task.wait(0);
	const runner = new NG.Runner(follower);
	runner.Run();
	if (runner.Output(b, "sum") !== 7) throw new Error("followed");
	carrier.SetAttribute("done", true);
});
)"
		);
		for (int beat = 0; beat < 6; ++beat) {
			Beat(store, *runtime);
		}
		INFO(runtime->LastError());
		const auto carrier = store.FindFirstChild(engine::scene::WorkspaceOf(store), "Carrier");
		engine::ecs::AttributeValue done;
		CHECK(engine::ecs::GetAttribute(store, carrier, engine::core::Name("done"), done));
	}
}

TEST_CASE("an image graph is edited as a node graph and committed", "[scripthost][nodegraph]") {
	const std::string document =
		R"({"version": 2, "parameters": [], "bindings": [], "nodes": [)"
		R"({"id": "base", "kind": "image.solid", "inputs": [], "position": [0, 0], "properties": {)"
		R"("width": 4, "height": 4, "colour": [255, 0, 0, 255]}}],)"
		R"("outputs": [{"name": "albedo", "node": "base", "space": "srgb"}]})";
	engine::scene::RegisterSceneClasses();
	Store store("nodegraph_image");
	const auto runtime = Make(store, Language::Luau);
	MustRun(*runtime, "local DOCUMENT = [==[" + document + R"(]==]
local image = Instance.new("ImageGraph")
image.Parent = workspace
image.InstanceKey = "skin"
assert(not pcall(function() NodeGraph.Graph.fromImageGraph(image) end), "no document")
image:SetGraphDocument(DOCUMENT)
local g = NodeGraph.Graph.fromImageGraph(image)
assert(#g:GetNodes() == 1)
g:SetParam("base", "colour", { X = 0, Y = 1, Z = 0, W = 1 })
g:Commit()
local pixels = image:ReadImage("albedo")
assert(buffer.readu8(pixels, 0) == 0 and buffer.readu8(pixels, 1) == 255, "committed colour")
g:RemoveNode("base")
assert(not pcall(function() g:Commit() end), "an output lost its node")
assert(buffer.readu8(image:ReadImage("albedo"), 1) == 255)
assert(not pcall(function() NodeGraph.Graph.fromProcedural() end))
)");
}
