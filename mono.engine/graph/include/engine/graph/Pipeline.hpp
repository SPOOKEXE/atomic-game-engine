#pragma once

// What a frame does, as data rather than as a function body.
//
// **The point of a graph is that the order is inspectable.** A renderer that
// hardcodes "cull, then shadows, then colour, then overlay" works exactly as
// well until something needs to ask what the passes are — a debug overlay
// listing them, a second view that skips one, an editor turning shadows off —
// and then every one of those has to know the function's shape rather than read
// a list.
//
// So the frame is a **list of stages** and each one is a small record. This
// module does not execute them: executing needs a device, and a device is
// `render`'s. What it owns is the decision of what runs, in what order, over
// what.
//
// **This is not a general dependency-resolving frame graph, and that is
// deliberate.** The kind that topologically sorts resource reads and writes
// earns its complexity at twenty passes and costs more than it returns at four.
// The stage list below is ordered by declaration, and `Validate` checks the one
// property that actually goes wrong at this size — a stage reading something no
// earlier stage wrote.
//
// @tier L9 · shared

#include <engine/core/Name.hpp>

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace engine::graph {

	// What a stage reads from and writes to.
	//
	// Names rather than handles, and rule 4 is the reason: a stage list is the
	// sort of thing a game file or a tool will eventually carry, and an index
	// into a vector of targets means a different target the moment the list is
	// reordered.
	//
	// @since v0.6
	struct Attachment {
		// What the target is called — `colour`, `depth`, `shadow`.
		core::Name Name;

		// Whether the stage clears it before writing.
		//
		// A load is what makes a second pass draw *onto* the first rather than
		// over it. The transparent pass and the overlay both depend on it.
		bool Clear = false;
	};

	// One pass of a frame.
	//
	// @since v0.6
	struct Stage {
		// What this stage is called, for a profiler and for `Validate`'s
		// messages.
		core::Name Name;

		// What it must be able to read.
		std::vector<Attachment> Reads;

		// What it writes.
		std::vector<Attachment> Writes;

		// Whether the stage's work is per view.
		//
		// **The distinction that decides how many worlds cost.** A shadow map is
		// per *light* and can be shared by every view of a world; a colour pass
		// is per view and cannot. Four split-screen views of one world therefore
		// pay for one shadow map and four colour passes, and it is this flag
		// that says so rather than a comment somewhere in the executor.
		bool PerView = true;

		// Whether the stage may be skipped when nothing it draws exists.
		//
		// The transparent pass over a scene with no transparency, the overlay
		// with no panels open. Named rather than inferred, so a skipped stage is
		// a decision the list records instead of a branch the executor took.
		bool Optional = false;
	};

	// Why a stage list is not runnable.
	//
	// @since v0.6
	enum class PipelineStatus : uint8_t {
		// It is runnable.
		Ok,

		// A stage reads a target nothing earlier wrote.
		ReadsBeforeWrite,

		// Two stages share a name, so a profiler and an error message cannot
		// tell them apart.
		DuplicateStage,

		// A stage writes nothing, which is a stage that cannot be observed.
		WritesNothing,
	};

	// A stable, human-readable name for a status.
	const char *Describe(PipelineStatus status);

	// The stages of a frame, in order.
	//
	// @since v0.6
	class Pipeline {
	  public:
		// Appends a stage.
		//
		// @param stage The stage to add.
		void Add(Stage stage);

		// The stages, in declaration order.
		//
		// **Refused on a temporary, and that is not pedantry.** The span points
		// into this object, so `StandardPipeline().Stages()` hands back a view
		// of a `Pipeline` that died at the end of the expression — and the
		// memory stays readable for a while, so the mistake passes its tests
		// until it does not. A test wrote exactly that and got three assertions
		// comparing freed names; the `&&` overload turns it into a compile
		// error.
		//
		// @return The list, valid while this pipeline is.
		std::span<const Stage> Stages() const & {
			return Steps;
		}

		std::span<const Stage> Stages() const && = delete;

		// Checks the one property that goes wrong at this size.
		//
		// **A stage reading a target nothing wrote.** That is the mistake a
		// reordering makes — moving the shadow pass after the colour pass leaves
		// the colour pass sampling a texture that has not been rendered, and on
		// a GPU that is not a crash but a frame lit by whatever was in the
		// memory. Checked here so it is an error at construction rather than a
		// black scene nobody can explain.
		//
		// @param offender Filled in with the stage that failed the check.
		// @return `Ok`, or why not.
		PipelineStatus Validate(core::Name &offender) const;

		// How many stages the list holds.
		size_t Count() const {
			return Steps.size();
		}

	  private:
		std::vector<Stage> Steps;
	};

	// The stages v0.6 ships, in order.
	//
	// **Colour, shadows and a culled draw**, which is exactly what the roadmap
	// line asks for and no more. What is absent is absent on purpose:
	//
	// - **No HDR and no G-buffer.** Both are v0.8's, and both change what every
	//   later stage reads — adding them now would be writing a pipeline for a
	//   renderer that does not exist.
	// - **No post-processing chain.** There is nothing to post-process yet.
	//
	// @return The pipeline.
	Pipeline StandardPipeline();
}
