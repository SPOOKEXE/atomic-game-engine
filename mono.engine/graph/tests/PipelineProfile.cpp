// The frame as a grid: passes across, resources down, access where they meet.
//
// **The half of the profiler that is arithmetic**, and therefore the half a
// headless suite can hold. What is left for the panel is drawing coloured
// squares, which is the part `docs/PIPELINE_NODES.md` §7 argues cannot be
// tested and should therefore contain no decisions.
//
// The lifetime pair is the interesting content. Everything a transient
// allocator does rests on "these two resources are never live at the same
// time", and getting that off by one pass in either direction is a frame that
// samples a target something else has already been given.

#include <engine/graph/PipelineDocument.hpp>
#include <engine/graph/PipelineProfile.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.graph.pipelineprofile")
TEST_DEPENDS("engine.graph.rendergraph")

using engine::core::Name;
using engine::graph::Access;
using engine::graph::CompiledGraph;
using engine::graph::GraphStatus;
using engine::graph::Node;
using engine::graph::NodeScope;
using engine::graph::PipelineProfile;
using engine::graph::ProfilePipeline;
using engine::graph::ProfileResource;
using engine::graph::RenderGraph;
using engine::graph::ResourceFormat;
using engine::graph::ResourceId;
using engine::graph::ResourceKind;

namespace {
	constexpr uint32_t WIDTH = 1920;
	constexpr uint32_t HEIGHT = 1080;

	// Profiles a graph at 1080p, requiring it to compile first.
	PipelineProfile Profiled(const RenderGraph &graph) {
		CompiledGraph compiled;
		Name offender;
		REQUIRE(graph.Compile(compiled, offender) == GraphStatus::Ok);
		return ProfilePipeline(graph, compiled, WIDTH, HEIGHT);
	}

	RenderGraph DefaultGraph() {
		RenderGraph graph;
		Name offender;
		REQUIRE(
			engine::graph::Build(engine::graph::DefaultPbrDocument(), graph, offender) ==
			engine::graph::PipelineDocumentStatus::Ok
		);
		return graph;
	}

	size_t RowOf(const PipelineProfile &profile, const char *name) {
		for (size_t at = 0; at < profile.Resources.size(); at++) {
			if (profile.Resources[at].Name == Name(name)) {
				return at;
			}
		}
		FAIL("no such resource: " << name);
		return 0;
	}

	size_t ColumnOf(const PipelineProfile &profile, const char *name) {
		for (size_t at = 0; at < profile.Passes.size(); at++) {
			if (profile.Passes[at].Name == Name(name)) {
				return at;
			}
		}
		FAIL("no such pass: " << name);
		return 0;
	}
}

// --- the frame we ship ------------------------------------------------------------

TEST_CASE("the default frame profiles into a full grid", "[graph][profile]") {
	const PipelineProfile profile = Profiled(DefaultGraph());

	REQUIRE(profile.Passes.size() == 18);
	REQUIRE(profile.Resources.size() == 18);
	CHECK(profile.Cells.size() == 324);

	// The three blocks, in the order a frame runs them.
	CHECK(profile.Passes.front().Where == engine::graph::Band::Shared);
	CHECK(profile.Passes.back().Where == engine::graph::Band::Final);

	// The relationship the whole grid exists to show, asserted on one cell:
	// Deferred lighting reads what `shadow` wrote.
	CHECK(profile.At(RowOf(profile, "shadow"), ColumnOf(profile, "shadow")) == Access::Write);
	CHECK(profile.At(RowOf(profile, "shadow"), ColumnOf(profile, "deferred-lighting")) == Access::Read);

	// **Read *and* write, which is the state a two-colour grid cannot show.**
	// `transparent` blends over the colour it then writes back, and a version of
	// this that reported only the write would make it look like a replacement.
	CHECK(profile.At(RowOf(profile, "display"), ColumnOf(profile, "transparent")) == Access::ReadWrite);

	// And a pass that never touches a resource says so.
	CHECK(profile.At(RowOf(profile, "shadow"), ColumnOf(profile, "interface")) == Access::None);
}

TEST_CASE("a resource's lifetime is where it is written to where it is last read", "[graph][profile]") {
	const PipelineProfile profile = Profiled(DefaultGraph());

	const ProfileResource &depth = profile.Resources[RowOf(profile, "linear-depth")];
	const auto written = static_cast<uint32_t>(ColumnOf(profile, "depth-linearise"));
	const auto lastRead = static_cast<uint32_t>(ColumnOf(profile, "deferred-lighting"));

	CHECK(depth.FirstWrite == written);
	CHECK(depth.LastRead == lastRead);

	// **Closed at both ends.** Alive at the pass that wrote it and at the pass
	// that last read it - an allocator that freed it a pass early would hand its
	// memory to something else while deferred lighting was still sampling it.
	CHECK(depth.LiveAt(written));
	CHECK(depth.LiveAt(lastRead));
	CHECK_FALSE(depth.LiveAt(lastRead + 1));

	// And it is not alive before anything wrote it.
	if (written > 0) {
		CHECK_FALSE(depth.LiveAt(written - 1));
	}
}

TEST_CASE("an external resource is alive for the whole frame", "[graph][profile]") {
	const PipelineProfile profile = Profiled(DefaultGraph());

	// The swapchain exists before the frame and after it, so no pass boundary
	// frees it and nothing may be aliased over it.
	const ProfileResource &window = profile.Resources[RowOf(profile, "window")];
	REQUIRE(window.External);

	for (uint32_t pass = 0; pass < profile.Passes.size(); pass++) {
		CHECK(window.LiveAt(pass));
	}
}

// --- what a frame costs -------------------------------------------------------------

TEST_CASE("a resource is priced from its format and its resolution", "[graph][profile]") {
	RenderGraph graph;

	// Full resolution, sixty-four bits a pixel.
	const ResourceId full = graph.AddResource(
		{.Name = Name("full"), .Kind = ResourceKind::Colour, .Format = ResourceFormat::RGBA16F}
	);

	// Quarter on each axis, so a sixteenth of the pixels - the shape of every
	// downsample chain in a real frame.
	const ResourceId small = graph.AddResource({
		.Name = Name("small"),
		.Kind = ResourceKind::Colour,
		.Format = ResourceFormat::RGBA16F,
		.Divisor = 4,
	});

	const ResourceId window =
		graph.AddResource({.Name = Name("window"), .Kind = ResourceKind::Colour, .External = true});

	graph.AddNode({
		.Name = Name("opaque"),
		.Kind = Name("opaque"),
		.Reads = {},
		.Writes = {full},
		.Scope = NodeScope::Frame,
		.Parameters = {},
	});
	graph.AddNode({
		.Name = Name("down"),
		.Kind = Name("scale"),
		.Reads = {full},
		.Writes = {small},
		.Scope = NodeScope::Frame,
		.Parameters = {},
	});
	graph.AddNode({
		.Name = Name("present"),
		.Kind = Name("present"),
		.Reads = {small},
		.Writes = {window},
		.Scope = NodeScope::Frame,
		.Parameters = {},
	});

	const PipelineProfile profile = Profiled(graph);

	const ProfileResource &wide = profile.Resources[RowOf(profile, "full")];
	const ProfileResource &narrow = profile.Resources[RowOf(profile, "small")];

	CHECK(wide.Width == WIDTH);
	CHECK(wide.Bytes == static_cast<uint64_t>(WIDTH) * HEIGHT * 8);

	// **The divisor is the whole "sampling smaller" feature**, and this is the
	// line that proves it reaches the number a memory budget is against.
	CHECK(narrow.Width == WIDTH / 4);
	CHECK(narrow.Height == HEIGHT / 4);
	CHECK(narrow.Bytes == wide.Bytes / 16);
}

TEST_CASE("the peak is below the sum when lifetimes do not overlap", "[graph][profile]") {
	RenderGraph graph;

	// Three intermediates in a chain, each dead before the next is written. A
	// transient allocator would give all three the same memory.
	const ResourceId a = graph.AddResource({.Name = Name("a"), .Kind = ResourceKind::Colour});
	const ResourceId b = graph.AddResource({.Name = Name("b"), .Kind = ResourceKind::Colour});
	const ResourceId c = graph.AddResource({.Name = Name("c"), .Kind = ResourceKind::Colour});
	const ResourceId window =
		graph.AddResource({.Name = Name("window"), .Kind = ResourceKind::Colour, .External = true});

	graph.AddNode({
		.Name = Name("one"),
		.Kind = Name("opaque"),
		.Reads = {},
		.Writes = {a},
		.Scope = NodeScope::Frame,
		.Parameters = {},
	});
	graph.AddNode(
		{.Name = Name("two"),
		 .Kind = Name("blit"),
		 .Reads = {a},
		 .Writes = {b},
		 .Scope = NodeScope::Frame,
		 .Parameters = {}}
	);
	graph.AddNode(
		{.Name = Name("three"),
		 .Kind = Name("blit"),
		 .Reads = {b},
		 .Writes = {c},
		 .Scope = NodeScope::Frame,
		 .Parameters = {}}
	);
	graph.AddNode({
		.Name = Name("present"),
		.Kind = Name("present"),
		.Reads = {c},
		.Writes = {window},
		.Scope = NodeScope::Frame,
		.Parameters = {},
	});

	const PipelineProfile profile = Profiled(graph);

	// **The gap between these two is what aliasing would recover**, and it is
	// the number this whole type exists to produce. Without it, "the frame uses
	// N megabytes" is a question with two very different answers and no way to
	// tell which one somebody meant.
	CHECK(profile.PeakBytes < profile.TotalBytes);
	CHECK(profile.TotalBytes > 0);
}

// --- the shape of the grid -----------------------------------------------------------

TEST_CASE("rows keep declaration order and columns keep execution order", "[graph][profile]") {
	const PipelineProfile profile = Profiled(DefaultGraph());

	// **Rows in declaration order, so one does not move when a pass is
	// reordered.** A grid whose rows shuffled between two runs is one nobody can
	// read across a change, which is exactly when somebody needs to.
	CHECK(profile.Resources[0].Name == Name("shadow"));
	CHECK(profile.Resources[1].Name == Name("surface"));

	// Columns in the order the frame runs, which is `Compile`'s answer and not
	// this type's to re-derive.
	CHECK(profile.Passes[0].Name == Name("world"));
	CHECK(RowOf(profile, "display") > 0);
}

TEST_CASE("an out-of-range cell is no access rather than a crash", "[graph][profile]") {
	const PipelineProfile profile = Profiled(DefaultGraph());

	// A panel walks this grid every frame while somebody resizes the window and
	// edits the pipeline. Returning `None` beats indexing past the end.
	CHECK(profile.At(profile.Resources.size(), 0) == Access::None);
	CHECK(profile.At(0, profile.Passes.size()) == Access::None);
}

TEST_CASE("an empty graph profiles to an empty grid", "[graph][profile]") {
	const RenderGraph graph;
	const PipelineProfile profile = Profiled(graph);

	CHECK(profile.Passes.empty());
	CHECK(profile.Cells.empty());
	CHECK(profile.PeakBytes == 0);
}

TEST_CASE("nothing is measured until something measures it", "[graph][profile]") {
	const PipelineProfile profile = Profiled(DefaultGraph());

	// **Zero means "not measured", and a panel has to say so.** Showing an
	// unmeasured pass as costing nothing would be the most misleading thing this
	// whole subsystem could do - a frame where every pass reads 0.00 ms looks
	// like a frame with no problems.
	for (const engine::graph::ProfilePass &pass : profile.Passes) {
		CHECK(pass.Elapsed == 0.0);
		CHECK(pass.Wall == 0.0);
	}
}
