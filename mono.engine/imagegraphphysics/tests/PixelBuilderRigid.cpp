#include "../../imagegraph/src/PixelBuilderPayload.hpp"
#include "RigidGraphFixture.hpp"

#include <engine/imagegraph/FeedbackHost.hpp>
#include <engine/imagegraph/PcxExpression.hpp>
#include <engine/imagegraph/StatefulReplay.hpp>
#include <engine/imagegraphphysics/RigidReplay.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
TEST_SUITE_ID("engine.imagegraphphysics.pixel_builder_rigid")
using namespace engine::imagegraph;
namespace {
	Document BuilderGraph() {
		auto document = engine::imagegraphphysics::testing::RigidGraphFixture();
		document.Nodes.front().GroupId = "pb";
		document.Groups.front().ParentId = "pb";
		document.Nodes.push_back(
			{"builder",
			 "pc.pixel_builder",
			 "",
			 {},
			 {{"dimension", Vector2{32, 32}}, {"dimension_unit", EnumValue{0}}}}
		);
		document.Nodes.push_back({"draw", "pc.pb_draw_surface", "pb", {}, {{"crop", false}}});
		document.Nodes.push_back({"layer", "pc.pb_output", "pb", {}, {}});
		document.Nodes.push_back({"gravity", "value.number", "", {}, {{"value", 1.}}});
		Group pb{"pb", "Builder"};
		pb.OwnerNodeId = "builder";
		document.Groups.push_back(pb);
		document.Links.push_back({"render", "surface_out", "draw", "surface"});
		document.Links.push_back({"draw", "surface", "layer", "surface"});
		document.Links.push_back({"gravity", "number", "owner", "strength"});
		document.Keyframes = {{"gravity", "value", 0, 1., "linear"}, {"gravity", "value", 12, 10., "linear"}};
		document.Outputs = {{"recipe", "builder", "dynamic_builder"}, {"pixels", "builder", "surface_out"}};
		return document;
	}
	Document NestedBuilderGraph() {
		auto document = BuilderGraph();
		for (auto &node : document.Nodes)
			if (node.Id == "builder") node.GroupId = "outer-scope";
		for (auto &group : document.Groups)
			if (group.Id == "pb") group.ParentId = "outer-scope";
		document.Nodes.push_back(
			{"outer",
			 "pc.pixel_builder",
			 "",
			 {},
			 {{"dimension", Vector2{32, 32}}, {"dimension_unit", EnumValue{0}}}}
		);
		document.Nodes.push_back({"outer-draw", "pc.pb_draw_surface", "outer-scope", {}, {{"crop", false}}});
		document.Nodes.push_back({"outer-layer", "pc.pb_output", "outer-scope", {}, {}});
		Group outer{"outer-scope", "Outer"};
		outer.OwnerNodeId = "outer";
		document.Groups.push_back(std::move(outer));
		document.Links.push_back({"builder", "dynamic_builder", "outer-draw", "surface"});
		document.Links.push_back({"outer-draw", "surface", "outer-layer", "surface"});
		document.Outputs = {{"recipe", "outer", "dynamic_builder"}, {"pixels", "outer", "surface_out"}};
		return document;
	}
	struct CapturedBuilder {
		DynamicSurfaceValue Recipe;
		Image Pixels;
		RigidReplayState Prior;
	};
	CapturedBuilder Capture(Document document = BuilderGraph()) {
		Plan plan;
		Diagnostic diagnostic;
		const auto compiled = Compile(document, plan, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(compiled == Status::Ok);
		engine::imagegraphphysics::RigidProvider provider;
		EvaluationRequest request;
		request.RigidProvider = &provider;
		request.RigidPlaying = request.RigidFrameProgress = true;
		request.RigidAuthoringRevision = 21;
		StatefulOutputEvaluationResult result;
		const std::array<std::string, 2> outputs{"recipe", "pixels"};
		RigidReplayState prior;
		for (uint64_t tick = 0; tick <= 12; ++tick) {
			request.Tick = tick;
			prior = result.Rigid;
			request.RigidReplay = tick ? &result.Rigid : nullptr;
			const auto status = EvaluateStatefulOutputs(document, plan, outputs, request, result, diagnostic);
			INFO(diagnostic.NodeId << ':' << diagnostic.Port << ' ' << diagnostic.Message);
			REQUIRE(status == Status::Ok);
		}
		CapturedBuilder captured;
		captured.Recipe =
			std::get<DynamicSurfaceValue>(std::get<EvaluatedValue>(result.Outputs[0].Output).Data);
		captured.Pixels = std::get<Image>(result.Outputs[1].Output);
		captured.Prior = std::move(prior);
		REQUIRE(captured.Prior.Owners.size() == 1);
		bool visible = false;
		for (size_t offset = 3; offset < captured.Pixels.Pixels.size(); offset += 4)
			visible |= captured.Pixels.Pixels[offset] != 0;
		REQUIRE(visible);
		return captured;
	}
}
TEST_CASE(
	"Nested Pixel Builder rigid recipes replay the shared frame-start journal once",
	"[imagegraph][rigid][pixel_builder]"
) {
	auto document = NestedBuilderGraph();
	const auto captured = Capture(std::move(document));
	const auto independent = Capture();
	REQUIRE(captured.Recipe.Data);
	REQUIRE(captured.Recipe.Data->RigidHistory);
	CHECK(*captured.Recipe.Data->RigidHistory == captured.Prior);
	CHECK(captured.Pixels == independent.Pixels);
	CHECK(captured.Prior == independent.Prior);
	engine::imagegraphphysics::RigidProvider replacement;
	Image output;
	Diagnostic diagnostic;
	REQUIRE(
		detail::RasterizePixelBuilder(
			captured.Recipe, {32, 32}, output, diagnostic, Limits::MaximumEvaluationBytes, &replacement
		) == Status::Ok
	);
	CHECK(output == independent.Pixels);
}
TEST_CASE(
	"Pixel Builder owns prior rigid journal and rerasterizes after provider destruction",
	"[imagegraph][rigid][pixel_builder]"
) {
	const auto captured = Capture();
	const auto &recipe = captured.Recipe;
	REQUIRE(recipe.Data);
	REQUIRE(recipe.Data->RigidHistory);
	CHECK(*recipe.Data->RigidHistory == captured.Prior);
	REQUIRE(recipe.Data->RigidHistory->Owners.size() == 1);
	CHECK(recipe.Data->RigidAuthoringRevision == 21);
	CHECK(recipe.Data->RigidPlaying);
	CHECK(recipe.Data->RigidFrameProgress);
	DynamicSurfaceValue withoutHistory = recipe;
	withoutHistory.Data->RigidHistory.reset();
	CHECK(
		detail::PixelBuilderStorageBytes(recipe, true) -
			detail::PixelBuilderStorageBytes(withoutHistory, true) ==
		RetainedRigidReplayBytes(captured.Prior)
	);
	const auto &history = recipe.Data->RigidHistory->Owners.front().History;
	REQUIRE(history.Frames.size() > 1);
	CHECK(history.Frames.front().Gravity != history.Frames.back().Gravity);
	engine::imagegraphphysics::RigidProvider replacement;
	Diagnostic diagnostic;
	Image output;
	REQUIRE(
		detail::RasterizePixelBuilder(
			recipe, {32, 32}, output, diagnostic, Limits::MaximumEvaluationBytes, &replacement
		) == Status::Ok
	);
	CHECK(output == captured.Pixels);
	DynamicSurfaceValue clone = recipe;
	clone.Data->RigidHistory->Owners[0].History.Frames[0].Gravity.X = 99;
	CHECK(clone != recipe);
	CHECK(*recipe.Data->RigidHistory == captured.Prior);
	DynamicSurfaceValue invalid = recipe;
	invalid.Data->RigidHistory->Owners.front().History.OwnerId.clear();
	CHECK_FALSE(detail::ValidPixelBuilderPayload(invalid));
	const auto reset = Capture();
	CHECK(reset.Prior == captured.Prior);
	CHECK(reset.Recipe == recipe);
	CHECK(reset.Pixels == captured.Pixels);
	const auto retained = output;
	CHECK(
		detail::RasterizePixelBuilder(recipe, {32, 32}, output, diagnostic, 1, &replacement) ==
		Status::LimitExceeded
	);
	CHECK(output == retained);
	CHECK(
		detail::RasterizePixelBuilder(recipe, {32, 32}, output, diagnostic) == Status::UnsupportedExecution
	);
	CHECK(output == retained);
	REQUIRE(
		detail::RasterizePixelBuilder(
			recipe, {32, 32}, output, diagnostic, Limits::MaximumEvaluationBytes, &replacement
		) == Status::Ok
	);
	CHECK(output == retained);
}
TEST_CASE(
	"PCX dynamic literal forwards only the active borrowed rigid provider",
	"[imagegraph][rigid][pixel_builder]"
) {
	const auto captured = Capture();
	PcxExpressionValue expression;
	Diagnostic diagnostic;
	REQUIRE(CompilePcxExpression("draw(recipes[0],0,0,1,1)", expression, diagnostic) == Status::Ok);
	ArrayValue recipes;
	recipes.ElementType = ValueType::DynamicSurface;
	recipes.Elements.emplace_back(captured.Recipe);
	expression.Data->Bindings.push_back({"recipes", std::move(recipes)});
	Image target;
	target.Width = target.Height = 32;
	target.Pixels.resize(32 * 32 * 4);
	EvaluationRequest request;
	engine::imagegraphphysics::RigidProvider replacement;
	request.RigidProvider = &replacement;
	PcxExecutionContext context{.Request = request, .Parameters = {}, .ProjectName = {}};
	context.Target = &target;
	context.MaximumBytes = Limits::MaximumEvaluationBytes;
	PcxExecutionResult result;
	REQUIRE(ExecutePcxExpression(expression, context, result, diagnostic) == Status::Ok);
	CHECK(target.Pixels == captured.Pixels.Pixels);
	request.RigidProvider = nullptr;
	const auto retained = target;
	CHECK(ExecutePcxExpression(expression, context, result, diagnostic) == Status::UnsupportedExecution);
	CHECK(target == retained);
}
TEST_CASE(
	"Rigid recipe refusal admits journal bytes before cloning or provider work",
	"[imagegraph][rigid][pixel_builder]"
) {
	const auto captured = Capture();
	const auto document = BuilderGraph();
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	struct CountingProvider final : SourceRigidProvider {
		engine::imagegraphphysics::RigidProvider Physics;
		size_t Calls = 0;
		Status Replay(
			const SourceRigidHistory &history,
			uint64_t tick,
			std::optional<SourceRigidEventPosition> position,
			uint64_t bytes,
			SourceRigidSnapshot &output,
			Diagnostic &diagnostic
		) override {
			++Calls;
			return Physics.Replay(history, tick, position, bytes, output, diagnostic);
		}
	} provider;
	StatefulOutputEvaluationResult result;
	result.Rigid = captured.Prior;
	result.Outputs.push_back({"recipe", EvaluatedValue{"dynamic_builder", captured.Recipe, {}}});
	EvaluationRequest request;
	request.Tick = 12;
	request.RigidReplay = &result.Rigid;
	request.RigidProvider = &provider;
	request.RigidPlaying = request.RigidFrameProgress = true;
	request.RigidAuthoringRevision = 21;
	const std::array<std::string, 2> outputs{"recipe", "pixels"};
	const auto bytes = RetainedRigidReplayBytes(result.Rigid);
	REQUIRE(bytes > 1);
	CHECK(
		EvaluateStatefulOutputs(document, plan, outputs, request, result, diagnostic, bytes - 1) ==
		Status::LimitExceeded
	);
	CHECK(provider.Calls == 0);
	CHECK(result.Rigid == captured.Prior);
	REQUIRE(result.Outputs.size() == 1);
	CHECK(
		std::get<DynamicSurfaceValue>(std::get<EvaluatedValue>(result.Outputs.front().Output).Data) ==
		captured.Recipe
	);
}

TEST_CASE(
	"Nested retained Pixel Builder refresh uses the immutable host frame-start journal",
	"[imagegraph][rigid][pixel_builder][feedback_checkpoint]"
) {
	const auto document = NestedBuilderGraph();
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	engine::imagegraphphysics::RigidProvider provider;
	CapturedFeedbackHost host;
	EvaluationRequest request;
	request.RigidProvider = &provider;
	request.RigidAuthoringRevision = 21;
	request.RigidFrameProgress = true;
	const auto prepare = [&](bool playing, uint64_t bytes = Limits::MaximumEvaluationBytes) {
		request.RigidPlaying = playing;
		const auto status = host.Prepare(document, plan, 21, 1, request, diagnostic, bytes, "recipe");
		INFO(diagnostic.NodeId << ':' << diagnostic.Port << ' ' << diagnostic.Message);
		return status ? Status::Ok : diagnostic.Code;
	};
	const auto recipe = [&]() {
		const auto *selected = host.Value("recipe");
		REQUIRE(selected);
		const auto *value = std::get_if<EvaluatedValue>(&selected->Output);
		REQUIRE(value);
		const auto *dynamic = std::get_if<DynamicSurfaceValue>(&value->Data);
		REQUIRE(dynamic);
		return *dynamic;
	};
	for (request.Tick = 0; request.Tick < 3; ++request.Tick)
		REQUIRE(prepare(false) == Status::Ok);
	REQUIRE(request.RigidReplay);
	const auto prefix = *request.RigidReplay;
	REQUIRE(prefix.Owners.size() == 1);
	REQUIRE(prepare(false) == Status::Ok);
	const auto paused = recipe();
	const auto pausedJournal = *request.RigidReplay;
	REQUIRE(paused.Data->RigidHistory);
	CHECK(*paused.Data->RigidHistory == prefix);
	CHECK_FALSE(paused.Data->RigidPlaying);
	REQUIRE(prepare(true) == Status::Ok);
	const auto played = recipe();
	REQUIRE(played.Data->RigidHistory);
	CHECK(*played.Data->RigidHistory == prefix);
	CHECK(played.Data->RigidPlaying);
	const auto playedJournal = *request.RigidReplay;
	EvaluationRequest independent = request;
	independent.RigidReplay = &prefix;
	StatefulOutputEvaluationResult expected;
	const std::array<std::string, 2> outputs{"recipe", "pixels"};
	REQUIRE(
		EvaluateStatefulOutputs(document, plan, outputs, independent, expected, diagnostic) == Status::Ok
	);
	CHECK(playedJournal == expected.Rigid);
	engine::imagegraphphysics::RigidProvider replacement;
	Image rerasterized;
	REQUIRE(
		detail::RasterizePixelBuilder(
			played, {32, 32}, rerasterized, diagnostic, Limits::MaximumEvaluationBytes, &replacement
		) == Status::Ok
	);
	CHECK(rerasterized == std::get<Image>(expected.Outputs[1].Output));
	bool visible = false;
	for (size_t offset = 3; offset < rerasterized.Pixels.size(); offset += 4)
		visible |= rerasterized.Pixels[offset] != 0;
	REQUIRE(visible);
	REQUIRE(prepare(false) == Status::Ok);
	CHECK(recipe() == paused);
	CHECK(*request.RigidReplay == pausedJournal);
	REQUIRE(prepare(true) == Status::Ok);
	CHECK(recipe() == played);
	CHECK(*request.RigidReplay == playedJournal);
	CHECK(prepare(false, 1) == Status::LimitExceeded);
	CHECK(recipe() == played);
	REQUIRE(prepare(true) == Status::Ok);
	CHECK(*request.RigidReplay == playedJournal);
}
