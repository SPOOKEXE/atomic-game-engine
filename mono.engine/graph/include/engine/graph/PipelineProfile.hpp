#pragma once

// A frame as a grid: every pass across the top, every resource down the side,
// and what each pass does to each resource where they meet.
//
// **This is Unity's Render Graph Viewer, and it is a better answer to "show me
// what happens" than a node canvas is.** A canvas shows *intent* — what
// somebody wired. This shows *consequence* — what will actually run, in what
// order, touching what, and how much memory is live while it does.
// `docs/PIPELINE_NODES.md` §1.2 and §7 carry the argument at length.
//
// ## Derived, never maintained
//
// Everything here comes from a `RenderGraph` and its `CompiledGraph`. Nothing
// is recorded, nothing is hand-kept in step, and there is no second description
// of the frame — which is the rule `DEFERRED.md` D00016 exists to enforce and
// which a subsystem this size could easily have broken.
//
// ## What it can and cannot say
//
// It can say what every pass reads and writes, when each resource comes alive
// and when it dies, how many bytes each costs at a given view size, and — the
// number worth having — what the **peak** live footprint is, against the naive
// sum. The gap between those two is what a transient allocator would recover by
// aliasing resources whose lifetimes do not overlap.
//
// It cannot say what anything *cost in time*. That needs timestamp queries
// around each pass, which is the stage after this one; `ProfilePass::Elapsed`
// is where those land, and it is zero until something measures them.
//
// @tier L9 · shared

#include <engine/graph/PipelineView.hpp>
#include <engine/graph/RenderGraph.hpp>

#include <cstdint>
#include <vector>

namespace engine::graph {

	// What one pass does to one resource.
	//
	// **Four states and a fifth that is the absence of one.** Unity draws these
	// as coloured blocks — green read, red write, both for read-write — and the
	// colouring is the whole reason the grid is readable at a glance.
	//
	// @since v0.11
	enum class Access : uint8_t {
		// The pass does not touch it.
		None,

		// Reads it.
		Read,

		// Writes it without reading it — a full replacement.
		Write,

		// Both, which is a refinement: `transparent` blending over `colour`.
		ReadWrite,
	};

	// A stable, human-readable name for an access.
	//
	// @param access The access to name.
	// @return A view valid for the lifetime of the process.
	const char *Describe(Access access);

	// One column of the grid.
	//
	// @since v0.11
	struct ProfilePass {
		// Which node this is.
		//@{
		NodeId Node;
		core::Name Name;
		core::Name Kind;
		//@}

		// Which block of the frame it runs in.
		Band Where = Band::PerView;

		// How long it took, in microseconds.
		//
		// **Zero until something measures it**, and a panel must show that as
		// "not measured" rather than as "free". The stage that fills this needs
		// timestamp queries around every pass; the grid is useful without it,
		// which is why the two are separable.
		double Elapsed = 0.0;
	};

	// One row of the grid.
	//
	// @since v0.11
	struct ProfileResource {
		// Which resource, and what it is.
		//@{
		ResourceId Id;
		core::Name Name;
		ResourceKind Kind = ResourceKind::Colour;
		ResourceFormat Format = ResourceFormat::RGBA8;
		//@}

		// Its size at the view size the profile was taken for.
		//@{
		uint32_t Width = 0;
		uint32_t Height = 0;
		//@}

		// What it costs, in bytes, at that size.
		uint64_t Bytes = 0;

		// Whether it lives outside the graph — the swapchain, a history buffer.
		// An external resource is alive for the whole frame by definition.
		bool External = false;

		// The value meaning "never".
		static constexpr uint32_t NEVER = 0xFFFFFFFFu;

		// The first pass that writes it and the last that reads it, as indices
		// into `PipelineProfile::Passes`.
		//
		// **This pair is the lifetime**, and the lifetime is what makes aliasing
		// possible: two resources whose spans do not overlap can share memory.
		// `NEVER` on either means the resource is never written or never read,
		// which `PipelineDiagnostics` reports as a fault rather than this.
		//@{
		uint32_t FirstWrite = NEVER;
		uint32_t LastRead = NEVER;
		//@}

		// Whether it is live at a pass.
		//
		// **Half-open at the start and closed at the end**: alive from the pass
		// that first writes it, through the pass that last reads it. A resource
		// nothing writes is never alive; an external one always is.
		//
		// @param at The pass index.
		// @return Whether memory has to exist for it then.
		bool LiveAt(uint32_t at) const {
			if (External) {
				return true;
			}
			if (FirstWrite == NEVER) {
				return false;
			}
			const uint32_t last = LastRead == NEVER ? FirstWrite : LastRead;
			return at >= FirstWrite && at <= last;
		}
	};

	// The whole grid.
	//
	// @since v0.11
	struct PipelineProfile {
		// The columns, in execution order.
		std::vector<ProfilePass> Passes;

		// The rows, in declaration order.
		//
		// **Declaration order and not first-use order.** It is the order the
		// author wrote them in and the order the document saves them in, so a
		// row does not move when somebody reorders a pass — which is the whole
		// difference between a grid you can read across two runs and one you
		// cannot.
		std::vector<ProfileResource> Resources;

		// What each pass does to each resource, `Resources.size()` rows of
		// `Passes.size()`.
		//
		// **A flat vector rather than a map**, because every cell is drawn every
		// frame and a lookup per cell would be the panel's cost rather than the
		// pipeline's.
		std::vector<Access> Cells;

		// The most bytes live at any one pass.
		//
		// **The number a memory budget is actually against**, and the one an
		// engine without a transient allocator does not get: see `TotalBytes`.
		uint64_t PeakBytes = 0;

		// What every resource costs added together.
		//
		// **The gap between this and `PeakBytes` is what aliasing would
		// recover.** A frame with a long chain of half-resolution intermediates
		// can easily allocate three times what it ever has live at once.
		uint64_t TotalBytes = 0;

		// What a pass does to a resource.
		//
		// @param resource The row.
		// @param pass     The column.
		// @return The access, or `None` for an out-of-range pair.
		Access At(size_t resource, size_t pass) const {
			if (resource >= Resources.size() || pass >= Passes.size()) {
				return Access::None;
			}
			return Cells[resource * Passes.size() + pass];
		}
	};

	// Works out the grid.
	//
	// **Takes the view size, because half of what makes a frame expensive is
	// resolution** and a resource that follows the view has no size until one is
	// named. `PIPELINE_NODES.md` §1.4 counts six resolutions in one frame; a
	// profile that could not price them would be reporting the wrong number for
	// every intermediate in a downsample chain.
	//
	// @param graph      The pipeline.
	// @param compiled   What `Compile` produced. Its order is the column order.
	// @param viewWidth  The view's width in pixels.
	// @param viewHeight Its height.
	// @return The grid. Empty for an empty compile.
	PipelineProfile ProfilePipeline(
		const RenderGraph &graph, const CompiledGraph &compiled, uint32_t viewWidth, uint32_t viewHeight
	);
}
