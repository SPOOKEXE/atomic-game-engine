#include "../src/SourceInputEvaluation.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <span>
#include <utility>

TEST_SUITE_ID("engine.imagegraph.source_input_evaluation")
using namespace engine::imagegraph;

namespace {
	Document SolidDimension() {
		Document document;
		document.FormatVersion = 9;
		document.Project = ProjectSettings{};
		document.Project->SurfaceWidth = 320;
		document.Project->SurfaceHeight = 180;
		Node solid{
			"solid",
			"pc.solid",
			"",
			{},
			{{"dimension", Vector2{2, 1}},
			 {"dimension_unit", EnumValue{1}},
			 {"color", Colour{20, 40, 60, 255}},
			 {"empty", false}}
		};
		solid.SourceInputExpressions = {{"dimension", "3.25", true}};
		solid.SourceAnimatedInputs = {"dimension"};
		document.Nodes.push_back(std::move(solid));
		document.Timeline = TimelineSettings{};
		document.Timeline->Frames = 20;
		document.Outputs = {{"image", "solid", "surface_out"}};
		return document;
	}
	Document MirrorPosition() {
		Document document;
		document.FormatVersion = 9;
		Node mirror{
			"mirror", "pc.mirror_polar", "", {}, {{"position", Vector2{0.25, 0.5}}, {"rotation", 0.0}}
		};
		mirror.SourceInputExpressions = {{"position", "value", true}, {"rotation", "self.rotation", true}};
		mirror.SourceAnimatedInputs = {"position", "rotation"};
		document.Nodes.push_back(std::move(mirror));
		Node unavailable{"unavailable", "pc.ase_file_read", "", {}, {{"path", std::string{}}}};
		unavailable.SourceInternalName = "unavailable";
		document.Nodes.push_back(std::move(unavailable));
		document.Links = {{"unavailable", "output", "mirror", "surface_in"}};
		document.Keyframes = {{"mirror", "rotation", 0, double{12}, "source", KeyframeEase{}}};
		document.Keyframes.front().SourceDriver = KeyframeAudioDriver{"missing", "rms"};
		document.Tracks = {{"mirror", "rotation", "hold"}};
		document.Outputs = {{"image", "mirror", "surface_out"}};
		return document;
	}
	Document Getter() {
		Document document;
		document.FormatVersion = 9;
		Node getter{
			"get",
			"pc.matrix_get",
			"",
			{},
			{{"matrix", MatrixValue{2, 1, {13, 29}}}, {"position", Vector2{0, 0}}}
		};
		getter.SourceInputExpressions = {{"position", "value", true}};
		getter.SourceAnimatedInputs = {"position"};
		SourceSeparatedVec2Animator axes;
		axes.Port = "position";
		axes.Axes[0].Keys = {
			{"get", "position", 0, 0.0, "source", KeyframeEase{}},
			{"get", "position", 10, 1.0, "source", KeyframeEase{}}
		};
		axes.Axes[1].Keys = {{"get", "position", 0, 0.0, "source", KeyframeEase{}}};
		getter.SourceSeparatedVec2Animators.emplace().Inputs.push_back(std::move(axes));
		document.Nodes = {std::move(getter)};
		document.Tracks = {{"get", "position", "hold"}};
		document.Timeline = TimelineSettings{};
		document.Timeline->Frames = 20;
		document.Outputs = {{"cell", "get", "output"}};
		return document;
	}
	Plan Compiled(const Document &document) {
		Plan plan;
		Diagnostic diagnostic;
		const auto status = Compile(document, plan, diagnostic);
		INFO(diagnostic.NodeId << ':' << diagnostic.Port << ':' << diagnostic.Message);
		REQUIRE(status == Status::Ok);
		return plan;
	}
	Status ReadInput(
		const Document &document,
		const Plan &plan,
		std::string_view node,
		std::string_view port,
		uint64_t tick,
		detail::EvaluationBudget &budget,
		Value &result,
		detail::AllocationReservation &charge,
		Diagnostic &diagnostic,
		std::optional<std::span<const AuthoredValue>> observed = std::nullopt
	) {
		EvaluationRequest request;
		request.Tick = tick;
		return detail::EvaluateSourceInput(
			document, plan, node, port, request, budget, result, charge, diagnostic, observed
		);
	}
	Status ReadPosition(
		const Document &document,
		const Plan &plan,
		uint64_t tick,
		detail::EvaluationBudget &budget,
		Value &result,
		detail::AllocationReservation &charge,
		Diagnostic &diagnostic,
		std::optional<std::span<const AuthoredValue>> observed = std::nullopt
	) {
		return ReadInput(
			document, plan, "get", "position", tick, budget, result, charge, diagnostic, observed
		);
	}
}

TEST_CASE("Source input evaluation rounds selected IVec2 axes after PCX", "[source_input]") {
	const auto document = Getter();
	const auto plan = Compiled(document);
	detail::EvaluationBudget budget(Limits::MaximumEvaluationBytes);
	Value result;
	detail::AllocationReservation charge;
	Diagnostic diagnostic;
	REQUIRE(ReadPosition(document, plan, 7, budget, result, charge, diagnostic) == Status::Ok);
	CHECK(result == Value{Vector2{1, 0}});
	CHECK(charge.Bytes() > 0);
	CHECK(budget.Used() == charge.Bytes());

	auto transformed = document;
	transformed.Nodes.front().SourceInputExpressions.front().Code = "value * 2";
	const auto transformedPlan = Compiled(transformed);
	detail::EvaluationBudget transformedBudget(Limits::MaximumEvaluationBytes);
	Value transformedValue;
	detail::AllocationReservation transformedCharge;
	REQUIRE(
		ReadPosition(
			transformed,
			transformedPlan,
			7,
			transformedBudget,
			transformedValue,
			transformedCharge,
			diagnostic
		) == Status::Ok
	);
	CHECK(transformedValue == Value{Vector2{1, 0}});
}

TEST_CASE(
	"Source input evaluation leaves same-node unrelated controls and linked producers asleep",
	"[source_input][source_input_selection]"
) {
	const auto document = MirrorPosition();
	const auto plan = Compiled(document);
	detail::EvaluationBudget budget(Limits::MaximumEvaluationBytes);
	Value result;
	detail::AllocationReservation charge;
	Diagnostic diagnostic;
	REQUIRE(
		ReadInput(document, plan, "mirror", "position", 0, budget, result, charge, diagnostic) == Status::Ok
	);
	CHECK(result == Value{Vector2{0.25, 0.5}});
	CHECK(budget.Used() == charge.Bytes());
}

TEST_CASE("Source input Dimension getter duplicates scalar and keeps project units raw", "[source_input]") {
	const auto document = SolidDimension();
	const auto plan = Compiled(document);
	detail::EvaluationBudget budget(Limits::MaximumEvaluationBytes);
	Value result;
	detail::AllocationReservation charge;
	Diagnostic diagnostic;
	REQUIRE(
		ReadInput(document, plan, "solid", "dimension", 0, budget, result, charge, diagnostic) == Status::Ok
	);
	CHECK(result == Value{Vector2{3.25, 3.25}});
	auto arrayDocument = document;
	arrayDocument.Nodes.front().SourceInputExpressions.front().Code = "self.dimension";
	const auto arrayPlan = Compiled(arrayDocument);
	const std::array cases{
		std::pair{ArrayValue{ValueType::Scalar, {2.0}}, Vector2{2, 0}},
		std::pair{ArrayValue{ValueType::Scalar, {2.0, 3.0, 4.0}}, Vector2{2, 3}}
	};
	for (const auto &[input, expected] : cases) {
		const std::array<AuthoredValue, 1> observed{{{"dimension", input}}};
		detail::EvaluationBudget arrayBudget(Limits::MaximumEvaluationBytes);
		Value arrayResult;
		detail::AllocationReservation arrayCharge;
		REQUIRE(
			ReadInput(
				arrayDocument,
				arrayPlan,
				"solid",
				"dimension",
				0,
				arrayBudget,
				arrayResult,
				arrayCharge,
				diagnostic,
				std::span{observed}
			) == Status::Ok
		);
		CHECK(arrayResult == Value{expected});
	}
}

TEST_CASE(
	"Source input Vec2 rows pad short rows within an atomic exact budget", "[source_input][evaluation_budget]"
) {
	auto document = MirrorPosition();
	document.Nodes.front().SourceInputExpressions.front().Code = "self.position";
	const auto plan = Compiled(document);
	ArrayValue shortRows;
	shortRows.ElementType = ValueType::Scalar;
	shortRows.Nested = {{double{1.25}}};
	const std::array<AuthoredValue, 1> observed{{{"position", shortRows}}};
	uint64_t peak = 0;
	{
		detail::EvaluationBudget budget(Limits::MaximumEvaluationBytes);
		Value result = Vector2{9, 8};
		auto prior = budget.Reserve(sizeof(Vector2));
		REQUIRE(prior);
		detail::AllocationReservation charge = std::move(*prior);
		Diagnostic diagnostic;
		REQUIRE(
			ReadInput(
				document,
				plan,
				"mirror",
				"position",
				0,
				budget,
				result,
				charge,
				diagnostic,
				std::span{observed}
			) == Status::Ok
		);
		const auto *rows = std::get_if<ArrayValue>(&result);
		REQUIRE(rows);
		REQUIRE(rows->Nested.size() == 1);
		REQUIRE(rows->Nested.front().size() == 2);
		CHECK(std::get<double>(rows->Nested.front()[0]) == 1.25);
		CHECK(std::get<double>(rows->Nested.front()[1]) == 0.0);
		peak = budget.Peak();
	}
	REQUIRE(peak > sizeof(Vector2));
	{
		detail::EvaluationBudget budget(peak - 1);
		Value result = Vector2{9, 8};
		auto prior = budget.Reserve(sizeof(Vector2));
		REQUIRE(prior);
		detail::AllocationReservation charge = std::move(*prior);
		Diagnostic diagnostic;
		CHECK(
			ReadInput(
				document,
				plan,
				"mirror",
				"position",
				0,
				budget,
				result,
				charge,
				diagnostic,
				std::span{observed}
			) == Status::LimitExceeded
		);
		CHECK(result == Value{Vector2{9, 8}});
		CHECK(charge.Bytes() == sizeof(Vector2));
		CHECK(budget.Used() == sizeof(Vector2));
	}
	{
		detail::EvaluationBudget budget(peak);
		Value result = Vector2{9, 8};
		auto prior = budget.Reserve(sizeof(Vector2));
		REQUIRE(prior);
		detail::AllocationReservation charge = std::move(*prior);
		Diagnostic diagnostic;
		REQUIRE(
			ReadInput(
				document,
				plan,
				"mirror",
				"position",
				0,
				budget,
				result,
				charge,
				diagnostic,
				std::span{observed}
			) == Status::Ok
		);
		CHECK(budget.Peak() == peak);
	}
}

TEST_CASE("Source input Vec2 rejects image leaves and SurfaceValue atomically", "[source_input]") {
	auto document = MirrorPosition();
	document.Nodes.front().SourceInputExpressions.front().Code = "self.position";
	const auto plan = Compiled(document);
	const Image image{1, 1, {10, 20, 30, 255}};
	ArrayValue imageRows;
	imageRows.ElementType = ValueType::Any;
	imageRows.Items.push_back({image});
	const std::array<AuthoredValue, 1> imageArray{{{"position", imageRows}}};
	const std::array<AuthoredValue, 1> surfaceLeaf{{{"position", SurfaceValue{image}}}};
	detail::EvaluationBudget budget(Limits::MaximumEvaluationBytes);
	Value result = Vector2{9, 8};
	auto prior = budget.Reserve(sizeof(Vector2));
	REQUIRE(prior);
	detail::AllocationReservation charge = std::move(*prior);
	const uint64_t retained = budget.Used();
	Diagnostic diagnostic;
	CHECK(
		ReadInput(
			document, plan, "mirror", "position", 0, budget, result, charge, diagnostic, std::span{imageArray}
		) == Status::UnsupportedExecution
	);
	CHECK(result == Value{Vector2{9, 8}});
	CHECK(charge.Bytes() == sizeof(Vector2));
	CHECK(budget.Used() == retained);
	CHECK(
		ReadInput(
			document,
			plan,
			"mirror",
			"position",
			0,
			budget,
			result,
			charge,
			diagnostic,
			std::span{surfaceLeaf}
		) == Status::UnsupportedExecution
	);
	CHECK(result == Value{Vector2{9, 8}});
	CHECK(budget.Used() == retained);
}

TEST_CASE("Source input PCX observations are required and published atomically", "[source_input][pcx]") {
	auto document = Getter();
	document.Nodes.front().SourceInputExpressions = {{"position", "self.position", true}};
	const auto plan = Compiled(document);
	detail::EvaluationBudget budget(Limits::MaximumEvaluationBytes);
	Value result = Vector2{9, 8};
	auto oldCharge = budget.Reserve(sizeof(Vector2));
	REQUIRE(oldCharge);
	detail::AllocationReservation charge = std::move(*oldCharge);
	const uint64_t oldUsed = budget.Used();
	Diagnostic diagnostic;
	CHECK(
		ReadPosition(document, plan, 7, budget, result, charge, diagnostic) == Status::UnsupportedExecution
	);
	CHECK(result == Value{Vector2{9, 8}});
	CHECK(charge.Bytes() == sizeof(Vector2));
	CHECK(budget.Used() == oldUsed);

	const std::array<AuthoredValue, 1> observed{{{"position", Vector2{0.7, 0}}}};
	CHECK(
		ReadPosition(document, plan, 7, budget, result, charge, diagnostic, std::span{observed}) == Status::Ok
	);
	CHECK(result == Value{Vector2{1, 0}});
	CHECK(budget.Used() == charge.Bytes());

	const std::array<AuthoredValue, 2> duplicate{
		{{"position", Vector2{0.7, 0}}, {"position", Vector2{0.2, 0}}}
	};
	const uint64_t retained = budget.Used();
	CHECK(
		ReadPosition(document, plan, 7, budget, result, charge, diagnostic, std::span{duplicate}) ==
		Status::InvalidValue
	);
	CHECK(result == Value{Vector2{1, 0}});
	CHECK(budget.Used() == retained);
}

TEST_CASE(
	"Source input evaluation prefers linked producers and wakes named PCX outputs",
	"[source_input][source_input_selection][pcx]"
) {
	auto linked = Getter();
	linked.Nodes.push_back({"position", "pc.vector2", "", {}, {{"x", 0.7}, {"y", 0.0}}});
	linked.Links = {{"position", "vector", "get", "position"}};
	const auto linkedPlan = Compiled(linked);
	detail::EvaluationBudget linkedBudget(Limits::MaximumEvaluationBytes);
	Value linkedValue;
	detail::AllocationReservation linkedCharge;
	Diagnostic diagnostic;
	REQUIRE(
		ReadPosition(linked, linkedPlan, 10, linkedBudget, linkedValue, linkedCharge, diagnostic) ==
		Status::Ok
	);
	CHECK(linkedValue == Value{Vector2{1, 0}});

	auto named = Getter();
	named.Nodes.front().SourceInputExpressions = {{"position", "producer.outputs.vector", true}};
	named.Nodes.push_back({"producer", "pc.vector2", "", {}, {{"x", 0.7}, {"y", 0.0}}});
	named.Nodes.back().SourceInternalName = "producer";
	const auto namedPlan = Compiled(named);
	detail::EvaluationBudget namedBudget(Limits::MaximumEvaluationBytes);
	Value namedValue;
	detail::AllocationReservation namedCharge;
	REQUIRE(
		ReadPosition(named, namedPlan, 10, namedBudget, namedValue, namedCharge, diagnostic) == Status::Ok
	);
	CHECK(namedValue == Value{Vector2{1, 0}});
}

TEST_CASE(
	"Source input evaluation admits exact peak and refuses one byte below atomically",
	"[source_input][evaluation_budget]"
) {
	const auto document = Getter();
	const auto plan = Compiled(document);
	uint64_t peak = 0;
	{
		detail::EvaluationBudget budget(Limits::MaximumEvaluationBytes);
		Value result = double{-5};
		auto prior = budget.Reserve(sizeof(double));
		REQUIRE(prior);
		detail::AllocationReservation charge = std::move(*prior);
		Diagnostic diagnostic;
		REQUIRE(ReadPosition(document, plan, 7, budget, result, charge, diagnostic) == Status::Ok);
		peak = budget.Peak();
	}
	REQUIRE(peak > sizeof(double));
	{
		detail::EvaluationBudget budget(peak - 1);
		Value result = double{-5};
		auto prior = budget.Reserve(sizeof(double));
		REQUIRE(prior);
		detail::AllocationReservation charge = std::move(*prior);
		Diagnostic diagnostic;
		CHECK(ReadPosition(document, plan, 7, budget, result, charge, diagnostic) == Status::LimitExceeded);
		CHECK(result == Value{double{-5}});
		CHECK(charge.Bytes() == sizeof(double));
		CHECK(budget.Used() == sizeof(double));
	}
	{
		detail::EvaluationBudget budget(peak);
		Value result = double{-5};
		auto prior = budget.Reserve(sizeof(double));
		REQUIRE(prior);
		detail::AllocationReservation charge = std::move(*prior);
		Diagnostic diagnostic;
		REQUIRE(ReadPosition(document, plan, 7, budget, result, charge, diagnostic) == Status::Ok);
		CHECK(result == Value{Vector2{1, 0}});
		CHECK(budget.Peak() == peak);
	}
}
