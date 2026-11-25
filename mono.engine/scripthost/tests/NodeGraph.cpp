// The NodeGraph script library, through both VMs: define, build, run, hooks,
// refusals, limits, the read-only guard and views.

#include <engine/ecs/Store.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/scene/Part.hpp>
#include <engine/script/Runtime.hpp>
#include <engine/scripthost/Runtime.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

TEST_SUITE_ID("engine.scripthost.nodegraph")
TEST_DEPENDS("engine.ang.runner")

using engine::ecs::Store;
using engine::script::Language;
using engine::script::MakeRuntime;
using engine::script::Runtime;

namespace {
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
local okLimit = pcall(function() for i = 1, 1100 do big:AddNode("math.add") end end)
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
