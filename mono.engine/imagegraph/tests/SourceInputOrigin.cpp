#include "SourceInputOrigin.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
TEST_SUITE_ID("engine.imagegraph.source_input_origin")
using namespace engine::imagegraph;
namespace {
	Document Fixture() {
		Document document;
		document.FormatVersion = 9;
		document.Nodes = {{"merge", "pc.string_merge", "", {}, {}}};
		document.Nodes.front().DynamicInputs = {
			{"text_0", ValueType::Text, std::string("a"), {}, "pxc:input:0"},
			{"text_1", ValueType::Text, std::string("b"), {}, "pxc:input:1"}
		};
		document.Outputs = {{"out", "merge", "text"}};
		return document;
	}
}
TEST_CASE(
	"Source input origins survive native document IO independently of socket positions",
	"[imagegraph][source_input_origin]"
) {
	auto document = Fixture();
	Plan plan;
	Diagnostic diagnostic;
	const auto compiled = Compile(document, plan, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(compiled == Status::Ok);
	Document restored;
	REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
	CHECK(restored == document);
	std::swap(
		document.Nodes[0].DynamicInputs[0].SourceInputId, document.Nodes[0].DynamicInputs[1].SourceInputId
	);
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
	CHECK(restored == document);
	document.Nodes[0].DynamicInputs[0].SourceInputId = "pxc:input:18446744073709551615";
	const auto withOrigin = NodeClonePayloadBytes(document.Nodes[0]);
	REQUIRE(withOrigin);
	for (auto &input : document.Nodes[0].DynamicInputs)
		std::string{}.swap(input.SourceInputId);
	const auto withoutOrigin = NodeClonePayloadBytes(document.Nodes[0]);
	REQUIRE(withoutOrigin);
	CHECK(*withOrigin > *withoutOrigin);
}
TEST_CASE(
	"Source input origin malformed duplicate and missing-socket records refuse atomically",
	"[imagegraph][source_input_origin]"
) {
	const auto original = Fixture();
	Diagnostic diagnostic;
	Plan plan;
	for (const std::string &invalid :
		 {std::string("pxc:input:"),
		  std::string("pxc:input:01"),
		  std::string("pxc:input:-1"),
		  std::string(65, 'x')}) {
		auto document = original;
		document.Nodes[0].DynamicInputs[0].SourceInputId = invalid;
		CHECK(Compile(document, plan, diagnostic) == Status::InvalidValue);
		CHECK(Write(document).empty());
	}
	auto document = original;
	document.Nodes[0].DynamicInputs[1].SourceInputId = "pxc:input:0";
	CHECK(Compile(document, plan, diagnostic) == Status::DuplicateId);
	Document restored = original;
	const auto before = restored;
	CHECK(Read(Write(document), restored, diagnostic) == Status::Malformed);
	CHECK(restored == before);
	const auto text = Write(original);
	CHECK(
		Read(text + "source_input_origin 0 \"merge\" \"missing\" \"pxc:input:2\"\n", restored, diagnostic) ==
		Status::Malformed
	);
	CHECK(restored == before);
	document = original;
	document.FormatVersion = 8;
	CHECK(Compile(document, plan, diagnostic) == Status::InvalidValue);
	CHECK(Write(document).empty());
}
TEST_CASE(
	"Source origin validation sorts bounded ordinal scratch and reports exact failure domains",
	"[imagegraph][source_input_origin]"
) {
	auto inputs = Fixture().Nodes.front().DynamicInputs;
	std::array<uint64_t, 2> scratch{};
	std::string_view port;
	CHECK(detail::ValidateSourceInputOrigins(inputs, scratch, port) == Status::Ok);
	CHECK(port.empty());
	inputs[0].SourceInputId = "pxc:input:18446744073709551615";
	inputs[1].SourceInputId = "pxc:input:0";
	CHECK(detail::ValidateSourceInputOrigins(inputs, scratch, port) == Status::Ok);
	CHECK(scratch[0] == 0);
	CHECK(scratch[1] == UINT64_MAX);
	CHECK(
		detail::ValidateSourceInputOrigins(inputs, std::span(scratch).first(1), port) == Status::LimitExceeded
	);
	CHECK(port == "text_1");
	inputs[1].SourceInputId = inputs[0].SourceInputId;
	CHECK(detail::ValidateSourceInputOrigins(inputs, scratch, port) == Status::DuplicateId);
	CHECK(port == "text_1");
	inputs[1].SourceInputId = "pxc:input:01";
	CHECK(detail::ValidateSourceInputOrigins(inputs, scratch, port) == Status::InvalidValue);
	inputs[1].SourceInputId = std::string(65, 'x');
	CHECK(detail::ValidateSourceInputOrigins(inputs, scratch, port) == Status::LimitExceeded);
	for (auto &input : inputs)
		input.SourceInputId.clear();
	CHECK(detail::ValidateSourceInputOrigins(inputs, std::span<uint64_t>{}, port) == Status::Ok);
}
