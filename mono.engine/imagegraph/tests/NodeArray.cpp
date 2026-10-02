// Pinned Array update appends one outer level when spreading and pushes complete rows otherwise.
#include "../src/NodeExecutors.hpp"
#include "../src/ValuePayload.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

TEST_SUITE_ID("engine.imagegraph.node_array")

using namespace engine::imagegraph;

namespace {
	// Measure the executor's live peak with an unrelated retained result, then exercise its admission edge.
	template <class Populate, class Verify>
	void CheckAdmission(const Node &node, Populate populate, Verify verify, bool hasScratch = false) {
		const auto *entry = FindCatalogueEntry("pc.array");
		const auto executor = detail::FindExecutor("pc.array");
		REQUIRE(entry);
		REQUIRE(executor);
		const EvaluationRequest request;
		uint64_t measuredPeak = 0;
		for (size_t attempt = 0; attempt < 3; ++attempt) {
			const uint64_t limit = attempt == 0	  ? Limits::MaximumEvaluationBytes
								   : attempt == 1 ? measuredPeak - 1
												  : measuredPeak;
			detail::EvaluationBudget budget(limit);
			auto previousCharge = budget.Reserve(512 * sizeof(double));
			REQUIRE(previousCharge);
			const std::vector<double> previous(512, 19.0);
			{
				detail::NodeContext context(node, *entry, request, budget);
				context.ByteBudget = limit;
				populate(context);
				const bool ok = executor(context);
				INFO(context.FailureMessage);
				if (attempt == 1) {
					CHECK_FALSE(ok);
					CHECK(context.FailureCode == Status::LimitExceeded);
					CHECK(context.OutputValues.empty());
					CHECK(context.OutputImageArrays.empty());
				} else {
					REQUIRE(ok);
					REQUIRE(context.FailureCode == Status::Ok);
					verify(context);
					if (attempt == 0) measuredPeak = budget.Peak();
					CHECK(budget.Peak() == measuredPeak);
					if (hasScratch) CHECK(budget.Peak() > budget.Used());
				}
				CHECK(budget.Peak() <= limit);
				CHECK(std::all_of(previous.begin(), previous.end(), [](double value) {
					return value == 19.0;
				}));
			}
			CHECK(budget.Used() == previousCharge->Bytes());
		}
	}
}

TEST_CASE(
	"Array text rows admit their complete owned leaves with prior live output",
	"[imagegraph][node_array][evaluation_budget]"
) {
	const Value first = ArrayValue{ValueType::Text, {std::string(257, 'a'), std::string(513, 'b')}};
	const Value second = ArrayValue{ValueType::Text, {std::string(129, 'c')}};
	Node node{"array", "pc.array", "", {}, {}};
	node.DynamicInputs = {{"input_0", ValueType::Array, {}}, {"input_1", ValueType::Array, {}}};
	for (const bool spread : {false, true}) {
		CheckAdmission(
			node,
			[&](detail::NodeContext &context) {
				context.Values = {{"type", EnumValue{4}}, {"spread_array", spread}};
				context.ValueViews = {{"input_0", &first}, {"input_1", &second}};
			},
			[&](const detail::NodeContext &context) {
				REQUIRE(context.OutputValues.size() == 1);
				const auto &array = std::get<ArrayValue>(context.OutputValues.front().Data);
				CHECK(array.ElementType == ValueType::Text);
				if (spread) {
					CHECK(array.Nested.empty());
					CHECK(
						array.Elements ==
						std::vector<ElementValue>{
							std::string(257, 'a'), std::string(513, 'b'), std::string(129, 'c')
						}
					);
				} else {
					CHECK(array.Elements.empty());
					CHECK(
						array.Nested ==
						std::vector<std::vector<ElementValue>>{
							std::get<ArrayValue>(first).Elements, std::get<ArrayValue>(second).Elements
						}
					);
				}
			}
		);
	}
	CHECK(std::get<ArrayValue>(first).Elements.front() == ElementValue{std::string(257, 'a')});
}

TEST_CASE(
	"Array rich leaves clone curve anchors within live admission",
	"[imagegraph][node_array][evaluation_budget]"
) {
	Curve curve;
	curve.Anchors = {{0, 0, 0, 0, 0, 0}, {1, 1, 1, 1, 1, 1}};
	const Value input = curve;
	Node node{"array", "pc.array", "", {}, {}};
	node.DynamicInputs = {{"input_0", ValueType::Curve, {}}};
	CheckAdmission(
		node,
		[&](detail::NodeContext &context) {
			context.Values = {{"type", EnumValue{0}}, {"spread_array", false}};
			context.ValueViews = {{"input_0", &input}};
		},
		[&](const detail::NodeContext &context) {
			REQUIRE(context.OutputValues.size() == 1);
			const auto &array = std::get<ArrayValue>(context.OutputValues.front().Data);
			CHECK(array.ElementType == ValueType::Curve);
			REQUIRE(array.Elements.size() == 1);
			CHECK(std::get<Curve>(array.Elements.front()) == curve);
		}
	);
}

TEST_CASE(
	"Array image clones preserve indices and admit spread scratch at peak",
	"[imagegraph][node_array][evaluation_budget]"
) {
	const Image image{1, 1, {1, 2, 3, 255}, 17};
	const ImageArray input{
		{Image{1, 1, {4, 5, 6, 255}, 18}, Image{1, 1, {7, 8, 9, 255}, 19}}, {{size_t{1}}, {size_t{0}}}
	};
	Node node{"array", "pc.array", "", {}, {}};
	node.DynamicInputs = {{"input_0", ValueType::Image, {}}, {"input_1", ValueType::Image, {}}};
	for (const bool spread : {false, true}) {
		CheckAdmission(
			node,
			[&](detail::NodeContext &context) {
				context.Values = {{"type", EnumValue{1}}, {"spread_array", spread}};
				context.Images = {{"input_0", &image}};
				context.ImageArrays = {{"input_1", &input}};
			},
			[&](const detail::NodeContext &context) {
				REQUIRE(context.OutputImageArrays.size() == 1);
				const auto &array = context.OutputImageArrays.front().second;
				REQUIRE(array.Images.size() == 3);
				CHECK(array.Images[0].Pixels == image.Pixels);
				CHECK(array.Images[1].Pixels == input.Images[0].Pixels);
				CHECK(array.Images[2].Pixels == input.Images[1].Pixels);
				CHECK(array.Images[2].Hash == 19);
				if (spread)
					CHECK(array.Items == std::vector<ImageArrayItem>{{size_t{0}}, {size_t{2}}, {size_t{1}}});
				else
					CHECK(
						array.Items ==
						std::vector<ImageArrayItem>{
							{size_t{0}}, {std::vector<ImageArrayItem>{{size_t{2}}, {size_t{1}}}}
						}
					);
			},
			spread
		);
	}
	CHECK(input.Images[0].Pixels == std::vector<uint8_t>{4, 5, 6, 255});
}
