#include <engine/imagegraph/Document.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <limits>

TEST_SUITE_ID("engine.imagegraph.inputs")
TEST_DEPENDS("engine.imagegraph.document")
TEST_DEPENDS("engine.imagegraph.evaluate")

using namespace engine::imagegraph;

namespace {
	Document Named() {
		return {
			.Nodes = {{"solid", Solid{}, {}, {}}},
			.Outputs = {{"main", "solid"}},
			.Parameters = {{"size", 2.0}, {"ink", std::array<uint8_t, 4>{17, 23, 31, 255}}},
			.Bindings = {{"solid", "width", "size"}, {"solid", "colour", "ink"}}
		};
	}
}
TEST_CASE(
	"named defaults and live overrides resolve to independent evaluated snapshots", "[imagegraph][inputs]"
) {
	Document authored = Named(), resolved;
	Diagnostic diagnostic;
	REQUIRE(ResolveInputs(authored, {}, resolved, diagnostic));
	CHECK(resolved.Parameters.empty());
	CHECK(resolved.Bindings.empty());
	CHECK(std::get<Solid>(resolved.Nodes[0].Value).Width == 2);
	CHECK(std::get<Solid>(authored.Nodes[0].Value).Width == 1);
	const std::array<InputOverride, 1> overrides{{{"size", 3.0}}};
	REQUIRE(ResolveInputs(authored, overrides, resolved, diagnostic));
	Plan plan;
	REQUIRE(Compile(resolved, plan, diagnostic));
	Image image;
	REQUIRE(Evaluate(resolved, plan, {}, {}, image, diagnostic));
	CHECK(image.Width == 3);
	CHECK(image.Height == 1);
	CHECK(
		image.Pixels == std::vector<std::byte>{
							std::byte{17},
							std::byte{23},
							std::byte{31},
							std::byte{255},
							std::byte{17},
							std::byte{23},
							std::byte{31},
							std::byte{255},
							std::byte{17},
							std::byte{23},
							std::byte{31},
							std::byte{255}
						}
	);
	REQUIRE(Compile(authored, plan, diagnostic));
	REQUIRE(EvaluateTyped(authored, plan, {}, {}, image, diagnostic));
	CHECK(image.Width == 2);
}
TEST_CASE(
	"input refusal preserves the accepted snapshot and rejects malformed bindings", "[imagegraph][inputs]"
) {
	Document document = Named(), accepted = Named();
	accepted.Nodes[0].Id = "accepted";
	Diagnostic diagnostic;
	std::vector<InputOverride> overrides;
	SECTION("unknown override") {
		overrides.push_back({"unknown", 2.0});
	}
	SECTION("duplicate override") {
		overrides = {{"size", 2.0}, {"size", 3.0}};
	}
	SECTION("type mismatch") {
		overrides.push_back({"size", true});
	}
	SECTION("nonfinite") {
		overrides.push_back({"size", std::numeric_limits<double>::infinity()});
	}
	SECTION("fractional dimension") {
		overrides.push_back({"size", 2.5});
	}
	SECTION("zero dimension") {
		overrides.push_back({"size", 0.0});
	}
	SECTION("oversized dimension") {
		overrides.push_back({"size", 4097.0});
	}
	SECTION("nonportable parameter") {
		document.Parameters[0].Name = "has spaces";
	}
	SECTION("duplicate parameter") {
		document.Parameters.push_back(document.Parameters[0]);
	}
	SECTION("missing input") {
		document.Bindings[0].Input = "missing";
	}
	SECTION("missing node") {
		document.Bindings[0].Node = "missing";
	}
	SECTION("missing property") {
		document.Bindings[0].Property = "depth";
	}
	SECTION("wrong property type") {
		document.Bindings[0].Input = "ink";
	}
	SECTION("duplicate binding") {
		document.Bindings.push_back(document.Bindings[0]);
	}
	SECTION("too many parameters") {
		document.Parameters.resize(Limits::MaximumParameters + 1);
	}
	SECTION("too many bindings") {
		document.Bindings.resize(Limits::MaximumBindings + 1);
	}
	SECTION("oversized string default") {
		document.Parameters.push_back({"path", std::string(4097, 'a')});
	}
	CHECK_FALSE(ResolveInputs(document, overrides, accepted, diagnostic));
	CHECK(accepted.Nodes[0].Id == "accepted");
	CHECK_FALSE(diagnostic.Message.empty());
}
TEST_CASE("every bindable operation control accepts its declared typed value", "[imagegraph][inputs]") {
	struct Control {
		Operation Op;
		std::string Property;
		InputValue Value;
	};
	const std::vector<Control> controls{
		{Source{"old.png"}, "path", std::string("new.png")},
		{Solid{}, "width", 4.0},
		{Solid{}, "height", 3.0},
		{Solid{}, "colour", std::array<uint8_t, 4>{1, 2, 3, 4}},
		{Resize{}, "width", 4.0},
		{Resize{}, "height", 3.0},
		{Resize{}, "filter", std::string("bilinear")},
		{Crop{}, "width", 4.0},
		{Crop{}, "height", 3.0},
		{Crop{}, "x", double(INT32_MIN)},
		{Crop{}, "y", double(INT32_MAX)},
		{Transform{}, "width", 4.0},
		{Transform{}, "height", 3.0},
		{Transform{}, "filter", std::string("bilinear")},
		{Transform{}, "translate_x", 2.0},
		{Transform{}, "translate_y", 2.0},
		{Transform{}, "scale_x", -2.0},
		{Transform{}, "scale_y", -2.0},
		{Transform{}, "degrees", 180.0},
		{Transform{}, "pivot_x", 2.0},
		{Transform{}, "pivot_y", 2.0},
		{Flip{}, "horizontal", true},
		{Flip{}, "vertical", true},
		{Blend{}, "opacity", 0.5}
	};
	for (const auto &control : controls) {
		CAPTURE(control.Property, Kind(control.Op));
		CHECK(PropertyType(control.Op, control.Property) == int(control.Value.index()));
		Document document{
			.Nodes = {{"base", Solid{}, {}, {}}, {"node", control.Op, {}, {}}},
			.Outputs = {{"main", "node"}},
			.Parameters = {{"input", control.Value}},
			.Bindings = {{"node", control.Property, "input"}}
		};
		if (!std::holds_alternative<Source>(control.Op) && !std::holds_alternative<Solid>(control.Op))
			document.Nodes[1].Inputs.push_back("base");
		if (std::holds_alternative<Blend>(control.Op)) document.Nodes[1].Inputs.push_back("base");
		Document resolved;
		Diagnostic diagnostic;
		REQUIRE(ResolveInputs(document, {}, resolved, diagnostic));
	}
	CHECK(PropertyType(Solid{}, "filter") == -1);
}
TEST_CASE(
	"bound integer filter path and scalar values retain operation constraints", "[imagegraph][inputs]"
) {
	Operation operation;
	std::string property;
	InputValue value;
	SECTION("crop overflow") {
		operation = Crop{};
		property = "x";
		value = double(INT32_MAX) + 1;
	}
	SECTION("crop fractional") {
		operation = Crop{};
		property = "y";
		value = 1.5;
	}
	SECTION("scale zero") {
		operation = Transform{};
		property = "scale_x";
		value = 0.0;
	}
	SECTION("opacity range") {
		operation = Blend{};
		property = "opacity";
		value = 1.01;
	}
	SECTION("filter name") {
		operation = Resize{};
		property = "filter";
		value = std::string("smooth");
	}
	SECTION("parent source") {
		operation = Source{"old.png"};
		property = "path";
		value = std::string("../escape.png");
	}
	Document document{
		.Nodes = {{"base", Solid{}, {}, {}}, {"node", operation, {}, {}}},
		.Outputs = {{"main", "node"}},
		.Parameters = {{"input", value}},
		.Bindings = {{"node", property, "input"}}
	};
	if (!std::holds_alternative<Source>(operation)) document.Nodes[1].Inputs.push_back("base");
	if (std::holds_alternative<Blend>(operation)) document.Nodes[1].Inputs.push_back("base");
	Document resolved;
	Diagnostic diagnostic;
	CHECK_FALSE(ResolveInputs(document, {}, resolved, diagnostic));
}
TEST_CASE(
	"version two preserves typed defaults bindings and colour contracts while static stays version one",
	"[imagegraph][inputs]"
) {
	Document authored = Named();
	authored.Nodes.push_back({"source", Source{"data.atex", SourceInterpretation::Data}, {}, {}});
	authored.Outputs.push_back({"data", "source", OutputSpace::Linear});
	authored.Parameters.push_back({"enabled", true});
	authored.Parameters.push_back({"path", std::string("data.atex")});
	Diagnostic diagnostic;
	std::string encoded;
	REQUIRE(Write(authored, encoded, diagnostic));
	CHECK(encoded.find("\"version\": 2") != std::string::npos);
	Document reopened;
	REQUIRE(Read(encoded, reopened, diagnostic));
	CHECK(reopened.Parameters.size() == 4);
	CHECK(reopened.Bindings.size() == 2);
	CHECK(std::get<bool>(reopened.Parameters[2].Default));
	CHECK(std::get<std::string>(reopened.Parameters[3].Default) == "data.atex");
	CHECK(std::get<Source>(reopened.Nodes[1].Value).Interpretation == SourceInterpretation::Data);
	CHECK(reopened.Outputs[1].Space == OutputSpace::Linear);
	std::string canonical;
	REQUIRE(Write(reopened, canonical, diagnostic));
	CHECK(canonical == encoded);
	std::string corrupted = encoded;
	corrupted.replace(corrupted.find("\"number\""), 8, "\"unknown\"");
	CHECK_FALSE(Read(corrupted, reopened, diagnostic));
	CHECK(reopened.Parameters.size() == 4);
	Document plain{.Nodes = {{"solid", Solid{}, {}, {}}}, .Outputs = {{"main", "solid"}}};
	REQUIRE(Write(plain, encoded, diagnostic));
	CHECK(encoded.find("\"version\": 1") != std::string::npos);
}

TEST_CASE(
	"saving bound controls refuses fallbacks that strict project decoding cannot reconstruct",
	"[imagegraph][inputs]"
) {
	Document document = Named();
	std::get<Solid>(document.Nodes[0].Value).Width = 0;
	Document snapshot;
	Diagnostic diagnostic;
	REQUIRE(ResolveInputs(document, {}, snapshot, diagnostic));
	CHECK(std::get<Solid>(snapshot.Nodes[0].Value).Width == 2);
	std::string saved = "accepted";
	CHECK_FALSE(Write(document, saved, diagnostic));
	CHECK(saved == "accepted");
}

TEST_CASE(
	"version two refuses mistyped defaults unknown fields and malformed colour without replacing authored "
	"state",
	"[imagegraph][inputs]"
) {
	Document document = Named();
	Diagnostic diagnostic;
	std::string encoded;
	REQUIRE(Write(document, encoded, diagnostic));
	SECTION("unknown parameter field") {
		const auto at = encoded.find("\"type\": \"number\"");
		REQUIRE(at != std::string::npos);
		encoded.insert(at, "\"unknown\": 1, ");
	}
	SECTION("number tagged as boolean") {
		const auto at = encoded.find("\"number\"");
		REQUIRE(at != std::string::npos);
		encoded.replace(at, 8, "\"boolean\"");
	}
	SECTION("colour channel out of range") {
		const auto at = encoded.find("31,\n");
		REQUIRE(at != std::string::npos);
		encoded.replace(at, 2, "256");
	}
	SECTION("unknown binding field") {
		const auto at = encoded.find("\"property\":");
		REQUIRE(at != std::string::npos);
		encoded.insert(at, "\"unknown\": 1, ");
	}
	Document accepted = Named();
	accepted.Nodes[0].Id = "accepted";
	CHECK_FALSE(Read(encoded, accepted, diagnostic));
	CHECK(accepted.Nodes[0].Id == "accepted");
}
