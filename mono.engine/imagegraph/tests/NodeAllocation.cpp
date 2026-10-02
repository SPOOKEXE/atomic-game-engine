#include "../src/NodeExecutors.hpp"
#include "../src/ValueText.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.imagegraph.node_allocation")

using namespace engine::imagegraph;

TEST_CASE(
	"typed publication admits a borrowed clone while previous output remains live",
	"[imagegraph][evaluation_budget]"
) {
	const auto *entry = FindCatalogueEntry("pc.number");
	REQUIRE(entry);
	const uint64_t storage =
		entry->Outputs.size() * (sizeof(std::pair<std::string, Image>) + sizeof(AuthoredValue) +
								 (sizeof(std::pair<std::string, ImageArray>) +
								  sizeof(std::pair<std::string_view, SourceSocketDomain>)));
	const Value borrowed = std::string(512, 'x');
	const Node node{"publish", "pc.number", "", {}, {}};
	const EvaluationRequest request;
	const uint64_t exactBytes =
		32 + storage + 512 + std::max(std::string_view("number").size(), std::string{}.capacity());
	for (const uint64_t limit : {exactBytes - 1, exactBytes}) {
		detail::EvaluationBudget budget(limit);
		auto previousCharge = budget.Reserve(32);
		REQUIRE(previousCharge);
		std::vector<double> previous{1, 2, 3, 4};
		{
			detail::NodeContext context(node, *entry, request, budget);
			context.ByteBudget = limit;
			context.SetValue("number", borrowed);
			if (limit < exactBytes) {
				CHECK(context.FailureCode == Status::LimitExceeded);
				CHECK(context.OutputValues.empty());
				CHECK(budget.Used() == 32 + storage);
			} else {
				REQUIRE(context.FailureCode == Status::Ok);
				REQUIRE(context.OutputValues.size() == 1);
				CHECK(context.OutputValues.front().Data == borrowed);
				CHECK(budget.Used() == exactBytes);
				CHECK_FALSE(context.ReserveWorkspace(1, "scratch"));
			}
			CHECK(previous == std::vector<double>{1, 2, 3, 4});
			CHECK(std::get<std::string>(borrowed) == std::string(512, 'x'));
		}
		CHECK(budget.Used() == 32);
	}
}

TEST_CASE(
	"default decoding admits parser scratch with retained data before clone",
	"[imagegraph][evaluation_budget]"
) {
	constexpr std::string_view text = "a text 2 s \"left\" s \"right\"";
	const uint64_t workspace = 5 * text.size() + 4 * std::string{}.capacity();
	const uint64_t payload = 2 * sizeof(ElementValue) + 2 * std::string{}.capacity();
	for (const uint64_t limit : {32 + workspace + payload - 1, 32 + workspace + payload}) {
		detail::EvaluationBudget budget(limit);
		auto previousCharge = budget.Reserve(32);
		REQUIRE(previousCharge);
		std::vector<double> previous{1, 2, 3, 4};
		auto outputCharge = budget.Reserve(0);
		REQUIRE(outputCharge);
		Value output = 7.0;
		const Status status = detail::ReadValueText(text, output, budget, *outputCharge);
		if (limit < 32 + workspace + payload) {
			CHECK(status == Status::LimitExceeded);
			CHECK(output == Value{7.0});
			CHECK(outputCharge->Bytes() == 0);
			CHECK(budget.Used() == 32);
		} else {
			REQUIRE(status == Status::Ok);
			CHECK(output == Value{ArrayValue{ValueType::Text, {std::string{"left"}, std::string{"right"}}}});
			CHECK(outputCharge->Bytes() == payload);
			CHECK(budget.Used() == 32 + payload);
		}
		CHECK(previous == std::vector<double>{1, 2, 3, 4});
	}
}

TEST_CASE(
	"moving spare-capacity typed storage retains its complete admission", "[imagegraph][evaluation_budget]"
) {
	const auto *entry = FindCatalogueEntry("pc.number");
	REQUIRE(entry);
	detail::EvaluationBudget budget(Limits::MaximumEvaluationBytes);
	const Node node{"capacity", "pc.number", "", {}, {}};
	const EvaluationRequest request;
	detail::NodeContext context(node, *entry, request, budget);
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	ArrayValue array{ValueType::Scalar, {}};
	array.Elements.reserve(100);
	array.Elements.emplace_back(3.0);
	const uint64_t retained = detail::RetainedPayloadBytes(array);
	REQUIRE(retained > detail::PayloadOwnedBytes(array));
	REQUIRE(context.ReserveOutput(retained, "number"));
	context.SetValue("number", std::move(array));
	REQUIRE(context.FailureCode == Status::Ok);
	REQUIRE(context.OutputValues.size() == 1);
	CHECK(detail::RetainedPayloadBytes(context.OutputValues.front().Data) == retained);
	CHECK(context.OutputCharge.Bytes() == retained + std::string{}.capacity());
}

TEST_CASE("compile route admission ignores unrelated long names", "[imagegraph][evaluation_budget]") {
	Document document;
	document.FormatVersion = 9;
	document.Nodes = {{"source", "pc.number_simple", "", {}, {{"value", 5.0}}}};
	document.Junctions = {
		{"route", "", ValueType::Scalar, std::nullopt},
		{"default", "", ValueType::Scalar, Value{3.0}},
		{std::string(131072, 'z'), "", ValueType::Scalar, Value{0.0}}
	};
	document.Links = {{"source", "number", "route", "value"}};
	constexpr size_t routes = 129;
	for (size_t index = 0; index < routes; ++index) {
		const std::string id = "target" + std::to_string(index);
		document.Nodes.push_back({id, "pc.math", "", {}, {{"a", 0.0}, {"b", 1.0}}});
		document.Links.push_back({"route", "value", id, "a"});
	}
	document.Links.push_back({"default", "value", "target0", "b"});
	document.Outputs = {{"answer", "target0", "result"}};
	// Charging each short route against an unrelated 128KiB name would exceed the entire budget.
	REQUIRE(routes * 8 * document.Junctions.back().Id.size() > Limits::MaximumEvaluationBytes);
	Diagnostic diagnostic;
	Plan plan;
	const Status compiled = Compile(document, plan, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(compiled == Status::Ok);
	REQUIRE(plan.EffectiveLinks.size() == routes);
	for (size_t index = 0; index < routes; ++index) {
		CHECK(plan.EffectiveLinks[index] == Link{"source", "number", "target" + std::to_string(index), "a"});
	}
	REQUIRE(plan.ResolvedInputs.size() == 1);
	CHECK(plan.ResolvedInputs.front().NodeId == "target0");
	CHECK(plan.ResolvedInputs.front().Port == "b");
	CHECK(plan.ResolvedInputs.front().Data == Value{3.0});
	document.Links.front().FromNode = "route";
	document.Links.front().FromPort = "value";
	CHECK(Compile(document, plan, diagnostic) == Status::Cycle);
}

TEST_CASE("default text decoding retains only admitted sized capacity", "[imagegraph][evaluation_budget]") {
	constexpr std::string_view encoded = "s \"0123456789abcdef\"";
	constexpr std::string_view decoded = "0123456789abcdef";
	const uint64_t workspace = 5 * encoded.size() + 4 * std::string{}.capacity();
	const uint64_t payload = std::max(decoded.size(), std::string{}.capacity());
	for (const uint64_t limit : {32 + workspace + payload - 1, 32 + workspace + payload}) {
		detail::EvaluationBudget budget(limit);
		auto previousCharge = budget.Reserve(32);
		REQUIRE(previousCharge);
		std::vector<double> previous{1, 2, 3, 4};
		{
			auto outputCharge = budget.Reserve(0);
			REQUIRE(outputCharge);
			Value output = 7.0;
			const Status status = detail::ReadValueText(encoded, output, budget, *outputCharge);
			if (limit < 32 + workspace + payload) {
				CHECK(status == Status::LimitExceeded);
				CHECK(output == Value{7.0});
				CHECK(outputCharge->Bytes() == 0);
				CHECK(budget.Used() == 32);
			} else {
				REQUIRE(status == Status::Ok);
				const auto &text = std::get<std::string>(output);
				CHECK(text == decoded);
				CHECK(text.capacity() == payload);
				CHECK(outputCharge->Bytes() == text.capacity());
				CHECK(budget.Used() == 32 + text.capacity());
				CHECK(budget.Peak() == limit);
			}
		}
		CHECK(budget.Used() == 32);
		CHECK(previous == std::vector<double>{1, 2, 3, 4});
	}
}
