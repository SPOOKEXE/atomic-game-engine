#include <engine/graph/Pipeline.hpp>

#include <algorithm>
#include <unordered_set>

namespace engine::graph {

	const char *Describe(PipelineStatus status) {
		switch (status) {
		case PipelineStatus::Ok:
			return "ok";
		case PipelineStatus::ReadsBeforeWrite:
			return "reads a target nothing earlier wrote";
		case PipelineStatus::DuplicateStage:
			return "shares a name with an earlier stage";
		case PipelineStatus::WritesNothing:
			return "writes nothing, so nothing can observe it";
		}
		return "unknown";
	}

	void Pipeline::Add(Stage stage) {
		Steps.push_back(std::move(stage));
	}

	PipelineStatus Pipeline::Validate(core::Name &offender) const {
		std::unordered_set<uint32_t> written;
		std::unordered_set<uint32_t> named;

		for (const Stage &stage : Steps) {
			offender = stage.Name;

			if (!named.insert(stage.Name.Id()).second) {
				return PipelineStatus::DuplicateStage;
			}
			if (stage.Writes.empty()) {
				return PipelineStatus::WritesNothing;
			}

			// **Reads checked before writes are recorded**, so a stage cannot
			// satisfy its own read. A pass that both reads and writes one target
			// — the transparent pass reading the depth the opaque pass wrote —
			// is expressed as reading what an *earlier* stage wrote, which is
			// exactly what makes reordering it an error rather than a silence.
			for (const Attachment &read : stage.Reads) {
				if (written.find(read.Name.Id()) == written.end()) {
					return PipelineStatus::ReadsBeforeWrite;
				}
			}

			for (const Attachment &write : stage.Writes) {
				written.insert(write.Name.Id());
			}
		}

		offender = core::Name{};
		return PipelineStatus::Ok;
	}

	Pipeline StandardPipeline() {
		Pipeline pipeline;

		// **Shadows first, and that is the whole reason the order is data.** The
		// colour pass samples the shadow map, so the map has to exist before it
		// runs — and moving this line below the next one is not a crash on a
		// GPU, it is a frame lit by whatever was in that memory. `Validate` is
		// what turns that into an error somebody can read.
		//
		// Per light rather than per view: a shadow map is a function of where
		// the light is, so four split-screen views of one world share one.
		pipeline.Add(
			Stage{
				core::Name("shadow"),
				{},
				{Attachment{core::Name("shadow"), true}},
				false,
				true,
			}
		);

		// **The surface pass, between the shadow map and the screen.** A second
		// view rendered into a texture, which a mirror samples a frame later —
		// so it reads the shadow map and writes a target nothing else does.
		//
		// Per light rather than per view for the same reason the shadow pass is:
		// there is one surface camera and it does not move with the eye.
		pipeline.Add(
			Stage{
				core::Name("surface"),
				{Attachment{core::Name("shadow"), false}},
				{Attachment{core::Name("surface"), true}, Attachment{core::Name("surfaceDepth"), true}},
				false,
				true,
			}
		);

		// The opaque pass. Clears both targets, because it is the first thing
		// that touches them in a frame.
		pipeline.Add(
			Stage{
				core::Name("opaque"),
				{Attachment{core::Name("shadow"), false}, Attachment{core::Name("surface"), false}},
				{Attachment{core::Name("colour"), true}, Attachment{core::Name("depth"), true}},
				true,
				false,
			}
		);

		// The transparent pass. **Loads rather than clears**, and reads the
		// depth the opaque pass wrote: a pane behind a wall must be hidden by
		// it, so the test stays even though the write does not.
		//
		// Optional, because a scene with no transparency should not pay a
		// pipeline switch to draw nothing.
		pipeline.Add(
			Stage{
				core::Name("transparent"),
				{Attachment{core::Name("depth"), false}, Attachment{core::Name("shadow"), false}},
				{Attachment{core::Name("colour"), false}},
				true,
				true,
			}
		);

		// The overlay. No depth at all: it is on top of everything by
		// definition, which is why it neither reads nor writes one.
		pipeline.Add(
			Stage{
				core::Name("overlay"),
				{},
				{Attachment{core::Name("colour"), false}},
				true,
				true,
			}
		);

		return pipeline;
	}
}
