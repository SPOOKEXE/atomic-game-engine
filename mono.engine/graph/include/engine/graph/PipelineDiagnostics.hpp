#pragma once

// What is wrong with a pipeline, worked out from the pipeline alone.
//
// **This is the cheapest useful thing in `docs/PIPELINE_NODES.md` and the
// reason that document exists.** Its §1.5 tabulates eleven faults found by a
// specialist reading a GPU capture of a shipping frame for an afternoon - a
// target cleared and never used, a scene copied into a buffer nothing reads
// again, normals in four times the bits they need, a group of draws whose
// resources connect to nothing before or after them.
//
// Six of the eleven are **static properties of the graph**. They need no
// capture, no GPU, no timing and no readback: they are arithmetic over who
// writes what and who reads it, which is exactly what a `RenderGraph` already
// holds. A frame profiler would find them too, eventually, at the cost of a
// profiler.
//
// ## Reported, never refused
//
// Nothing here returns a status. A pipeline mid-edit is half-wired by
// definition - `PipelineDocument::Record` makes the same argument - and a
// diagnostic that blocked a compile would be an editor nobody could build
// anything in. `RenderGraph::Compile` refuses what cannot run; this describes
// what can run and should not.
//
// ## Every check names a node
//
// So a panel can draw a mark on a box and a headless test can assert on the
// name rather than on a message. The message is for a human and may be
// rephrased; `DiagnosticKind` and the names are the contract.
//
// @tier L9 · shared

#include <engine/graph/PipelineCatalogue.hpp>
#include <engine/graph/RenderGraph.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace engine::graph {

	// What sort of fault a `Diagnostic` reports.
	//
	// **One entry per row of `PIPELINE_NODES.md` §1.5 that a graph can answer.**
	// The three that need a readback - is this channel constant, is this target
	// uniform, how many times was this pixel shaded - are deliberately absent,
	// because guessing at them from the declaration would produce a warning
	// nobody could act on.
	//
	// @since v0.11
	enum class DiagnosticKind : uint8_t {
		// A resource something writes and nothing reads.
		//
		// Fault 1 and fault 7: an `R8` cleared and never used, and a frame
		// copied into a buffer that is never touched again.
		DeadResource,

		// A write wholly overwritten before anything reads it.
		//
		// Fault 2. The clearest case is a clear followed by a copy over the
		// whole target, which is work paid for twice and used once.
		WastedWrite,

		// A node whose every output is unread. It could be deleted.
		DeadNode,

		// A node that reaches no output of the frame.
		//
		// Fault 8: *"draws whose resources have nothing to do with previous or
		// following draws"*. Distinct from `DeadNode` because the node's own
		// outputs may well be read - by other nodes that also go nowhere.
		Disconnected,

		// A node reads a resource nothing writes.
		//
		// **Not the same as `GraphStatus::ReadsBeforeWrite`**, which is about
		// order. This is about a resource with no writer at all, which compiles
		// and samples whatever the allocator last left there.
		UnwrittenRead,

		// A target with more bits than any reader takes.
		//
		// Fault 6: normals in `RGBA16F` where `RGB10A2` carries them exactly.
		// Doubling the bandwidth of the widest target in a deferred frame is
		// worth a warning.
		FormatOverspend,

		// A wire that throws information away.
		//
		// Legal, and sometimes intended - a tone mapper's whole job is to land
		// HDR in fewer bits. A hint rather than a warning for that reason.
		LossyWire,

		// A pass declared before the pass that feeds it.
		//
		// Fault 11, and **the editor-facing half of
		// `GraphStatus::ReadsBeforeWrite`.** This engine's `Compile` refuses
		// such a graph rather than quietly sorting it - which is the better
		// behaviour and is not, on its own, enough to fix one: it reports a
		// *resource* name and stops at the first occurrence, so a panel can say
		// "will not run: ReadsBeforeWrite (shadow)" and nothing more.
		//
		// This names **both passes and every occurrence**: which one to move and
		// what it depends on. That is the difference between a diagnostic
		// somebody can act on and one they have to go and investigate, and it is
		// the whole reason a redundant-looking check earns its place.
		//
		// **Written on the assumption that `Compile` sorted silently**, which is
		// what Unreal and Unity both do and what `PIPELINE_NODES.md` §1.5
		// recorded. The test that was meant to prove the frame still ran is what
		// found otherwise.
		OutOfOrder,

		// A target with an alpha channel no reader looks at.
		//
		// Fault 3, as far as a declaration can reach: the writer's format has
		// four channels and every reader's slot takes fewer. It cannot know the
		// channel is *blank* - that needs a readback - but it can say nothing
		// is arranged to use it.
		UnusedAlpha,

		// A pass that reads the resource it writes.
		//
		// **The hazard `SDL_GPU` documents and nothing was checking.** Every
		// target this engine writes is *cycled* - SDL's answer to "do not
		// overwrite what a pending command still references" - and its rule is
		// that cycling leaves the resource's contents undefined until they are
		// written again. So a fullscreen pass wired to sample the target it is
		// drawing into does not read last frame's image or this one's: it reads
		// undefined memory, which on most drivers looks like a plausible frame
		// most of the time.
		//
		// **A read-modify-write needs two resources**, which is what every
		// engine's blur and every temporal resolve does - write B from A, then
		// A from B. The graph can say that, and this is what tells an author
		// they have not.
		//
		// Not reported for a target a pass merely *loads* rather than samples:
		// `transparent` blends onto what `opaque` wrote and declares both, and
		// that is a read-modify-write the hardware does natively inside one
		// render pass. Only a node that binds its own output as a **texture**
		// is caught - see `PipelineDiagnostics.cpp`.
		SamplesOwnTarget,
	};

	// A stable, human-readable name for a diagnostic kind.
	//
	// @param kind The kind to name.
	// @return A view valid for the lifetime of the process.
	const char *Describe(DiagnosticKind kind);

	// How much a diagnostic matters.
	//
	// @since v0.11
	enum class DiagnosticSeverity : uint8_t {
		// Almost certainly a mistake. Costs memory, bandwidth or both.
		Warning,

		// Might be deliberate. Shown, not shouted about.
		Hint,
	};

	// A stable, human-readable name for a severity.
	//
	// @param severity The severity to name.
	// @return A view valid for the lifetime of the process.
	const char *Describe(DiagnosticSeverity severity);

	// One thing wrong with a pipeline.
	//
	// @since v0.11
	struct Diagnostic {
		// What sort of fault.
		DiagnosticKind Kind = DiagnosticKind::DeadResource;

		// How much it matters.
		DiagnosticSeverity Severity = DiagnosticSeverity::Warning;

		// Which node to mark. Always set - a diagnostic a panel cannot place is
		// one nobody acts on.
		core::Name Node;

		// Which resource, when the fault is about one. Invalid otherwise.
		core::Name Resource;

		// A sentence for a human. **Not the contract** - assert on `Kind` and
		// `Node`, because this is the field that gets rephrased.
		std::string Message;
	};

	// Everything wrong with a graph.
	//
	// **Over the enabled nodes only.** A pass somebody switched off is not a
	// pass with a dead output; it is a pass that is off, and reporting it would
	// bury the real findings under one line per disabled node.
	//
	// **Needs the catalogue for the format checks**, and silently skips them for
	// a node whose kind is not registered - an unknown kind has no declared slot
	// formats to compare against, and guessing would be worse than not asking.
	// The structural checks work on any graph.
	//
	// @param graph The pipeline. Need not compile.
	// @return The findings, warnings before hints, and stable within each: by
	//         kind, then by node name. A panel redraws every frame and an order
	//         that moved would make the list unreadable.
	std::vector<Diagnostic> Diagnose(const RenderGraph &graph);
}
