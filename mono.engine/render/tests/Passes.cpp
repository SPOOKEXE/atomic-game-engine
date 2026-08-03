// The check D00016 was filed for.
//
// `Renderer::Render` submits five passes from a function that knows all five by
// name, and `graph::StandardPipeline` is the same five as data. Keeping them in
// step used to be a sentence in `mono.engine/render/AGENTS.md` — rule 6 out
// loud, a rule the build did not check. This file is the build checking it.
//
// **No device is created here and none is needed.** `PassOrder` is a list of
// names and `StandardPipeline` is arithmetic over names, so the comparison runs
// anywhere. That is the whole reason the enum exists as a separate thing from
// the function body: the body needs a GPU, the description does not.

#include <engine/graph/Pipeline.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

TEST_SUITE_ID("engine.render.passes")

using engine::core::Name;
using engine::graph::Pipeline;
using engine::graph::PipelineStatus;
using engine::graph::Stage;
using engine::graph::StandardPipeline;
using engine::render::FrameResult;
using engine::render::Pass;
using engine::render::PassOrder;

namespace {
	// The stage names, in declaration order.
	//
	// Bound to a named `Pipeline` rather than taken from the temporary:
	// `Stages()` is deleted on an rvalue for exactly this reason, and writing
	// the mistake here once was how that overload came to exist.
	std::vector<Name> StageNames(const Pipeline &pipeline) {
		std::vector<Name> names;
		for (const Stage &stage : pipeline.Stages()) {
			names.push_back(stage.Name);
		}
		return names;
	}
}

TEST_CASE("the renderer's passes are the standard pipeline's stages", "[render][graph]") {
	const Pipeline pipeline = StandardPipeline();
	const std::vector<Name> stages = StageNames(pipeline);
	const auto order = PassOrder();

	// **Count first, and the message matters more than the assertion.** This is
	// the failure the entry predicted: a sixth pass added to one description and
	// not the other. Whichever side was edited, the fix is to edit both.
	INFO(
		"Renderer submits " << order.size() << " passes and StandardPipeline declares " << stages.size()
							<< " stages. A pass added to one must be added to the other — see D00016."
	);
	REQUIRE(order.size() == stages.size());

	// **In order, not as a set.** A pipeline holding the right five stages in
	// the wrong order describes a frame where the colour pass samples a shadow
	// map nothing has rendered, which is not a crash on a GPU — it is a frame
	// lit by whatever was in that memory.
	for (size_t index = 0; index < stages.size(); index++) {
		INFO("position " << index);
		CHECK(order[index].Text() == stages[index].Text());
	}
}

TEST_CASE("the pass enum indexes its own names", "[render]") {
	// `PassRecorder` shifts by the enum value and `PassOrder` is indexed by it,
	// so a member inserted in the middle without moving the name beside it
	// silently renames every pass after it. Cheap to assert and impossible to
	// notice otherwise.
	const auto order = PassOrder();

	REQUIRE(order.size() == static_cast<size_t>(Pass::Count));
	CHECK(order[static_cast<size_t>(Pass::Shadow)].Text() == "shadow");
	CHECK(order[static_cast<size_t>(Pass::Surface)].Text() == "surface");
	CHECK(order[static_cast<size_t>(Pass::Opaque)].Text() == "opaque");
	CHECK(order[static_cast<size_t>(Pass::Transparent)].Text() == "transparent");
	CHECK(order[static_cast<size_t>(Pass::Overlay)].Text() == "overlay");
}

TEST_CASE("the standard pipeline the renderer follows is runnable", "[render][graph]") {
	// The other half of the pairing. `PassOrder` says the renderer agrees with
	// the pipeline; this says the pipeline is worth agreeing with — no stage
	// reads a target that no earlier stage wrote.
	const Pipeline pipeline = StandardPipeline();

	Name offender;
	INFO("offending stage: " << std::string(offender.Text()));
	CHECK(pipeline.Validate(offender) == PipelineStatus::Ok);
}

TEST_CASE("a frame result reports which passes ran", "[render]") {
	// Every pass is conditional, so "ran" is a different question from "exists"
	// and the draw-call count cannot answer it — the shadow pass and the overlay
	// pass are one draw each and are indistinguishable from there.
	FrameResult result;

	for (uint8_t index = 0; index < static_cast<uint8_t>(Pass::Count); index++) {
		CHECK_FALSE(result.Ran(static_cast<Pass>(index)));
	}

	result.Passes = static_cast<uint8_t>(
		(1u << static_cast<uint8_t>(Pass::Opaque)) | (1u << static_cast<uint8_t>(Pass::Overlay))
	);

	CHECK(result.Ran(Pass::Opaque));
	CHECK(result.Ran(Pass::Overlay));

	// The interesting one: a frame that drew but cast no shadows, which is what
	// a scene with no casters looks like and is not a bug.
	CHECK_FALSE(result.Ran(Pass::Shadow));
	CHECK_FALSE(result.Ran(Pass::Surface));
	CHECK_FALSE(result.Ran(Pass::Transparent));
}
