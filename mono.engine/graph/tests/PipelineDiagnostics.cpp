// The faults a pipeline can have that the pipeline itself can report.
//
// **Every case here is a fault observed in a shipping frame**, tabulated in
// `docs/PIPELINE_NODES.md` §1.5 from a GPU capture somebody spent an afternoon
// reading. The point of this file is that six of them are arithmetic: a graph
// that knows who writes what and who reads it can find them with no GPU, no
// capture and no timing.
//
// So each case builds the smallest graph that has one fault, asserts it is
// reported, and - the half that matters more - asserts the *correct* version of
// the same graph reports nothing. A check that fires on everything is a check
// people turn off.

#include <engine/graph/PipelineDiagnostics.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>

TEST_SUITE_ID("engine.graph.pipelinediagnostics")
TEST_DEPENDS("engine.graph.rendergraph")
TEST_DEPENDS("engine.graph.pipelinecatalogue")

using engine::core::Name;
using engine::graph::Diagnose;
using engine::graph::Diagnostic;
using engine::graph::DiagnosticKind;
using engine::graph::DiagnosticSeverity;
using engine::graph::Node;
using engine::graph::NodeId;
using engine::graph::RenderGraph;
using engine::graph::ResourceFormat;
using engine::graph::ResourceId;
using engine::graph::ResourceKind;

namespace {
	RenderGraph DefaultGraph() {
		RenderGraph graph;
		Name offender;
		REQUIRE(
			engine::graph::Build(engine::graph::DefaultPbrDocument(), graph, offender) ==
			engine::graph::PipelineDocumentStatus::Ok
		);
		return graph;
	}

	void Kinds() {
		engine::graph::RegisterRenderNodeKinds();
	}

	// How many of one kind were reported.
	size_t Count(const std::vector<Diagnostic> &found, DiagnosticKind kind) {
		return static_cast<size_t>(std::count_if(found.begin(), found.end(), [kind](const Diagnostic &one) {
			return one.Kind == kind;
		}));
	}

	// Whether a named node was reported for a kind.
	bool Names(const std::vector<Diagnostic> &found, DiagnosticKind kind, const char *node) {
		return std::any_of(found.begin(), found.end(), [kind, node](const Diagnostic &one) {
			return one.Kind == kind && one.Node == Name(node);
		});
	}

	ResourceId Declare(RenderGraph &graph, const char *name, ResourceKind kind, ResourceFormat format) {
		return graph.AddResource({.Name = Name(name), .Kind = kind, .Format = format});
	}

	NodeId
	Add(RenderGraph &graph,
		const char *name,
		const char *kind,
		std::vector<ResourceId> reads,
		std::vector<ResourceId> writes) {
		Node node;
		node.Name = Name(name);
		node.Kind = Name(kind);
		node.Reads = std::move(reads);
		node.Writes = std::move(writes);
		return graph.AddNode(std::move(node));
	}
}

// --- the frame we actually ship -------------------------------------------------

// **The first thing this has to do is not cry wolf on our own frame.** A checker
// that reported six faults in the pipeline the engine draws every frame would be
// one nobody read the output of, and every case below would be worthless.
TEST_CASE("the default frame reports no warnings", "[graph][diagnostics]") {
	Kinds();

	const std::vector<Diagnostic> found = Diagnose(DefaultGraph());

	for (const Diagnostic &one : found) {
		INFO(
			engine::graph::Describe(one.Severity)
			<< " " << engine::graph::Describe(one.Kind) << " on '" << one.Node.Text() << "': " << one.Message
		);
		CHECK(one.Severity != DiagnosticSeverity::Warning);
	}
}

// --- fault 1 and 7: written and never read ---------------------------------------

TEST_CASE("a resource nothing reads is reported", "[graph][diagnostics]") {
	Kinds();
	RenderGraph graph;

	const ResourceId colour = Declare(graph, "colour", ResourceKind::Colour, ResourceFormat::RGBA16F);
	const ResourceId spare = Declare(graph, "spare", ResourceKind::Colour, ResourceFormat::RGBA16F);
	const ResourceId window = Declare(graph, "window", ResourceKind::Colour, ResourceFormat::RGB10A2);

	Add(graph, "opaque", "opaque", {}, {colour});

	// The fault: a pass copies the frame into a buffer nothing ever touches
	// again. Observed verbatim in the captured frame.
	Add(graph, "stash", "blit", {colour}, {spare});
	Add(graph, "present", "present", {colour}, {window});

	const std::vector<Diagnostic> found = Diagnose(graph);

	CHECK(Names(found, DiagnosticKind::DeadResource, "stash"));
	CHECK(Names(found, DiagnosticKind::DeadNode, "stash"));

	// **And the swapchain is not reported**, though nothing reads it either.
	// That is the whole point of the terminal exemption: `present`'s output
	// leaves the graph.
	CHECK_FALSE(Names(found, DiagnosticKind::DeadResource, "present"));
}

TEST_CASE("removing the dead pass silences it", "[graph][diagnostics]") {
	Kinds();
	RenderGraph graph;

	const ResourceId colour = Declare(graph, "colour", ResourceKind::Colour, ResourceFormat::RGBA16F);
	const ResourceId window = Declare(graph, "window", ResourceKind::Colour, ResourceFormat::RGB10A2);

	Add(graph, "opaque", "opaque", {}, {colour});
	Add(graph, "present", "present", {colour}, {window});

	const std::vector<Diagnostic> found = Diagnose(graph);
	CHECK(Count(found, DiagnosticKind::DeadResource) == 0);
	CHECK(Count(found, DiagnosticKind::DeadNode) == 0);
}

// --- fault 2: a clear whose every pixel is overwritten ----------------------------

TEST_CASE("a write overwritten before any read is reported", "[graph][diagnostics]") {
	Kinds();
	RenderGraph graph;

	const ResourceId target = Declare(graph, "target", ResourceKind::Colour, ResourceFormat::RGBA16F);
	const ResourceId window = Declare(graph, "window", ResourceKind::Colour, ResourceFormat::RGB10A2);

	// Exactly the captured frame's opening: a clear, then a copy over the whole
	// thing, and nothing read what the clear wrote.
	Add(graph, "clear", "clear", {}, {target});
	Add(graph, "fill", "opaque", {}, {target});
	Add(graph, "present", "present", {target}, {window});

	const std::vector<Diagnostic> found = Diagnose(graph);

	CHECK(Names(found, DiagnosticKind::WastedWrite, "clear"));
	CHECK_FALSE(Names(found, DiagnosticKind::WastedWrite, "fill"));
}

TEST_CASE("a pass that blends over what it writes is not a wasted write", "[graph][diagnostics]") {
	Kinds();
	RenderGraph graph;

	const ResourceId colour = Declare(graph, "colour", ResourceKind::Colour, ResourceFormat::RGBA16F);
	const ResourceId depth = Declare(graph, "depth", ResourceKind::Depth, ResourceFormat::D24S8);
	const ResourceId window = Declare(graph, "window", ResourceKind::Colour, ResourceFormat::RGB10A2);

	// **The case that makes the check usable.** `transparent` reads `colour` and
	// writes it back, which is a refinement rather than a replacement - a
	// version of this check that only compared writers would report the whole
	// standard frame.
	Add(graph, "opaque", "opaque", {}, {colour, depth});
	Add(graph, "transparent", "transparent", {colour, depth}, {colour});
	Add(graph, "present", "present", {colour}, {window});

	CHECK(Count(Diagnose(graph), DiagnosticKind::WastedWrite) == 0);
}

// --- reading what nothing wrote ---------------------------------------------------

TEST_CASE("a read of an unwritten resource is reported", "[graph][diagnostics]") {
	Kinds();
	RenderGraph graph;

	const ResourceId colour = Declare(graph, "colour", ResourceKind::Colour, ResourceFormat::RGBA16F);
	const ResourceId ghost = Declare(graph, "ghost", ResourceKind::Texture, ResourceFormat::RGBA8);
	const ResourceId window = Declare(graph, "window", ResourceKind::Colour, ResourceFormat::RGB10A2);

	// **Not the same as reading before writing**, which `Compile` already
	// refuses. Nothing writes this at all, so the pass samples whatever the
	// allocator last left in that memory - which is a black screen on one
	// machine and last frame's fog on another.
	Add(graph, "opaque", "opaque", {ghost}, {colour});
	Add(graph, "present", "present", {colour}, {window});

	CHECK(Names(Diagnose(graph), DiagnosticKind::UnwrittenRead, "opaque"));
}

// --- fault 8: a subgraph that goes nowhere -----------------------------------------

TEST_CASE("a cluster wired only to itself is reported", "[graph][diagnostics]") {
	Kinds();
	RenderGraph graph;

	const ResourceId colour = Declare(graph, "colour", ResourceKind::Colour, ResourceFormat::RGBA16F);
	const ResourceId window = Declare(graph, "window", ResourceKind::Colour, ResourceFormat::RGB10A2);
	const ResourceId scratch = Declare(graph, "scratch", ResourceKind::Colour, ResourceFormat::RGBA16F);
	const ResourceId scratch2 = Declare(graph, "scratch2", ResourceKind::Colour, ResourceFormat::RGBA16F);

	Add(graph, "opaque", "opaque", {}, {colour});
	Add(graph, "present", "present", {colour}, {window});

	// **The fault as observed**: *"draws whose resources have nothing to do with
	// previous or following draws"*. Each of these two looks fine on its own -
	// the first's output is read, the second's input is written - and together
	// they reach nothing.
	Add(graph, "orphan-a", "blit", {colour}, {scratch});
	Add(graph, "orphan-b", "blit", {scratch}, {scratch2});

	const std::vector<Diagnostic> found = Diagnose(graph);

	// `orphan-b` writes something nothing reads, so it is dead outright.
	CHECK(Names(found, DiagnosticKind::DeadNode, "orphan-b"));

	// `orphan-a`'s output *is* read - by `orphan-b` - so only the reachability
	// walk catches it. This is the assertion the whole check exists for.
	CHECK(Names(found, DiagnosticKind::Disconnected, "orphan-a"));

	// And nothing on the real chain is reported.
	CHECK_FALSE(Names(found, DiagnosticKind::Disconnected, "opaque"));
	CHECK_FALSE(Names(found, DiagnosticKind::Disconnected, "present"));
}

// --- fault 6: more bits than anybody takes -------------------------------------------

TEST_CASE("a target wider than its readers is reported", "[graph][diagnostics]") {
	Kinds();
	RenderGraph graph;

	// Deferred lighting writes `RGBA16F` at 64 bits; `smaa-edges` takes
	// `RGB10A2` at 32.
	const ResourceId colour = Declare(graph, "colour", ResourceKind::Colour, ResourceFormat::RGBA16F);
	const ResourceId edges = Declare(graph, "edges", ResourceKind::Colour, ResourceFormat::RG8);
	const ResourceId window = Declare(graph, "window", ResourceKind::Colour, ResourceFormat::RGB10A2);

	Add(graph, "lighting", "deferred-lighting", {}, {colour});
	Add(graph, "smaa-edges", "smaa-edges", {colour}, {edges});
	Add(graph, "present", "present", {edges}, {window});

	const std::vector<Diagnostic> found = Diagnose(graph);

	CHECK(Names(found, DiagnosticKind::FormatOverspend, "lighting"));

	// **A hint rather than a warning for the wire itself**, because landing HDR
	// in fewer bits is what a tone mapper is *for* - the fault is only the
	// spend, not the narrowing.
	const auto lossy = std::find_if(found.begin(), found.end(), [](const Diagnostic &one) {
		return one.Kind == DiagnosticKind::LossyWire;
	});
	REQUIRE(lossy != found.end());
	CHECK(lossy->Severity == DiagnosticSeverity::Hint);
}

// --- fault 3, as far as a declaration reaches -------------------------------------

TEST_CASE("an alpha channel no reader takes is reported", "[graph][diagnostics]") {
	Kinds();
	RenderGraph graph;

	const ResourceId colour = Declare(graph, "colour", ResourceKind::Colour, ResourceFormat::RGBA16F);
	const ResourceId pyramid = Declare(graph, "pyramid", ResourceKind::Storage, ResourceFormat::R32F);
	const ResourceId window = Declare(graph, "window", ResourceKind::Colour, ResourceFormat::RGB10A2);

	// **The question is what a reader *declares*, not what it happens to
	// sample.** `hzb` takes a single-channel `R32F`, so nothing in this graph is
	// arranged to look at deferred lighting's alpha. A first version of this case used
	// `bloom`, whose input slot declares `RGBA16F` - four channels - and the
	// check correctly said nothing: bloom asks for the alpha even though its
	// output has none.
	Add(graph, "lighting", "deferred-lighting", {}, {colour});
	Add(graph, "hzb", "hzb", {colour}, {pyramid});
	Add(graph, "present", "present", {pyramid}, {window});

	const std::vector<Diagnostic> found = Diagnose(graph);
	CHECK(Names(found, DiagnosticKind::UnusedAlpha, "lighting"));

	// **It cannot know the channel is blank** - that needs a readback, which is
	// the last stage of the plan. It can say nothing is arranged to use it, and
	// that is why this is a hint.
	const auto alpha = std::find_if(found.begin(), found.end(), [](const Diagnostic &one) {
		return one.Kind == DiagnosticKind::UnusedAlpha;
	});
	REQUIRE(alpha != found.end());
	CHECK(alpha->Severity == DiagnosticSeverity::Hint);
}

// --- fault 11: the list and the frame disagree -------------------------------------

TEST_CASE("a pass declared before what feeds it is reported", "[graph][diagnostics]") {
	Kinds();
	RenderGraph graph;

	const ResourceId shadow = Declare(graph, "shadow", ResourceKind::Depth, ResourceFormat::D32F);
	const ResourceId colour = Declare(graph, "colour", ResourceKind::Colour, ResourceFormat::RGBA16F);
	const ResourceId window = Declare(graph, "window", ResourceKind::Colour, ResourceFormat::RGB10A2);

	Add(graph, "opaque", "opaque", {shadow}, {colour});
	Add(graph, "shadow", "shadow", {}, {shadow});
	Add(graph, "present", "present", {colour}, {window});

	const std::vector<Diagnostic> found = Diagnose(graph);
	REQUIRE(Names(found, DiagnosticKind::OutOfOrder, "opaque"));

	// **This check was written believing `Compile` would sort it silently** -
	// which is what Unreal and Unity do, and what the assertion below was meant
	// to prove. It refuses instead, which is the better behaviour and is what
	// this case now records.
	engine::graph::CompiledGraph compiled;
	Name offender;
	CHECK(graph.Compile(compiled, offender) == engine::graph::GraphStatus::ReadsBeforeWrite);

	// So the value here is not finding something the compiler misses. It is
	// saying **which pass to move**: the compiler answers with a resource name
	// and stops, and a panel showing "ReadsBeforeWrite (shadow)" has told
	// somebody there is a problem and nothing about where.
	const auto ordered = std::find_if(found.begin(), found.end(), [](const Diagnostic &one) {
		return one.Kind == DiagnosticKind::OutOfOrder;
	});
	REQUIRE(ordered != found.end());
	CHECK(ordered->Node == Name("opaque"));
	CHECK(ordered->Message.find("shadow") != std::string::npos);
}

TEST_CASE("declaring them the right way round silences it", "[graph][diagnostics]") {
	Kinds();
	RenderGraph graph;

	const ResourceId shadow = Declare(graph, "shadow", ResourceKind::Depth, ResourceFormat::D32F);
	const ResourceId colour = Declare(graph, "colour", ResourceKind::Colour, ResourceFormat::RGBA16F);
	const ResourceId window = Declare(graph, "window", ResourceKind::Colour, ResourceFormat::RGB10A2);

	Add(graph, "shadow", "shadow", {}, {shadow});
	Add(graph, "opaque", "opaque", {shadow}, {colour});
	Add(graph, "present", "present", {colour}, {window});

	CHECK(Count(Diagnose(graph), DiagnosticKind::OutOfOrder) == 0);
}

TEST_CASE("a pass that refines what it reads is in order with itself", "[graph][diagnostics]") {
	Kinds();
	RenderGraph graph;

	const ResourceId colour = Declare(graph, "colour", ResourceKind::Colour, ResourceFormat::RGBA16F);
	const ResourceId depth = Declare(graph, "depth", ResourceKind::Depth, ResourceFormat::D24S8);
	const ResourceId window = Declare(graph, "window", ResourceKind::Colour, ResourceFormat::RGB10A2);

	// `transparent` appears in both the reader and the writer list for `colour`.
	// A version of this check that did not exempt that would report the standard
	// frame, which is the shape every one of these checks has to survive.
	Add(graph, "opaque", "opaque", {}, {colour, depth});
	Add(graph, "transparent", "transparent", {colour, depth}, {colour});
	Add(graph, "present", "present", {colour}, {window});

	CHECK(Count(Diagnose(graph), DiagnosticKind::OutOfOrder) == 0);
}

// --- the rules about the report itself ----------------------------------------------

TEST_CASE("a disabled pass is not a faulty pass", "[graph][diagnostics]") {
	Kinds();
	RenderGraph graph;

	const ResourceId colour = Declare(graph, "colour", ResourceKind::Colour, ResourceFormat::RGBA16F);
	const ResourceId spare = Declare(graph, "spare", ResourceKind::Colour, ResourceFormat::RGBA16F);
	const ResourceId window = Declare(graph, "window", ResourceKind::Colour, ResourceFormat::RGB10A2);

	Add(graph, "opaque", "opaque", {}, {colour});
	const NodeId stash = Add(graph, "stash", "blit", {colour}, {spare});
	Add(graph, "present", "present", {colour}, {window});

	REQUIRE(Names(Diagnose(graph), DiagnosticKind::DeadNode, "stash"));

	// **Switching a pass off is not the same as it being broken.** A report that
	// listed every disabled node would bury the findings that matter, and a pass
	// somebody turned off is a decision rather than a fault.
	graph.SetEnabled(stash, false);
	CHECK_FALSE(Names(Diagnose(graph), DiagnosticKind::DeadNode, "stash"));
}

TEST_CASE("warnings come before hints and the order is stable", "[graph][diagnostics]") {
	Kinds();
	RenderGraph graph;

	const ResourceId colour = Declare(graph, "colour", ResourceKind::Colour, ResourceFormat::RGBA16F);
	const ResourceId bloom = Declare(graph, "bloom", ResourceKind::Colour, ResourceFormat::RG11B10F);
	const ResourceId spare = Declare(graph, "spare", ResourceKind::Colour, ResourceFormat::RGBA16F);
	const ResourceId window = Declare(graph, "window", ResourceKind::Colour, ResourceFormat::RGB10A2);

	Add(graph, "opaque", "opaque", {}, {colour});
	Add(graph, "bloom", "bloom", {colour}, {bloom});
	Add(graph, "stash", "blit", {colour}, {spare});
	Add(graph, "present", "present", {bloom}, {window});

	const std::vector<Diagnostic> found = Diagnose(graph);
	REQUIRE(found.size() > 1);

	// **A panel redraws this every frame.** A list that reordered itself between
	// two frames would be unreadable, which is the same argument
	// `NodeCatalogue::All` makes about the add menu.
	bool seenHint = false;
	for (const Diagnostic &one : found) {
		if (one.Severity == DiagnosticSeverity::Hint) {
			seenHint = true;
		} else {
			CHECK_FALSE(seenHint);
		}
	}

	const std::vector<Diagnostic> again = Diagnose(graph);
	REQUIRE(again.size() == found.size());
	for (size_t at = 0; at < found.size(); at++) {
		CHECK(again[at].Kind == found[at].Kind);
		CHECK(again[at].Node == found[at].Node);
	}
}

TEST_CASE("every diagnostic names a node and says something", "[graph][diagnostics]") {
	Kinds();
	RenderGraph graph;

	const ResourceId colour = Declare(graph, "colour", ResourceKind::Colour, ResourceFormat::RGBA16F);
	const ResourceId ghost = Declare(graph, "ghost", ResourceKind::Texture, ResourceFormat::RGBA8);
	const ResourceId spare = Declare(graph, "spare", ResourceKind::Colour, ResourceFormat::RGBA16F);

	Add(graph, "opaque", "opaque", {ghost}, {colour});
	Add(graph, "stash", "blit", {colour}, {spare});

	const std::vector<Diagnostic> found = Diagnose(graph);
	REQUIRE_FALSE(found.empty());

	for (const Diagnostic &one : found) {
		INFO(engine::graph::Describe(one.Kind));

		// **A diagnostic a panel cannot place is one nobody acts on.**
		CHECK(one.Node.IsValid());
		CHECK_FALSE(one.Message.empty());
		CHECK(std::string(engine::graph::Describe(one.Kind)) != "?");
	}
}

TEST_CASE("an empty graph is not a faulty graph", "[graph][diagnostics]") {
	Kinds();
	const RenderGraph graph;
	CHECK(Diagnose(graph).empty());
}

TEST_CASE("a pass that samples its own target is refused", "[graph][diagnostics]") {
	// **The hazard SDL documents and nothing was checking.** Every target this
	// engine writes is cycled, and SDL's rule is that cycling leaves the
	// resource undefined until written again. A fullscreen pass wired to sample
	// what it draws into reads undefined memory - which on most drivers looks
	// like a plausible frame most of the time, and is the worst way for a bug to
	// behave.
	engine::graph::RegisterRenderNodeKinds();

	RenderGraph graph;
	const ResourceId scene = graph.AddResource({.Name = Name("colour"), .Kind = ResourceKind::Colour});

	Node opaque;
	opaque.Name = Name("opaque");
	opaque.Kind = Name("opaque");
	opaque.Writes = {scene};
	graph.AddNode(opaque);

	// `raster`'s first input slot is a texture, so this one samples.
	Node blur;
	blur.Name = Name("blur");
	blur.Kind = Name("raster");
	blur.Reads = {scene};
	blur.Writes = {scene};
	graph.AddNode(blur);

	const std::vector<Diagnostic> found = Diagnose(graph);

	bool reported = false;
	for (const Diagnostic &one : found) {
		if (one.Kind == DiagnosticKind::SamplesOwnTarget && one.Node == Name("blur")) {
			reported = true;
			CHECK(one.Resource == Name("colour"));
			CHECK(one.Severity == DiagnosticSeverity::Warning);
		}
	}
	CHECK(reported);
}

TEST_CASE("blending onto your own attachment is not the same mistake", "[graph][diagnostics]") {
	// **`transparent` reads and writes `colour` and is perfectly correct.** It
	// blends onto the attachment inside one render pass, which the hardware does
	// natively - no sampling, no cycling hazard. A check that fired on "reads
	// and writes the same resource" would report the standard frame as broken,
	// which is how a diagnostic teaches people to ignore it.
	engine::graph::RegisterRenderNodeKinds();

	RenderGraph graph;
	const ResourceId scene = graph.AddResource({.Name = Name("colour"), .Kind = ResourceKind::Colour});
	const ResourceId depth = graph.AddResource({.Name = Name("depth"), .Kind = ResourceKind::Depth});

	Node opaque;
	opaque.Name = Name("opaque");
	opaque.Kind = Name("opaque");
	opaque.Writes = {scene, depth};
	graph.AddNode(opaque);

	Node blended;
	blended.Name = Name("transparent");
	blended.Kind = Name("transparent");
	blended.Reads = {scene, depth};
	blended.Writes = {scene};
	graph.AddNode(blended);

	for (const Diagnostic &one : Diagnose(graph)) {
		INFO("reported " << Describe(one.Kind) << " on " << std::string(one.Node.Text()));
		CHECK(one.Kind != DiagnosticKind::SamplesOwnTarget);
	}
}

TEST_CASE("the standard frame samples nothing it writes", "[graph][diagnostics]") {
	// The check that stops this becoming noise: our own frame must be clean, or
	// nobody will read what the panel says about anybody else's.
	engine::graph::RegisterRenderNodeKinds();

	for (const Diagnostic &one : Diagnose(DefaultGraph())) {
		INFO("reported " << Describe(one.Kind) << " on " << std::string(one.Node.Text()));
		CHECK(one.Kind != DiagnosticKind::SamplesOwnTarget);
	}
}
