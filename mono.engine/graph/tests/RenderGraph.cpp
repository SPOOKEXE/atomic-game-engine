// The frame as a graph, run on a machine with no GPU.
//
// **Every case here runs a real executor**, because the thing worth checking is
// not that a list came out sorted — it is that a shadow pass runs once while a
// colour pass runs four times, and that the two happen in that order. A
// recording `NodeRunner` is what makes that assertable at all, and it is the
// same trick `bake::Graph` uses to test a bake with no filesystem.

#include <engine/graph/RenderGraph.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

TEST_SUITE_ID("engine.graph.rendergraph")

using engine::core::Name;
using engine::graph::CompiledGraph;
using engine::graph::GraphStatus;
using engine::graph::Node;
using engine::graph::NodeRunner;
using engine::graph::RenderGraph;
using engine::graph::ResourceDesc;
using engine::graph::ResourceId;
using engine::graph::ResourceKind;
using engine::graph::RunContext;
using engine::graph::StandardGraph;

namespace {

	// Writes down what it was asked to do, and can be told to fail.
	struct Recorder : NodeRunner {
		std::vector<std::string> Ran;
		std::string FailOn;

		bool Run(const RunContext &context) override {
			std::string line{context.Name.Text()};
			if (context.View != RunContext::WHOLE_FRAME) {
				line += "@" + std::to_string(context.View);
			}
			Ran.push_back(line);
			return std::string{context.Name.Text()} != FailOn;
		}
	};

	ResourceId Colour(RenderGraph &graph, const char *name) {
		return graph.AddResource({Name(name), ResourceKind::Colour, 0, 0});
	}
}

TEST_CASE("a standard frame compiles and its shadow pass is shared", "[graph]") {
	const RenderGraph graph = StandardGraph();

	CompiledGraph compiled;
	Name offender;
	REQUIRE(graph.Compile(compiled, offender) == GraphStatus::Ok);

	// **Shared at both ends, per view in the middle**, which is the shape of a
	// real frame: one shadow map every view samples, three passes each view
	// draws for itself, and the window's overlay and chrome once over the lot.
	CHECK(compiled.Shared.size() == 1);
	CHECK(compiled.PerView.size() == 3);
	CHECK(compiled.Final.size() == 2);

	CHECK(graph.Find(compiled.Shared.front())->Name == Name("shadow"));
	CHECK(graph.Find(compiled.Final.front())->Name == Name("overlay"));
	CHECK(graph.Find(compiled.Final.back())->Name == Name("interface"));
}

TEST_CASE("four views pay for one shadow pass and four colour passes", "[graph]") {
	const RenderGraph graph = StandardGraph();

	CompiledGraph compiled;
	Name offender;
	REQUIRE(graph.Compile(compiled, offender) == GraphStatus::Ok);

	Recorder recorder;
	REQUIRE(graph.Execute(compiled, recorder, 4));

	// **The number this version exists to change.** Before it, four viewports
	// were four `Render` calls and four shadow maps.
	size_t shadows = 0;
	size_t opaques = 0;
	for (const std::string &line : recorder.Ran) {
		shadows += line == "shadow" ? 1 : 0;
		opaques += line.rfind("opaque@", 0) == 0 ? 1 : 0;
	}
	CHECK(shadows == 1);
	CHECK(opaques == 4);

	// And the shared one goes first, or every view samples a map written for
	// the frame after it.
	CHECK(recorder.Ran.front() == "shadow");
}

TEST_CASE("no views runs the shared work and nothing else", "[graph]") {
	const RenderGraph graph = StandardGraph();

	CompiledGraph compiled;
	Name offender;
	REQUIRE(graph.Compile(compiled, offender) == GraphStatus::Ok);

	Recorder recorder;
	REQUIRE(graph.Execute(compiled, recorder, 0));

	// A headless host that presents nothing still has a world to light, and an
	// editor with every viewport closed still has panels to draw. **Both ends of
	// the frame survive a viewless one** — which is the same contract
	// `Renderer::Render` documents for an empty span of views.
	REQUIRE(recorder.Ran.size() == 3);
	CHECK(recorder.Ran[0] == "shadow");
	CHECK(recorder.Ran[1] == "overlay");
	CHECK(recorder.Ran[2] == "interface");
}

TEST_CASE("one view's passes are adjacent rather than interleaved", "[graph]") {
	const RenderGraph graph = StandardGraph();

	CompiledGraph compiled;
	Name offender;
	REQUIRE(graph.Compile(compiled, offender) == GraphStatus::Ok);

	Recorder recorder;
	REQUIRE(graph.Execute(compiled, recorder, 2));

	// **View outermost.** The other nesting is legal on every backend and
	// illegible in every profiler.
	const auto first = std::find(recorder.Ran.begin(), recorder.Ran.end(), "surface@0");
	const auto second = std::find(recorder.Ran.begin(), recorder.Ran.end(), "surface@1");
	REQUIRE(first != recorder.Ran.end());
	REQUIRE(second != recorder.Ran.end());

	// Everything between them belongs to view 0.
	for (auto it = first; it != second; ++it) {
		CHECK(it->find("@1") == std::string::npos);
	}
}

TEST_CASE("declaration order is the order, and a read below its write is refused", "[graph]") {
	RenderGraph graph;
	const ResourceId early = Colour(graph, "early");
	const ResourceId late = Colour(graph, "late");

	// The consumer first, which is now an error rather than something to sort
	// out. **`Pipeline.hpp` refuses a general dependency-resolving frame graph
	// at this size, and a read-modify-write chain is why**: `opaque`,
	// `transparent`, `overlay` and `interface` all read *and* write colour, so
	// there is no dependency edge that recovers which goes on top. That is
	// authored, and the check's job is to catch an author who got it backwards.
	graph.AddNode({.Name = Name("consumer"), .Reads = {early}, .Writes = {late}});
	graph.AddNode({.Name = Name("producer"), .Reads = {}, .Writes = {early}});

	CompiledGraph compiled;
	Name offender;
	CHECK(graph.Compile(compiled, offender) == GraphStatus::ReadsBeforeWrite);
	CHECK(offender == Name("consumer"));

	// The right way round compiles and runs in the order it was written.
	RenderGraph fixed;
	const ResourceId a = Colour(fixed, "early");
	const ResourceId b = Colour(fixed, "late");
	fixed.AddNode({.Name = Name("producer"), .Reads = {}, .Writes = {a}});
	fixed.AddNode({.Name = Name("consumer"), .Reads = {a}, .Writes = {b}});

	REQUIRE(fixed.Compile(compiled, offender) == GraphStatus::Ok);

	Recorder recorder;
	REQUIRE(fixed.Execute(compiled, recorder, 1));
	REQUIRE(recorder.Ran.size() == 2);
	CHECK(recorder.Ran[0] == "producer@0");
	CHECK(recorder.Ran[1] == "consumer@0");
}

TEST_CASE("a node reading what it writes is a load and not a cycle", "[graph]") {
	RenderGraph graph;
	const ResourceId colour = Colour(graph, "colour");

	graph.AddNode({.Name = Name("opaque"), .Reads = {}, .Writes = {colour}});
	graph.AddNode({.Name = Name("transparent"), .Reads = {colour}, .Writes = {colour}});

	// **The case a naive dependency walk calls a loop.** Drawing *onto* what a
	// previous pass wrote is the entire reason `Attachment::Clear` exists.
	CompiledGraph compiled;
	Name offender;
	CHECK(graph.Compile(compiled, offender) == GraphStatus::Ok);
}

TEST_CASE("a shared node may not read what a per-view node writes", "[graph]") {
	RenderGraph graph;
	const ResourceId perView = Colour(graph, "perview");
	const ResourceId shared = Colour(graph, "shared");

	// Declared first, so it is not an ordering mistake — the shared block runs
	// before the per-view block whatever order they were written in, so this
	// read can never be satisfied.
	graph.AddNode({.Name = Name("consumer"), .Reads = {perView}, .Writes = {shared}, .PerView = false});
	graph.AddNode({.Name = Name("producer"), .Reads = {}, .Writes = {perView}, .PerView = true});

	// **And "which view's output would it even be" is the reason.** A shared
	// node runs once for a frame with four views; there is no single per-view
	// result for it to read.
	CompiledGraph compiled;
	Name offender;
	CHECK(graph.Compile(compiled, offender) == GraphStatus::ReadsBeforeWrite);
	CHECK(offender == Name("consumer"));
}

TEST_CASE("a read nothing writes is its own error", "[graph]") {
	RenderGraph graph;
	const ResourceId colour = Colour(graph, "colour");
	const ResourceId missing = Colour(graph, "missing");

	graph.AddNode({.Name = Name("reader"), .Reads = {missing}, .Writes = {colour}});

	// **Apart from `Cycle`, because the fixes differ**: this is a producer
	// deleted or never added, where a cycle is two passes waiting on each other.
	CompiledGraph compiled;
	Name offender;
	CHECK(graph.Compile(compiled, offender) == GraphStatus::ReadsBeforeWrite);
	CHECK(offender == Name("reader"));
}

TEST_CASE("a shared and a per-view node cannot write one resource", "[graph]") {
	RenderGraph graph;
	const ResourceId colour = Colour(graph, "colour");

	graph.AddNode({.Name = Name("shared"), .Writes = {colour}, .PerView = false});
	graph.AddNode({.Name = Name("perview"), .Writes = {colour}, .PerView = true});

	// **The one failure the partition can produce and nothing else can catch.**
	// The shared node runs once, the per-view node runs per view, and the last
	// view silently discards the shared work — which looks like the shared pass
	// never running.
	Name offender;
	CHECK(graph.Validate(offender) == GraphStatus::SharedWriteConflict);
	CHECK(offender == Name("colour"));
}

TEST_CASE("a disabled node leaves the compile entirely", "[graph]") {
	RenderGraph graph = StandardGraph();

	CompiledGraph before;
	Name offender;
	REQUIRE(graph.Compile(before, offender) == GraphStatus::Ok);

	// Find the overlay and switch it off.
	engine::graph::NodeId overlay;
	for (const engine::graph::NodeId id : before.Final) {
		if (graph.Find(id)->Name == Name("overlay")) {
			overlay = id;
		}
	}
	REQUIRE(overlay.IsValid());
	REQUIRE(graph.SetEnabled(overlay, false));

	CompiledGraph after;
	REQUIRE(graph.Compile(after, offender) == GraphStatus::Ok);

	// **Out of the compile rather than skipped at run time**, so switching a
	// pass off costs nothing per frame instead of a branch per node per view.
	CHECK(after.Final.size() == before.Final.size() - 1);
	CHECK(after.PerView.size() == before.PerView.size());

	Recorder recorder;
	REQUIRE(graph.Execute(after, recorder, 1));
	CHECK(std::find(recorder.Ran.begin(), recorder.Ran.end(), "overlay") == recorder.Ran.end());
}

TEST_CASE("disabling a producer is a missing producer and says so", "[graph]") {
	RenderGraph graph;
	const ResourceId early = Colour(graph, "early");
	const ResourceId late = Colour(graph, "late");

	const engine::graph::NodeId producer =
		graph.AddNode({.Name = Name("producer"), .Reads = {}, .Writes = {early}});
	graph.AddNode({.Name = Name("consumer"), .Reads = {early}, .Writes = {late}});

	REQUIRE(graph.SetEnabled(producer, false));

	// **Switching a pass off can break the frame, and the error has to say
	// which pass noticed.** An editor that let somebody disable a node and then
	// drew black would be the "distinct from dead" failure `Node::Enabled`
	// warns about.
	CompiledGraph compiled;
	Name offender;
	CHECK(graph.Compile(compiled, offender) == GraphStatus::ReadsBeforeWrite);
	CHECK(offender == Name("consumer"));
}

TEST_CASE("a runner that fails abandons the frame", "[graph]") {
	const RenderGraph graph = StandardGraph();

	CompiledGraph compiled;
	Name offender;
	REQUIRE(graph.Compile(compiled, offender) == GraphStatus::Ok);

	Recorder recorder;
	recorder.FailOn = "opaque";
	CHECK_FALSE(graph.Execute(compiled, recorder, 2));

	// **Stops rather than carrying on**, because a pass that failed leaves its
	// writes undefined and everything downstream reads them. View 1 is never
	// started.
	CHECK(std::find(recorder.Ran.begin(), recorder.Ran.end(), "opaque@1") == recorder.Ran.end());
}

TEST_CASE("two nodes may not share a name", "[graph]") {
	RenderGraph graph;
	const ResourceId colour = Colour(graph, "colour");

	graph.AddNode({.Name = Name("pass"), .Writes = {colour}});
	graph.AddNode({.Name = Name("pass"), .Writes = {colour}});

	Name offender;
	CHECK(graph.Validate(offender) == GraphStatus::DuplicateNode);
}

TEST_CASE("a node that writes nothing cannot be observed", "[graph]") {
	RenderGraph graph;
	graph.AddNode({.Name = Name("pointless"), .Reads = {}, .Writes = {}});

	Name offender;
	CHECK(graph.Validate(offender) == GraphStatus::WritesNothing);
	CHECK(offender == Name("pointless"));
}

TEST_CASE("a resource nothing declared is refused", "[graph]") {
	RenderGraph graph;
	graph.AddNode({.Name = Name("pass"), .Writes = {ResourceId{99}}});

	Name offender;
	CHECK(graph.Validate(offender) == GraphStatus::UnknownResource);
}

TEST_CASE("a duplicate resource name is refused rather than merged", "[graph]") {
	RenderGraph graph;
	CHECK(Colour(graph, "colour").IsValid());

	// Two declarations of one name is a graph whose behaviour depends on which
	// a node happened to be handed.
	CHECK_FALSE(Colour(graph, "colour").IsValid());
	CHECK(graph.ResourceCount() == 1);
}

TEST_CASE("the compile is reproducible", "[graph]") {
	const RenderGraph graph = StandardGraph();

	CompiledGraph first;
	CompiledGraph second;
	Name offender;
	REQUIRE(graph.Compile(first, offender) == GraphStatus::Ok);
	REQUIRE(graph.Compile(second, offender) == GraphStatus::Ok);

	// **Ties break by declaration order**, because a topological sort over a
	// hash map's iteration order puts two independent passes in a different
	// order on a different run — and `just determinism` compares byte for byte.
	CHECK(first.Shared == second.Shared);
	CHECK(first.PerView == second.PerView);
}

TEST_CASE("the graph refuses to grow past its bound", "[graph]") {
	RenderGraph graph;
	const ResourceId colour = Colour(graph, "colour");

	for (uint32_t index = 0; index < RenderGraph::MAXIMUM_NODES; index++) {
		const Name name(std::string("pass") + std::to_string(index));
		REQUIRE(graph.AddNode({.Name = name, .Writes = {colour}}).IsValid());
	}

	// A bound so an editor cannot build a graph that takes unbounded time to
	// compile.
	CHECK_FALSE(graph.AddNode({.Name = Name("one-too-many"), .Writes = {colour}}).IsValid());
	CHECK(graph.Count() == RenderGraph::MAXIMUM_NODES);
}

// --- shared work at both ends -------------------------------------------------

TEST_CASE("a shared node declared after the views runs after all of them", "[graph]") {
	RenderGraph graph;
	const ResourceId colour = graph.AddResource({Name("colour"), ResourceKind::Colour, 0, 0});
	const ResourceId window = graph.AddResource({Name("window"), ResourceKind::Colour, 0, 0});

	graph.AddNode({.Name = Name("draw"), .Kind = Name("draw"), .Writes = {colour}, .PerView = true});
	graph.AddNode({.Name = Name("present"), .Kind = Name("present"), .Writes = {window}, .PerView = false});

	CompiledGraph compiled;
	Name offender;
	REQUIRE(graph.Compile(compiled, offender) == GraphStatus::Ok);

	// **Nothing before, one after**, decided by where it was declared rather
	// than by a second field.
	CHECK(compiled.Shared.empty());
	CHECK(compiled.PerView.size() == 1);
	REQUIRE(compiled.Final.size() == 1);

	Recorder recorder;
	REQUIRE(graph.Execute(compiled, recorder, 3));

	// Three views' work, then the shared pass once, last.
	REQUIRE(recorder.Ran.size() == 4);
	CHECK(recorder.Ran[0] == "draw@0");
	CHECK(recorder.Ran[3] == "present");
}

TEST_CASE("the same node declared before the views runs before all of them", "[graph]") {
	// The identical graph with the declaration order swapped, so the only thing
	// deciding which end the shared node lands at is its position.
	RenderGraph graph;
	const ResourceId colour = graph.AddResource({Name("colour"), ResourceKind::Colour, 0, 0});
	const ResourceId window = graph.AddResource({Name("window"), ResourceKind::Colour, 0, 0});

	graph.AddNode({.Name = Name("present"), .Kind = Name("present"), .Writes = {window}, .PerView = false});
	graph.AddNode({.Name = Name("draw"), .Kind = Name("draw"), .Writes = {colour}, .PerView = true});

	CompiledGraph compiled;
	Name offender;
	REQUIRE(graph.Compile(compiled, offender) == GraphStatus::Ok);

	CHECK(compiled.Shared.size() == 1);
	CHECK(compiled.Final.empty());

	Recorder recorder;
	REQUIRE(graph.Execute(compiled, recorder, 3));
	CHECK(recorder.Ran.front() == "present");
}

TEST_CASE("a shared node between two per-view nodes is refused", "[graph]") {
	// The one arrangement three blocks cannot express: every view's first pass,
	// then one shared pass, then every view's second. Refused rather than
	// hoisted to an end, because either end changes what it reads.
	RenderGraph graph;
	const ResourceId a = graph.AddResource({Name("a"), ResourceKind::Colour, 0, 0});
	const ResourceId b = graph.AddResource({Name("b"), ResourceKind::Colour, 0, 0});
	const ResourceId c = graph.AddResource({Name("c"), ResourceKind::Colour, 0, 0});

	graph.AddNode({.Name = Name("first"), .Kind = Name("k"), .Writes = {a}, .PerView = true});
	graph.AddNode({.Name = Name("middle"), .Kind = Name("k"), .Writes = {b}, .PerView = false});
	graph.AddNode({.Name = Name("last"), .Kind = Name("k"), .Writes = {c}, .PerView = true});

	CompiledGraph compiled;
	Name offender;
	CHECK(graph.Compile(compiled, offender) == GraphStatus::SharedBetweenViews);
	CHECK(offender == Name("middle"));

	// Nothing half-compiled is left behind for a caller that ignored the status.
	CHECK(compiled.Shared.empty());
	CHECK(compiled.PerView.empty());
	CHECK(compiled.Final.empty());
}

TEST_CASE("switching off the per-view node that trapped a shared one makes it legal", "[graph]") {
	// **Checked over the enabled set, which is why this is in `Compile` and not
	// `Validate`.** The middle node is only "between" views while the per-view
	// node after it runs.
	RenderGraph graph;
	const ResourceId a = graph.AddResource({Name("a"), ResourceKind::Colour, 0, 0});
	const ResourceId b = graph.AddResource({Name("b"), ResourceKind::Colour, 0, 0});
	const ResourceId c = graph.AddResource({Name("c"), ResourceKind::Colour, 0, 0});

	graph.AddNode({.Name = Name("first"), .Kind = Name("k"), .Writes = {a}, .PerView = true});
	graph.AddNode({.Name = Name("middle"), .Kind = Name("k"), .Writes = {b}, .PerView = false});
	const engine::graph::NodeId last =
		graph.AddNode({.Name = Name("last"), .Kind = Name("k"), .Writes = {c}, .PerView = true});

	CompiledGraph compiled;
	Name offender;
	REQUIRE(graph.Compile(compiled, offender) == GraphStatus::SharedBetweenViews);

	REQUIRE(graph.SetEnabled(last, false));
	CHECK(graph.Compile(compiled, offender) == GraphStatus::Ok);
	CHECK(compiled.Final.size() == 1);
}

TEST_CASE("the standard frame draws the window's passes after every view", "[graph]") {
	const RenderGraph graph = StandardGraph();

	CompiledGraph compiled;
	Name offender;
	REQUIRE(graph.Compile(compiled, offender) == GraphStatus::Ok);

	Recorder recorder;
	REQUIRE(graph.Execute(compiled, recorder, 2));

	// **The editor's chrome is drawn once, last, however many viewports there
	// are.** Marking it per view would draw the panels twice here, each into
	// whichever target that viewport was using.
	CHECK(recorder.Ran.back() == "interface");
	CHECK(std::count(recorder.Ran.begin(), recorder.Ran.end(), "interface") == 1);
	CHECK(std::count(recorder.Ran.begin(), recorder.Ran.end(), "overlay") == 1);

	// And the world's passes really did run twice.
	CHECK(std::count(recorder.Ran.begin(), recorder.Ran.end(), "opaque@1") == 1);
}

// --- shared means shared per world ---------------------------------------------

TEST_CASE("two views of one world pay for one shadow pass", "[graph]") {
	const RenderGraph graph = StandardGraph();

	CompiledGraph compiled;
	Name offender;
	REQUIRE(graph.Compile(compiled, offender) == GraphStatus::Ok);

	Recorder recorder;
	const uint64_t worlds[] = {7, 7};
	REQUIRE(graph.Execute(compiled, recorder, worlds));

	CHECK(std::count(recorder.Ran.begin(), recorder.Ran.end(), "shadow") == 1);
	CHECK(std::count(recorder.Ran.begin(), recorder.Ran.end(), "opaque@0") == 1);
	CHECK(std::count(recorder.Ran.begin(), recorder.Ran.end(), "opaque@1") == 1);
}

TEST_CASE("two views of two worlds pay for two shadow passes", "[graph]") {
	// **The defect `D00040` recorded, now fixed.** A shadow map is per world per
	// light, so a frame showing two worlds needs two — running one for the frame
	// would light one world's geometry with the other's sun fit.
	const RenderGraph graph = StandardGraph();

	CompiledGraph compiled;
	Name offender;
	REQUIRE(graph.Compile(compiled, offender) == GraphStatus::Ok);

	Recorder recorder;
	const uint64_t worlds[] = {1, 2};
	REQUIRE(graph.Execute(compiled, recorder, worlds));

	CHECK(std::count(recorder.Ran.begin(), recorder.Ran.end(), "shadow") == 2);

	// And the window's passes still run once, however many worlds there were.
	CHECK(std::count(recorder.Ran.begin(), recorder.Ran.end(), "interface") == 1);
}

TEST_CASE("a world's views run immediately after its shared work", "[graph]") {
	// **The other grouping loses.** Every world's shared block first, then every
	// view, would have the second world's shadow pass overwrite the first's
	// before the first's views had sampled it.
	const RenderGraph graph = StandardGraph();

	CompiledGraph compiled;
	Name offender;
	REQUIRE(graph.Compile(compiled, offender) == GraphStatus::Ok);

	Recorder recorder;
	const uint64_t worlds[] = {1, 2};
	REQUIRE(graph.Execute(compiled, recorder, worlds));

	const auto at = [&recorder](std::string_view name) {
		return std::find(recorder.Ran.begin(), recorder.Ran.end(), name) - recorder.Ran.begin();
	};

	// shadow, view 0's passes, shadow, view 1's passes.
	CHECK(at("opaque@0") < at("opaque@1"));
	CHECK(at("shadow") < at("opaque@0"));
}

TEST_CASE("views of one world need not be adjacent", "[graph]") {
	// A studio's panels are in panel order, not world order, and asking a caller
	// to sort them would be asking it to know why.
	const RenderGraph graph = StandardGraph();

	CompiledGraph compiled;
	Name offender;
	REQUIRE(graph.Compile(compiled, offender) == GraphStatus::Ok);

	Recorder recorder;
	const uint64_t worlds[] = {1, 2, 1};
	REQUIRE(graph.Execute(compiled, recorder, worlds));

	// Two worlds, so two shadow passes — not three, and not one.
	CHECK(std::count(recorder.Ran.begin(), recorder.Ran.end(), "shadow") == 2);

	// All three views still drew.
	CHECK(std::count(recorder.Ran.begin(), recorder.Ran.end(), "opaque@0") == 1);
	CHECK(std::count(recorder.Ran.begin(), recorder.Ran.end(), "opaque@1") == 1);
	CHECK(std::count(recorder.Ran.begin(), recorder.Ran.end(), "opaque@2") == 1);
}

TEST_CASE("no views still runs the shared block once", "[graph]") {
	const RenderGraph graph = StandardGraph();

	CompiledGraph compiled;
	Name offender;
	REQUIRE(graph.Compile(compiled, offender) == GraphStatus::Ok);

	Recorder recorder;
	REQUIRE(graph.Execute(compiled, recorder, std::span<const uint64_t>{}));

	CHECK(std::count(recorder.Ran.begin(), recorder.Ran.end(), "shadow") == 1);
	CHECK(std::count(recorder.Ran.begin(), recorder.Ran.end(), "interface") == 1);
}

TEST_CASE("the view-count overload is every view in one world", "[graph]") {
	// What a game and a single-panel editor both are, and the reason the simpler
	// signature is kept rather than making every caller invent an id.
	const RenderGraph graph = StandardGraph();

	CompiledGraph compiled;
	Name offender;
	REQUIRE(graph.Compile(compiled, offender) == GraphStatus::Ok);

	Recorder counted;
	REQUIRE(graph.Execute(compiled, counted, 3));

	Recorder named;
	const uint64_t worlds[] = {0, 0, 0};
	REQUIRE(graph.Execute(compiled, named, worlds));

	CHECK(counted.Ran == named.Ran);
}
