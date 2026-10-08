#include <engine/imagegraph/SourceArgumentHost.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>

TEST_SUITE_ID("engine.imagegraph.source_argument_tag")
using namespace engine::imagegraph;
namespace {
	Document Graph(std::string tag = "missing") {
		Document document;
		document.FormatVersion = 9;
		document.Nodes = {
			{"argument",
			 "pc.argument",
			 "",
			 {},
			 {{"tag", std::move(tag)}, {"type", EnumValue{0}}, {"default_value", std::string{"fallback"}}}}
		};
		document.Outputs = {{"out", "argument", "value"}};
		return document;
	}
	Document NumberTag(double number) {
		auto document = Graph();
		document.Nodes.push_back({"number", "pc.number", "", {}, {{"value", number}}});
		document.Links.push_back({"number", "number", "argument", "tag"});
		return document;
	}
	Document RawTag(Value value) {
		auto document = Graph();
		document.Junctions = {{"raw", "", ValueType::Any, std::move(value)}};
		document.Links = {{"raw", "value", "argument", "tag"}};
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
	SourceArgumentHost Provider(std::vector<AuthoredValue> arguments) {
		SourceArgumentHost host;
		Diagnostic diagnostic;
		REQUIRE(host.Prepare(arguments, Limits::MaximumEvaluationBytes, diagnostic) == Status::Ok);
		return host;
	}
	Value Evaluated(const Document &document, SourceArgumentHost &host) {
		Document restored;
		Diagnostic diagnostic;
		REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
		CHECK(restored == document);
		const auto plan = Compiled(restored);
		EvaluationRequest request;
		request.HostProvider = &host;
		EvaluatedValue output;
		const auto status = EvaluateValue(restored, plan, "out", request, output, diagnostic);
		INFO(diagnostic.NodeId << ':' << diagnostic.Port << ':' << diagnostic.Message);
		REQUIRE(status == Status::Ok);
		return output.Data;
	}
}

TEST_CASE(
	"Argument linked Number uses HTML5 struct key spelling after native reload",
	"[source_argument][argument_tag]"
) {
	// yyGetString uses signed-int32 equality before choosing toString or toFixed(2).
	const std::pair<double, std::string_view> fixtures[]{
		{73, "73"},
		{.25, "0.25"},
		{2147483648., "2147483648.00"},
		{.125, "0.13"},
		{1.005, "1.00"},
		{-0., "0"},
		{-1e-9, "-0.00"},
		{1e21, "1e+21"},
		{1e23, "1e+23"},
		{-1e21, "-1e+21"},
		{1e308, "1e+308"}
	};
	for (const auto &[number, key] : fixtures) {
		CAPTURE(number, key);
		auto host = Provider({{std::string(key), int64_t{73}}, {"missing", int64_t{-1}}});
		CHECK(Evaluated(NumberTag(number), host) == Value{int64_t{73}});
	}
}

TEST_CASE(
	"Argument raw Boolean keys and literal strings preserve source lookup precedence",
	"[source_argument][argument_tag]"
) {
	auto host = Provider(
		{{"1", int64_t{11}},
		 {"0", int64_t{22}},
		 {"a_b", int64_t{33}},
		 {"a b", int64_t{44}},
		 {"x y", int64_t{55}},
		 {"0.250", int64_t{66}},
		 {"", int64_t{77}}}
	);
	CHECK(Evaluated(RawTag(true), host) == Value{int64_t{11}});
	CHECK(Evaluated(RawTag(false), host) == Value{int64_t{22}});
	CHECK(Evaluated(Graph("a_b"), host) == Value{int64_t{33}});
	CHECK(Evaluated(Graph("x_y"), host) == Value{int64_t{55}});
	CHECK(Evaluated(Graph("0.250"), host) == Value{int64_t{66}});
	CHECK(Evaluated(Graph(""), host) == Value{int64_t{77}});
	CHECK(Evaluated(Graph("absent"), host) == Value{std::string{"fallback"}});
	host = Provider({{"a_b", UndefinedValue{}}, {"a b", int64_t{44}}});
	CHECK(Evaluated(Graph("a_b"), host) == Value{int64_t{44}});
}

TEST_CASE(
	"Argument unsupported tuple and map tags preserve the caller output", "[source_argument][argument_tag]"
) {
	auto host = Provider({{"missing", int64_t{73}}});
	for (const Value &tag : {Value{Vector2{1, 2}}, Value{StructValue{}}}) {
		CAPTURE(tag.index());
		auto document = Graph();
		document.Nodes.push_back({"tag_source", "pc.argument", "", {}, {{"tag", std::string{"raw_value"}}}});
		document.Links = {{"tag_source", "value", "argument", "tag"}};
		host = Provider({{"missing", int64_t{73}}, {"raw_value", tag}});
		const auto plan = Compiled(document);
		EvaluationRequest request;
		request.HostProvider = &host;
		EvaluatedValue output;
		output.Data = std::string{"retained"};
		const auto previous = output;
		Diagnostic diagnostic;
		CHECK(
			EvaluateValue(document, plan, "out", request, output, diagnostic) == Status::UnsupportedExecution
		);
		CHECK(output == previous);
		CHECK(diagnostic.NodeId == "argument");
		CHECK(Evaluated(Graph("missing"), host) == Value{int64_t{73}});
	}
}

TEST_CASE(
	"Argument tag publication and oversized table keys refuse atomically", "[source_argument][argument_tag]"
) {
	auto host = Provider({{"73", int64_t{91}}});
	const auto document = NumberTag(73);
	const auto plan = Compiled(document);
	EvaluationRequest request;
	HostNodeCapture capture;
	Diagnostic diagnostic;
	REQUIRE(PrepareHostCapture(document, plan, "argument", request, capture, diagnostic) == Status::Ok);
	HostNodeInvocation invocation{
		capture.Authored, request, capture.Inputs, {}, Limits::MaximumEvaluationBytes
	};
	HostNodeCapture output;
	std::string failure;
	REQUIRE(host.Capture(invocation, output, failure));
	const auto previous = output;
	invocation.MaximumOperationBytes = 1;
	CHECK_FALSE(host.Capture(invocation, output, failure));
	CHECK_FALSE(failure.empty());
	CHECK(output.Authored == previous.Authored);
	CHECK(output.Inputs == previous.Inputs);
	CHECK(output.Outputs == previous.Outputs);
	CHECK(output.Tick == previous.Tick);
	CHECK(output.Subframe == previous.Subframe);
	CHECK(output.State == previous.State);
	CHECK(output.Failure == previous.Failure);

	const auto retained = host.RetainedBytes();
	const std::array<AuthoredValue, 1> replacement{
		{{std::string(Limits::MaximumTextBytes + 1, 'x'), int64_t{2}}}
	};
	CHECK(host.Prepare(replacement, Limits::MaximumEvaluationBytes, diagnostic) == Status::InvalidValue);
	CHECK(host.RetainedBytes() == retained);
	CHECK(Evaluated(document, host) == Value{int64_t{91}});
	const std::array<AuthoredValue, 1> small{{{"73", int64_t{2}}}};
	CHECK(host.Prepare(small, 1, diagnostic) == Status::LimitExceeded);
	CHECK(Evaluated(document, host) == Value{int64_t{91}});
}
