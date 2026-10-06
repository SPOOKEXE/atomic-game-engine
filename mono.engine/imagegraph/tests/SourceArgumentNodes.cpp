
#include <engine/imagegraph/Catalogue.hpp>
#include <engine/imagegraph/SourceArgumentHost.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <limits>

TEST_SUITE_ID("engine.imagegraph.source_argument")
using namespace engine::imagegraph;
namespace {
	Document Graph(int64_t mode = 0, Value fallback = std::string{"fallback"}, std::string tag = "name") {
		Document d;
		d.FormatVersion = 9;
		d.Nodes = {
			{"argument",
			 "pc.argument",
			 "",
			 {},
			 {{"tag", std::move(tag)}, {"type", EnumValue{mode}}, {"default_value", std::move(fallback)}}}
		};
		d.Outputs = {{"out", "argument", "value"}};
		return d;
	}
	Plan Compiled(const Document &d) {
		Plan p;
		Diagnostic diagnostic;
		const auto status = Compile(d, p, diagnostic);
		INFO(diagnostic.NodeId << ':' << diagnostic.Port << ':' << diagnostic.Message);
		REQUIRE(status == Status::Ok);
		return p;
	}
	Document Restored(const Document &d) {
		Document restored;
		Diagnostic diagnostic;
		REQUIRE(Read(Write(d), restored, diagnostic) == Status::Ok);
		REQUIRE(restored == d);
		return restored;
	}
	SourceArgumentHost Provider(std::vector<AuthoredValue> arguments) {
		SourceArgumentHost host;
		Diagnostic diagnostic;
		REQUIRE(host.Prepare(arguments, Limits::MaximumEvaluationBytes, diagnostic) == Status::Ok);
		return host;
	}
	Value Evaluated(Document d, HostNodeProvider *host, std::span<const HostNodeCapture> captures = {}) {
		d = Restored(d);
		auto p = Compiled(d);
		EvaluationRequest request;
		request.HostProvider = host;
		request.HostCaptures = captures;
		EvaluatedValue output;
		Diagnostic diagnostic;
		const auto status = EvaluateValue(d, p, "out", request, output, diagnostic);
		INFO(diagnostic.NodeId << ':' << diagnostic.Port << ':' << diagnostic.Message);
		REQUIRE(status == Status::Ok);
		return output.Data;
	}
}
TEST_CASE(
	"Argument owns copied immutable observations and distinguishes missing from empty", "[source_argument]"
) {
	std::vector<AuthoredValue> table{
		{"name", std::string{}}, {"false", false}, {"zero", int64_t{0}}, {"not_defined", UndefinedValue{}}
	};
	auto host = Provider(table);
	table[0].Data = std::string{"changed"};
	table.clear();
	CHECK(Evaluated(Graph(), &host) == Value{std::string{}});
	CHECK(Evaluated(Graph(0, std::string{"fallback"}, "missing"), &host) == Value{std::string{"fallback"}});
	CHECK(Evaluated(Graph(0, int64_t{73}, "not_defined"), &host) == Value{int64_t{73}});
	CHECK(Evaluated(Graph(0, 1., "false"), &host) == Value{false});
	CHECK(Evaluated(Graph(0, 1., "zero"), &host) == Value{int64_t{0}});
}
TEST_CASE("Argument exact name precedes underscore space lookup", "[source_argument]") {
	auto host = Provider({{"a_b", int64_t{1}}, {"a b", int64_t{2}}, {"x y z", int64_t{3}}});
	CHECK(Evaluated(Graph(0, 0., "a_b"), &host) == Value{int64_t{1}});
	CHECK(Evaluated(Graph(0, 0., "x_y_z"), &host) == Value{int64_t{3}});
	host = Provider({{"a_b", UndefinedValue{}}, {"a b", int64_t{2}}});
	CHECK(Evaluated(Graph(0, 0., "a_b"), &host) == Value{int64_t{2}});
}
TEST_CASE("Argument Number preserves booleans and exact int64 observations", "[source_argument]") {
	constexpr int64_t exact = INT64_C(9007199254740993);
	for (const Value &raw : {Value{false}, Value{true}, Value{exact}, Value{int64_t{-73}}, Value{1.25}}) {
		auto host = Provider({{"name", raw}});
		CHECK(Evaluated(Graph(1), &host) == raw);
		CHECK(Evaluated(Graph(1, raw, "absent"), &host) == raw);
	}
}
TEST_CASE(
	"Argument explicit primitive real profile follows prefix parse and caught failure", "[source_argument]"
) {
	const std::pair<std::string, double> samples[]{
		{"", 0},
		{"invalid", 0},
		{"nan", 0},
		{"inf", 0},
		{"12.5suffix", 12.5},
		{"  +.25e2tail", 25},
		{"0xfftail", 255},
		{" 0xff", 0},
		{"0Xff", 0},
		{"2e", 2},
		{"1,000", 1},
		{"+-12", 0},
		{"++12", 0},
		{"--12", 0}
	};
	for (const auto &[text, expected] : samples) {
		auto host = Provider({{"name", text}});
		CHECK(Evaluated(Graph(1), &host) == Value{expected});
	}
	ArrayValue array{ValueType::Scalar, {1., 2.}};
	auto host = Provider({{"name", array}});
	CHECK(Evaluated(Graph(1), &host) == Value{0.});
}
TEST_CASE(
	"Argument String keeps full source array defaults and recorded raw alternatives", "[source_argument]"
) {
	ArrayValue array;
	array.ElementType = ValueType::Any;
	array.Items = {{ElementValue{int64_t{3}}}, {ElementValue{std::string{"text"}}}};
	auto host = Provider({});
	CHECK(Evaluated(Graph(0, array), &host) == Value{array});
	host = Provider({{"name", array}});
	CHECK(Evaluated(Graph(), &host) == Value{array});
	CHECK(FindCatalogueInput(*FindCatalogueEntry("pc.argument"), "default_value")->Type == ValueType::Any);
	CHECK(FindCatalogueEntry("pc.argument")->Outputs[0].Type == ValueType::Any);
}
TEST_CASE(
	"Argument dynamic resolved Type changes mode without changing authored carrier", "[source_argument]"
) {
	auto host = Provider({{"name", std::string{"12.5units"}}});
	for (double mode : {0., 1.}) {
		auto d = Graph();
		d.Nodes.push_back({"mode", "pc.number_simple", "", {}, {{"value", mode}}});
		d.Links.push_back({"mode", "number", "argument", "type"});
		CHECK(Evaluated(d, &host) == (mode == 0 ? Value{std::string{"12.5units"}} : Value{12.5}));
	}
}
TEST_CASE(
	"Argument mode domain preserves raw Text getter and projects integer consumer", "[source_argument]"
) {
	auto host = Provider({{"name", 2.5}});
	for (int64_t mode : {0, 1}) {
		auto d = Graph(mode);
		d.Nodes.push_back({"consumer", "pc.vector2", "", {}, {{"integer", true}}});
		d.Links.push_back({"argument", "value", "consumer", "x"});
		d.Outputs = {{"out", "consumer", "x"}};
		CHECK(Evaluated(d, &host) == Value{2.});
	}
	// The raw Text source getter retains numeric values before the consumer's own update.
	auto d = Graph(0);
	d.Nodes.push_back({"consumer", "pc.argument", "", {}, {{"tag", std::string{"missing"}}}});
	d.Links.push_back({"argument", "value", "consumer", "default_value"});
	d.Outputs = {{"out", "consumer", "value"}};
	CHECK(Evaluated(d, &host) == Value{2.5});
}
TEST_CASE(
	"Argument exact final source receipts replay and stale receipts fail atomically", "[source_argument]"
) {
	auto d = Graph(1);
	auto p = Compiled(d);
	EvaluationRequest request{.Tick = 3, .Subframe = .25};
	HostNodeCapture capture;
	Diagnostic diagnostic;
	REQUIRE(PrepareHostCapture(d, p, "argument", request, capture, diagnostic) == Status::Ok);
	capture.Outputs = {{"value", int64_t{INT64_C(9007199254740993)}}};
	request.HostCaptures = std::span<const HostNodeCapture>(&capture, 1);
	EvaluatedValue output;
	REQUIRE(EvaluateValue(d, p, "out", request, output, diagnostic) == Status::Ok);
	CHECK(output.Data == Value{int64_t{INT64_C(9007199254740993)}});
	const auto prior = output;
	request.Subframe = .5;
	CHECK(EvaluateValue(d, p, "out", request, output, diagnostic) == Status::UnsupportedExecution);
	CHECK(output == prior);
	request.Subframe = .25;
	capture.Outputs[0].Data = std::string{"unconverted"};
	CHECK(EvaluateValue(d, p, "out", request, output, diagnostic) == Status::UnsupportedExecution);
	CHECK(output == prior);
}
TEST_CASE(
	"Argument provider refuses invalid observations and tiny replacement before changing table",
	"[source_argument]"
) {
	auto host = Provider({{"name", int64_t{7}}});
	Diagnostic diagnostic;
	const auto retained = host.RetainedBytes();
	std::vector<AuthoredValue> replacement{{"name", int64_t{9}}};
	CHECK(host.Prepare(replacement, 1, diagnostic) == Status::LimitExceeded);
	CHECK(host.RetainedBytes() == retained);
	CHECK(Evaluated(Graph(), &host) == Value{int64_t{7}});
	replacement[0].Data = std::numeric_limits<double>::infinity();
	CHECK(host.Prepare(replacement, Limits::MaximumEvaluationBytes, diagnostic) == Status::InvalidValue);
	replacement = {{"x", 1.}, {"x", 2.}};
	CHECK(host.Prepare(replacement, Limits::MaximumEvaluationBytes, diagnostic) == Status::DuplicateId);
	CHECK(Evaluated(Graph(), &host) == Value{int64_t{7}});
}
TEST_CASE(
	"Argument host receipt byte refusal leaves previously captured observation intact", "[source_argument]"
) {
	auto host = Provider({{"name", std::string(1024, 'x')}});
	auto d = Graph();
	EvaluationRequest request;
	std::vector<AuthoredValue> controls = d.Nodes[0].Values;
	HostNodeInvocation invocation{d.Nodes[0], request, controls, {}, Limits::MaximumEvaluationBytes};
	HostNodeCapture receipt;
	std::string failure;
	REQUIRE(host.Capture(invocation, receipt, failure));
	const auto previous = receipt.Outputs;
	invocation.MaximumOperationBytes = 1;
	CHECK_FALSE(host.Capture(invocation, receipt, failure));
	CHECK(receipt.Outputs == previous);
}
TEST_CASE(
	"Argument named conversion boundary rejects nonfinite and uncaptured object conversions",
	"[source_argument]"
) {
	for (std::string text : {"Infinity", "1e999"}) {
		auto host = Provider({{"name", text}});
		auto d = Graph(1);
		auto p = Compiled(d);
		EvaluationRequest request;
		request.HostProvider = &host;
		EvaluatedValue output;
		output.Data = int64_t{73};
		const auto prior = output;
		Diagnostic diagnostic;
		CHECK(EvaluateValue(d, p, "out", request, output, diagnostic) == Status::UnsupportedExecution);
		CHECK(output == prior);
	}
}

TEST_CASE("Argument exposes resolved source domain without coercing its native value", "[source_argument]") {
	auto host = Provider({{"name", int64_t{73}}});
	for (int64_t mode : {0, 1}) {
		auto d = Graph(mode);
		auto p = Compiled(d);
		EvaluationRequest request;
		request.HostProvider = &host;
		EvaluatedValue output;
		Diagnostic diagnostic;
		REQUIRE(EvaluateValue(d, p, "out", request, output, diagnostic) == Status::Ok);
		CHECK(output.Data == Value{int64_t{73}});
		REQUIRE(output.Domain);
		CHECK(output.Domain->Type == (mode == 0 ? ValueType::Text : ValueType::Scalar));
		CHECK(output.Domain->Kind == (mode == 0 ? SourceSocketKind::Text : SourceSocketKind::Float));
	}
}
TEST_CASE(
	"Argument requires explicit host observation and keeps previous output on refusal", "[source_argument]"
) {
	auto d = Graph();
	auto p = Compiled(d);
	EvaluatedValue output;
	output.Data = int64_t{73};
	const auto previous = output;
	Diagnostic diagnostic;
	CHECK(EvaluateValue(d, p, "out", {}, output, diagnostic) == Status::UnsupportedExecution);
	CHECK(output == previous);
	auto host = Provider({{"name", StructValue{}}});
	CHECK(Evaluated(d, &host) == Value{StructValue{}});
	d = Graph(1);
	p = Compiled(d);
	EvaluationRequest request;
	request.HostProvider = &host;
	CHECK(EvaluateValue(d, p, "out", request, output, diagnostic) == Status::UnsupportedExecution);
	CHECK(output == previous);
}

TEST_CASE(
	"Argument input snapshot preserves source output provenance through downstream receipt",
	"[source_argument]"
) {
	auto host = Provider({{"name", int64_t{9007199254740993}}});
	for (int64_t mode : {0, 1}) {
		auto d = Graph(mode);
		d.Nodes.push_back({"consumer", "pc.argument", "", {}, {{"tag", std::string{"absent"}}}});
		d.Links.push_back({"argument", "value", "consumer", "default_value"});
		auto p = Compiled(d);
		EvaluationRequest request;
		request.HostProvider = &host;
		EvaluationSnapshot snapshot;
		Diagnostic diagnostic;
		REQUIRE(EvaluateNodeInputs(d, p, "consumer", request, snapshot, diagnostic) == Status::Ok);
		const EvaluationInputValue *resolved = nullptr;
		for (const auto &value : snapshot.Values())
			if (value.Port == "default_value") resolved = &value;
		REQUIRE(resolved);
		CHECK(resolved->Data == Value{int64_t{9007199254740993}});
		CHECK(resolved->Linked);
		REQUIRE(resolved->Domain);
		CHECK(resolved->Domain->Kind == (mode == 0 ? SourceSocketKind::Text : SourceSocketKind::Float));
	}
}
TEST_CASE(
	"Argument Any bridge is limited to Default value and refuses malformed arrays", "[source_argument]"
) {
	auto d = Graph();
	Plan p;
	Diagnostic diagnostic;
	d.Nodes[0].Values[0].Data = int64_t{2};
	CHECK(Compile(d, p, diagnostic) == Status::TypeMismatch);
	d = Graph();
	ArrayValue mixed{ValueType::Scalar, {1.}};
	mixed.Nested = {{2.}};
	d.Nodes[0].Values[2].Data = mixed;
	CHECK(Compile(d, p, diagnostic) == Status::InvalidValue);
}

TEST_CASE(
	"Argument raw animated defaults use represented source keys after native reload", "[source_argument]"
) {
	auto host = Provider({});
	for (const Value &raw : {Value{true}, Value{int64_t{9007199254740993}}, Value{std::string{"raw"}}}) {
		auto d = Graph(0, raw, "absent");
		d.Nodes[0].SourceAnimatedInputs = {"default_value"};
		d.Keyframes = {{"argument", "default_value", 0, raw, "source", KeyframeEase{}}};
		d.Tracks = {{"argument", "default_value", "hold", -1}};
		CHECK(Evaluated(d, &host) == raw);
	}
}
TEST_CASE(
	"Argument raw default permission rejects runtime-only authored payloads and keys", "[source_argument]"
) {
	SurfaceValue surface;
	surface.Data = {1, 1, {10, 20, 30, 255}, 0};
	auto d = Graph(0, surface);
	Plan p;
	Diagnostic diagnostic;
	CHECK(Compile(d, p, diagnostic) == Status::TypeMismatch);
	CHECK(diagnostic.Port == "default_value");
	d = Graph();
	d.Keyframes = {{"argument", "default_value", 0, surface, "source", KeyframeEase{}}};
	d.Tracks = {{"argument", "default_value", "hold", -1}};
	CHECK(Compile(d, p, diagnostic) == Status::TypeMismatch);
	CHECK(diagnostic.Port == "default_value");
	Document decoded;
	CHECK(Read(Write(d), decoded, diagnostic) != Status::Ok);
}

TEST_CASE(
	"Typed argument assignments preserve explicit primitive payloads", "[source_argument][argument_options]"
) {
	SourceArgumentHost host;
	Diagnostic diagnostic;
	const std::string_view text[] = {"empty=", "name=a=b"};
	const std::string_view boolean[] = {"enabled=false"};
	const std::string_view integer[] = {"exact=9007199254740993", "minimum=-9223372036854775808"};
	const std::string_view real[] = {"fraction=1.25e-2"};
	REQUIRE(
		host.PrepareOptions({text, boolean, integer, real}, Limits::MaximumEvaluationBytes, diagnostic) ==
		Status::Ok
	);
	CHECK(Evaluated(Graph(0, std::string{"fallback"}, "empty"), &host) == Value{std::string{}});
	CHECK(Evaluated(Graph(0, 0., "name"), &host) == Value{std::string{"a=b"}});
	CHECK(Evaluated(Graph(1, 1., "enabled"), &host) == Value{false});
	CHECK(Evaluated(Graph(1, 0., "exact"), &host) == Value{INT64_C(9007199254740993)});
	CHECK(Evaluated(Graph(1, 0., "minimum"), &host) == Value{INT64_MIN});
	CHECK(Evaluated(Graph(1, 0., "fraction"), &host) == Value{.0125});
}
TEST_CASE(
	"Malformed or duplicate typed options preserve the previous table", "[source_argument][argument_options]"
) {
	auto host = Provider({{"name", int64_t{73}}});
	const auto retained = host.RetainedBytes();
	Diagnostic diagnostic;
	const std::string_view bad[] = {"", "missing", "=value"};
	for (const auto assignment : bad) {
		const std::string_view values[] = {assignment};
		CHECK(
			host.PrepareOptions({values, {}, {}, {}}, Limits::MaximumEvaluationBytes, diagnostic) ==
			Status::InvalidValue
		);
		CHECK(host.RetainedBytes() == retained);
	}
	const std::string_view text[] = {"name=changed"}, boolean[] = {"name=true"};
	CHECK(
		host.PrepareOptions({text, boolean, {}, {}}, Limits::MaximumEvaluationBytes, diagnostic) ==
		Status::DuplicateId
	);
	for (const auto raw : {"truex", "1", ""}) {
		const std::string value = "name=" + std::string(raw);
		const std::string_view values[] = {value};
		CHECK(
			host.PrepareOptions({{}, values, {}, {}}, Limits::MaximumEvaluationBytes, diagnostic) ==
			Status::InvalidValue
		);
	}
	for (const auto raw : {"1.5", "9223372036854775808", "1tail", ""}) {
		const std::string value = "name=" + std::string(raw);
		const std::string_view values[] = {value};
		CHECK(
			host.PrepareOptions({{}, {}, values, {}}, Limits::MaximumEvaluationBytes, diagnostic) ==
			Status::InvalidValue
		);
	}
	for (const auto raw : {"nan", "inf", "1e9999", "1tail", ""}) {
		const std::string value = "name=" + std::string(raw);
		const std::string_view values[] = {value};
		CHECK(
			host.PrepareOptions({{}, {}, {}, values}, Limits::MaximumEvaluationBytes, diagnostic) ==
			Status::InvalidValue
		);
	}
	CHECK(Evaluated(Graph(), &host) == Value{int64_t{73}});
	CHECK(host.RetainedBytes() == retained);
}
TEST_CASE(
	"Argument option quota failure and empty replacement are atomic", "[source_argument][argument_options]"
) {
	auto host = Provider({{"name", int64_t{73}}});
	Diagnostic diagnostic;
	const std::string_view text[] = {"name=new"};
	CHECK(host.PrepareOptions({text, {}, {}, {}}, host.RetainedBytes(), diagnostic) == Status::LimitExceeded);
	CHECK(Evaluated(Graph(), &host) == Value{int64_t{73}});
	const std::vector<std::string_view> excess(Limits::MaximumLinks + 1, "name=value");
	CHECK(
		host.PrepareOptions({excess, {}, {}, {}}, Limits::MaximumEvaluationBytes, diagnostic) ==
		Status::LimitExceeded
	);
	const std::string oversized(Limits::MaximumTextBytes + 1, 'x');
	const std::string assignment = "name=" + oversized;
	const std::string_view huge[] = {assignment};
	CHECK(
		host.PrepareOptions({huge, {}, {}, {}}, Limits::MaximumEvaluationBytes, diagnostic) ==
		Status::LimitExceeded
	);
	CHECK(Evaluated(Graph(), &host) == Value{int64_t{73}});
	REQUIRE(host.PrepareOptions({}, Limits::MaximumEvaluationBytes, diagnostic) == Status::Ok);
	CHECK(Evaluated(Graph(), &host) == Value{std::string{"fallback"}});
}

TEST_CASE("Argument Number accepts source unicode whitespace before numeric prefixes", "[source_argument]") {
	const std::string_view spaces[]{
		"\xc2\xa0",
		"\xe1\x9a\x80",
		"\xe2\x80\x80",
		"\xe2\x80\x81",
		"\xe2\x80\x82",
		"\xe2\x80\x83",
		"\xe2\x80\x84",
		"\xe2\x80\x85",
		"\xe2\x80\x86",
		"\xe2\x80\x87",
		"\xe2\x80\x88",
		"\xe2\x80\x89",
		"\xe2\x80\x8a",
		"\xe2\x80\xa8",
		"\xe2\x80\xa9",
		"\xe2\x80\xaf",
		"\xe2\x81\x9f",
		"\xe3\x80\x80",
		"\xef\xbb\xbf"
	};
	for (const auto space : spaces) {
		const std::string prefix = " \t" + std::string(space) + " \t" + std::string(space);
		auto host = Provider({{"name", prefix + "+.25e2tail"}});
		CHECK(Evaluated(Graph(1), &host) == Value{25.});
		CHECK(Evaluated(Graph(1, prefix + "-12.5suffix", "absent"), &host) == Value{-12.5});
		host = Provider({{"name", prefix}});
		CHECK(Evaluated(Graph(1), &host) == Value{0.});
		host = Provider({{"name", prefix + "0xff"}});
		CHECK(Evaluated(Graph(1), &host) == Value{0.});
	}
}
TEST_CASE(
	"Argument Number catches source nonnumeric unicode without widening whitespace", "[source_argument]"
) {
	const std::string samples[]{
		"\xc2\x85"
		"12",
		"\xe1\xa0\x8e"
		"12",
		"\xe2\x80\x8b"
		"12",
		"\xc3\xa9"
		"12",
		"+\xc2\xa0"
		"12",
		"\xe2\x80\x83"
		"invalid"
	};
	for (const auto &text : samples) {
		auto host = Provider({{"name", text}});
		CHECK(Evaluated(Graph(1), &host) == Value{0.});
	}
}

TEST_CASE("Argument unicode whitespace preserves nonfinite receipt refusals", "[source_argument]") {
	auto graph = Graph(1);
	auto plan = Compiled(graph);
	EvaluationRequest request;
	HostNodeCapture prior;
	Diagnostic diagnostic;
	REQUIRE(PrepareHostCapture(graph, plan, "argument", request, prior, diagnostic) == Status::Ok);
	prior.Outputs = {{"value", 77.}};
	const auto original = prior;
	const HostNodeInvocation invocation{
		graph.Nodes[0], request, original.Inputs, {}, Limits::MaximumEvaluationBytes
	};
	for (const std::string text :
		 {"\xc2\xa0"
		  "Infinity",
		  "\xef\xbb\xbf"
		  "-Infinity",
		  "\xe2\x80\x83"
		  "1e999"}) {
		auto host = Provider({{"name", text}});
		std::string failure;
		CHECK_FALSE(host.Capture(invocation, prior, failure));
		CHECK_FALSE(failure.empty());
		CHECK(prior.Authored == original.Authored);
		CHECK(prior.Inputs == original.Inputs);
		CHECK(prior.Outputs == original.Outputs);
		CHECK(prior.Tick == original.Tick);
		CHECK(prior.Subframe == original.Subframe);
		CHECK(prior.State == original.State);
		CHECK(prior.Failure == original.Failure);
	}
}
