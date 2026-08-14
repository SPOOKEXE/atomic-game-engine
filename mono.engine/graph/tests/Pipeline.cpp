#include <engine/graph/Pipeline.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

TEST_SUITE_ID("engine.graph.pipeline")

using engine::core::Name;
using engine::graph::Attachment;
using engine::graph::Pipeline;
using engine::graph::PipelineStatus;
using engine::graph::Stage;
using engine::graph::StandardPipeline;

namespace {
	Stage Writing(const char *name, const char *target) {
		return Stage{Name(name), {}, {Attachment{Name(target), true}}, true, false};
	}

	Stage Reading(const char *name, const char *source, const char *target) {
		return Stage{
			Name(name), {Attachment{Name(source), false}}, {Attachment{Name(target), false}}, true, false
		};
	}
}

TEST_CASE("the standard pipeline validates", "[graph][pipeline]") {
	Name offender;
	CHECK(StandardPipeline().Validate(offender) == PipelineStatus::Ok);
	CHECK_FALSE(offender.IsValid());
}

TEST_CASE("the standard pipeline is the six stages v0.7 ships", "[graph][pipeline]") {
	const Pipeline pipeline = StandardPipeline();
	REQUIRE(pipeline.Count() == 6);

	const auto stages = pipeline.Stages();
	CHECK(stages[0].Name == Name("shadow"));
	CHECK(stages[1].Name == Name("surface"));
	CHECK(stages[2].Name == Name("opaque"));
	CHECK(stages[3].Name == Name("transparent"));
	CHECK(stages[4].Name == Name("overlay"));

	// The editor's, added at v0.7. **Last, and the position is the assertion
	// worth making** - `interface` before `overlay` would put the debug panels
	// on top of the studio's own windows, which is the one arrangement in which
	// the profiler you opened to explain a slow frame is the thing you cannot
	// see.
	CHECK(stages[5].Name == Name("interface"));
}

TEST_CASE("the surface pass reads the shadow map and the opaque pass reads both", "[graph][pipeline]") {
	// **The ordering `Validate` is here to protect.** A mirror shows a lit
	// world, so its own pass needs the shadow map - and the screen pass needs
	// the surface, so moving it earlier would sample a texture nothing had
	// rendered. On a GPU that is not a crash, it is a mirror showing whatever
	// was in that memory.
	const Pipeline pipeline = StandardPipeline();
	const auto stages = pipeline.Stages();

	REQUIRE(stages[1].Reads.size() == 1);
	CHECK(stages[1].Reads[0].Name == Name("shadow"));

	REQUIRE(stages[2].Reads.size() == 2);
	CHECK(stages[2].Reads[0].Name == Name("shadow"));
	CHECK(stages[2].Reads[1].Name == Name("surface"));
}

TEST_CASE("a shadow map is per light and a colour pass is per view", "[graph][pipeline]") {
	// The distinction that decides what many worlds cost: four split-screen
	// views of one world pay for one shadow map and four colour passes.
	const Pipeline pipeline = StandardPipeline();
	const auto stages = pipeline.Stages();

	CHECK_FALSE(stages[0].PerView);

	// The surface camera does not move with the eye either, so several views of
	// one world share its texture exactly as they share the shadow map.
	CHECK_FALSE(stages[1].PerView);

	CHECK(stages[2].PerView);
	CHECK(stages[3].PerView);
}

TEST_CASE("the passes that can be skipped say so", "[graph][pipeline]") {
	// A scene with no transparency should not pay a pipeline switch to draw
	// nothing, and the overlay with no panels open is the same. Named rather
	// than inferred, so it is a decision the list records.
	const Pipeline pipeline = StandardPipeline();
	const auto stages = pipeline.Stages();

	CHECK_FALSE(stages[2].Optional);
	CHECK(stages[1].Optional);
	CHECK(stages[3].Optional);
	CHECK(stages[4].Optional);
}

TEST_CASE("the opaque pass clears and the transparent one loads", "[graph][pipeline]") {
	// The difference between drawing *onto* the first pass and drawing *over*
	// it. A transparent pass that cleared would erase the scene it blends with.
	const Pipeline pipeline = StandardPipeline();
	const auto stages = pipeline.Stages();

	CHECK(stages[2].Writes[0].Clear);
	CHECK_FALSE(stages[3].Writes[0].Clear);
	CHECK_FALSE(stages[4].Writes[0].Clear);
}

TEST_CASE("reordering the shadow pass after the colour pass is an error", "[graph][pipeline]") {
	// **The mistake this check exists for.** On a GPU it is not a crash - it is
	// a frame lit by whatever was in that memory, which reads as a lighting bug
	// rather than as an ordering one.
	Pipeline pipeline;
	pipeline.Add(Reading("opaque", "shadow", "colour"));
	pipeline.Add(Writing("shadow", "shadow"));

	Name offender;
	CHECK(pipeline.Validate(offender) == PipelineStatus::ReadsBeforeWrite);
	CHECK(offender == Name("opaque"));
}

TEST_CASE("a stage cannot satisfy its own read", "[graph][pipeline]") {
	// Reads are checked before writes are recorded, so a pass that samples what
	// it also writes has to name an *earlier* stage - which is what makes
	// moving it an error rather than a silence.
	Pipeline pipeline;
	pipeline.Add(Reading("self", "colour", "colour"));

	Name offender;
	CHECK(pipeline.Validate(offender) == PipelineStatus::ReadsBeforeWrite);
	CHECK(offender == Name("self"));
}

TEST_CASE("two stages cannot share a name", "[graph][pipeline]") {
	Pipeline pipeline;
	pipeline.Add(Writing("pass", "colour"));
	pipeline.Add(Writing("pass", "depth"));

	Name offender;
	CHECK(pipeline.Validate(offender) == PipelineStatus::DuplicateStage);
	CHECK(offender == Name("pass"));
}

TEST_CASE("a stage that writes nothing is refused", "[graph][pipeline]") {
	// Nothing can observe it, so it is either dead or a mistake - and both are
	// worth an error at construction.
	Pipeline pipeline;
	pipeline.Add(Stage{Name("silent"), {}, {}, true, false});

	Name offender;
	CHECK(pipeline.Validate(offender) == PipelineStatus::WritesNothing);
	CHECK(offender == Name("silent"));
}

TEST_CASE("an empty pipeline validates", "[graph][pipeline]") {
	Name offender;
	CHECK(Pipeline{}.Validate(offender) == PipelineStatus::Ok);
	CHECK(Pipeline{}.Count() == 0);
}

TEST_CASE("every status has a name", "[graph][pipeline]") {
	for (const PipelineStatus status :
		 {PipelineStatus::Ok,
		  PipelineStatus::ReadsBeforeWrite,
		  PipelineStatus::DuplicateStage,
		  PipelineStatus::WritesNothing}) {
		CHECK(std::string(engine::graph::Describe(status)) != "unknown");
	}
}
