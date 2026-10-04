#include "NativeSamplerBindings.hpp"

#include "TimelineOverrides.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <sstream>
TEST_SUITE_ID("engine.imagegraph.native_sampler_bindings")
using namespace engine::imagegraph;
static_assert(sizeof(NativeSamplerBinding) == 64);
static_assert(sizeof(std::vector<NativeSamplerBinding>) == 24);
namespace {
	Document Bound() {
		Document doc;
		doc.FormatVersion = 9;
		Node node{
			"shader",
			"pc.hlsl",
			"",
			{},
			{},
			{{"argument_name_0", ValueType::Text, Value{std::string{"albedo"}}},
			 {"argument_type_0", ValueType::Enum, Value{EnumValue{7}}},
			 {"argument_value_0", ValueType::Image, std::nullopt}}
		};
		node.NativeSamplerBindings = {{"albedo", "actor/sequence"}};
		doc.Nodes.push_back(std::move(node));
		doc.Outputs = {{"out", "shader", "surface"}};
		return doc;
	}
}
TEST_CASE("Native sampler codec names remain authoritative after node reordering", "[native_sampler]") {
	auto doc = Bound();
	Diagnostic error;
	Plan plan;
	REQUIRE(Compile(doc, plan, error) == Status::Ok);
	const auto first = Write(doc);
	CHECK(first.find("native_sampler 0 \"shader\" \"albedo\" \"actor/sequence\"") != std::string::npos);
	Document restored;
	REQUIRE(Read(first, restored, error) == Status::Ok);
	CHECK(restored == doc);
	doc.Nodes.insert(doc.Nodes.begin(), Node{"extra", "pc.integer", "", {}, {{"value", int64_t{2}}}});
	const auto reordered = Write(doc);
	CHECK(reordered.find("native_sampler 1 \"shader\"") != std::string::npos);
	REQUIRE(Read(reordered, restored, error) == Status::Ok);
	CHECK(restored.Nodes[1].NativeSamplerBindings == doc.Nodes[1].NativeSamplerBindings);
	auto wrong = reordered;
	const auto location = wrong.find("native_sampler 1");
	REQUIRE(location != std::string::npos);
	wrong[location + 15] = '0';
	const auto previous = restored;
	CHECK(Read(wrong, restored, error) == Status::Malformed);
	CHECK(restored == previous);
}
TEST_CASE(
	"Native sampler validation rejects invalid authored bindings before plan publication", "[native_sampler]"
) {
	const auto valid = Bound();
	Plan plan;
	Diagnostic error;
	REQUIRE(Compile(valid, plan, error) == Status::Ok);
	for (size_t fault = 0; fault < 6; ++fault) {
		auto doc = valid;
		auto &node = doc.Nodes[0];
		if (fault == 0) doc.FormatVersion = 8;
		if (fault == 1) node.Type = "pc.integer";
		if (fault == 2) node.NativeSamplerBindings.push_back(node.NativeSamplerBindings[0]);
		if (fault == 3) node.NativeSamplerBindings[0].Argument = "9invalid";
		if (fault == 4) node.NativeSamplerBindings[0].Texture = std::string{"a\0b", 3};
		if (fault == 5) node.NativeSamplerBindings.resize(17, {"extra", "texture"});
		INFO(fault);
		const auto before = plan;
		CHECK(Compile(doc, plan, error) != Status::Ok);
		CHECK(plan == before);
		CHECK(Write(doc).empty());
	}
}
TEST_CASE(
	"Native sampler accounting charges actual retained vector and string capacity", "[native_sampler]"
) {
	auto doc = Bound();
	const auto baseline = DocumentRetainedPayloadBytes(doc);
	REQUIRE(baseline);
	doc.Nodes[0].NativeSamplerBindings.reserve(16);
	doc.Nodes[0].NativeSamplerBindings[0].Argument.reserve(128);
	doc.Nodes[0].NativeSamplerBindings[0].Texture.reserve(255);
	const auto bytes = detail::NativeSamplerBindingsPayloadBytes(doc.Nodes[0], true);
	REQUIRE(bytes);
	const auto &bindings = doc.Nodes[0].NativeSamplerBindings;
	CHECK(
		*bytes == bindings.capacity() * sizeof(NativeSamplerBinding) + bindings[0].Argument.capacity() +
					  bindings[0].Texture.capacity()
	);
	const auto retained = DocumentRetainedPayloadBytes(doc);
	REQUIRE(retained);
	CHECK(*retained > *baseline);
	auto empty = Bound();
	empty.Nodes[0].NativeSamplerBindings.clear();
	const auto emptyBytes = detail::NativeSamplerBindingsPayloadBytes(empty.Nodes[0], true);
	REQUIRE(emptyBytes);
	CHECK(*emptyBytes == empty.Nodes[0].NativeSamplerBindings.capacity() * sizeof(NativeSamplerBinding));
}
TEST_CASE(
	"Native sampler metadata survives animated replacement within its admitted clone", "[native_sampler]"
) {
	auto doc = Bound();
	doc.Keyframes = {{"shader", "mix", 0, 0.0}, {"shader", "mix", 10, 1.0}};
	detail::EvaluationBudget budget(Limits::MaximumEvaluationBytes);
	detail::TimelineOverrides replacement;
	Diagnostic error;
	EvaluationRequest request;
	request.Tick = 5;
	const std::array<uint8_t, 1> needed{1};
	REQUIRE(detail::ResolveTimelineOverrides(doc, needed, request, budget, replacement, error) == Status::Ok);
	REQUIRE(replacement.Nodes.size() == 1);
	CHECK(replacement.Nodes[0].Authored.NativeSamplerBindings == doc.Nodes[0].NativeSamplerBindings);
	detail::EvaluationBudget small(1);
	detail::TimelineOverrides refused;
	CHECK(
		detail::ResolveTimelineOverrides(doc, needed, request, small, refused, error) == Status::LimitExceeded
	);
	CHECK(refused.Nodes.empty());
}

TEST_CASE("Native sampler parsing refuses repeated names and excess rows atomically", "[native_sampler]") {
	auto doc = Bound();
	auto text = Write(doc);
	const std::string duplicate = "native_sampler 0 \"shader\" \"albedo\" \"other\"\n";
	Document previous = Bound();
	Diagnostic error;
	const auto original = previous;
	CHECK(Read(text + duplicate, previous, error) == Status::DuplicateId);
	CHECK(previous == original);
	doc.Nodes[0].NativeSamplerBindings.clear();
	for (size_t i = 0; i < 16; ++i)
		doc.Nodes[0].NativeSamplerBindings.push_back(
			{"sampler_" + std::to_string(i), "destination_" + std::to_string(i)}
		);
	REQUIRE(Read(Write(doc), previous, error) == Status::Ok);
	CHECK(previous == doc);
	const auto retained = previous;
	CHECK(
		Read(Write(doc) + "native_sampler 0 \"shader\" \"overflow\" \"other\"\n", previous, error) ==
		Status::LimitExceeded
	);
	CHECK(previous == retained);
}
