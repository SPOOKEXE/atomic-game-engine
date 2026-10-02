#include <engine/imagegraph/FrameTime.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

TEST_SUITE_ID("engine.imagegraph.keyframe_kind")
using namespace engine::imagegraph;
namespace {
	Document Keys(Node node, std::string port, std::string output, Value first, Value last) {
		Document d;
		d.FormatVersion = 9;
		node.Id = "n";
		d.Nodes = {std::move(node)};
		d.Outputs = {{"out", "n", std::move(output)}};
		d.Keyframes = {
			{"n", port, 0, first, "source", KeyframeEase{}}, {"n", port, 4, last, "source", KeyframeEase{}}
		};
		d.Tracks = {{"n", port, "hold", -1}};
		d.Timeline = TimelineSettings{8, 0, 7, "loop", 30};
		return d;
	}
	EvaluatedValue Run(const Document &d, double time) {
		Document parsed;
		Diagnostic diagnostic;
		REQUIRE(Read(Write(d), parsed, diagnostic) == Status::Ok);
		CHECK(parsed == d);
		Plan plan;
		auto status = Compile(parsed, plan, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(status == Status::Ok);
		EvaluationRequest request;
		FrameTime t;
		REQUIRE(SplitFrameTime(time, t));
		REQUIRE(SetFrameTime(request, t));
		EvaluatedValue value;
		status = EvaluateValue(parsed, plan, "out", request, value, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(status == Status::Ok);
		return value;
	}
	void Mark(Document &d) {
		for (auto &key : d.Keyframes)
			key.Kind = KeyframeKind::Adder;
	}
	ArrayValue Numbers(std::initializer_list<double> values) {
		ArrayValue a{ValueType::Scalar, {}};
		for (double v : values)
			a.Elements.emplace_back(v);
		return a;
	}
}

TEST_CASE(
	"Source key kinds roundtrip with exact signed identity and atomic grammar", "[imagegraph][keyframe_kind]"
) {
	auto d = Keys({"", "value.number", "", {}, {{"value", 0.}}}, "value", "number", 0., 10.);
	REQUIRE(SetFrameTime(d.Keyframes.front(), {1, .25, true}));
	REQUIRE(SetFrameTime(d.Keyframes.back(), {1, .75, false}));
	d.Keyframes.front().Kind = KeyframeKind::Adder;
	const auto text = Write(d);
	CHECK(text.find("key_kind \"n\" \"value\" 1 adder") != std::string::npos);
	Document parsed;
	Diagnostic diagnostic;
	REQUIRE(Read(text, parsed, diagnostic) == Status::Ok);
	CHECK(parsed == d);
	const auto retained = parsed;
	for (const std::string marker :
		 {"key_kind \"n\" \"value\" 1 unknown\n",
		  "key_kind \"n\" \"value\" 1 normal\n",
		  "key_kind \"missing\" \"value\" 1 adder\n"}) {
		auto malformed = text;
		const auto start = malformed.find("key_kind");
		const auto end = malformed.find('\n', start);
		if (marker.find(" normal") != std::string::npos)
			malformed.insert(end + 1, marker);
		else
			malformed.replace(start, end - start + 1, marker);
		CHECK(Read(malformed, parsed, diagnostic) == Status::Malformed);
		CHECK(parsed == retained);
	}
	Plan plan;
	d.Keyframes.back() = d.Keyframes.front();
	d.Keyframes.back().Kind = KeyframeKind::Normal;
	CHECK(Compile(d, plan, diagnostic) == Status::DuplicateId);
	d.Keyframes.pop_back();
	d.Keyframes.front().Kind = static_cast<KeyframeKind>(99);
	CHECK(Compile(d, plan, diagnostic) == Status::InvalidValue);
	CHECK(diagnostic.Port == "value");
}

TEST_CASE(
	"Normal source markers keep legacy grammar and adder requires format9", "[imagegraph][keyframe_kind]"
) {
	Document d;
	d.Nodes = {{"n", "value.number", "", {}, {{"value", 0.}}}};
	d.Outputs = {{"out", "n", "number"}};
	d.Keyframes = {{"n", "value", 0, 0., "step"}};
	Diagnostic diagnostic;
	Document parsed;
	Plan plan;
	for (uint32_t version = 1; version <= 8; ++version) {
		d.FormatVersion = version;
		const auto text = Write(d);
		CHECK(text.find("key_kind") == std::string::npos);
		REQUIRE(Read(text, parsed, diagnostic) == Status::Ok);
		CHECK(parsed == d);
		d.Keyframes.front().Kind = KeyframeKind::Adder;
		CHECK(Compile(d, plan, diagnostic) == Status::UnsupportedVersion);
		CHECK(Read(text + "key_kind \"n\" \"value\" 0 adder\n", parsed, diagnostic) == Status::Malformed);
		d.Keyframes.front().Kind = KeyframeKind::Normal;
	}
}

TEST_CASE(
	"Adder markers use ordinary source typed interpolation without arithmetic additions",
	"[imagegraph][keyframe_kind]"
) {
	struct Case {
		Document Graph;
		Value ExpectedControl;
		Value ExpectedOutput;
	};
	std::vector<Case> cases;
	cases.push_back({Keys({"", "value.number", "", {}, {{"value", 0.}}}, "value", "number", 2., 6.), 4., 4.});
	cases.push_back(
		{Keys(
			 {"",
			  "value.text_get_char",
			  "",
			  {},
			  {{"text", std::string{"abcdefgh"}}, {"index", int64_t{2}}, {"amount", int64_t{1}}}},
			 "index",
			 "text",
			 int64_t{2},
			 int64_t{7}
		 ),
		 int64_t{4},
		 std::string{"d"}}
	);
	cases.push_back(
		{Keys({"", "pc.color", "", {}, {}}, "color", "color", Colour{2, 4, 6, 255}, Colour{6, 8, 10, 255}),
		 Colour{4, 6, 8, 255},
		 Colour{4, 6, 8, 255}}
	);
	cases.push_back(
		{Keys(
			 {"", "pc.point_in_area", "", {}, {{"area", Area{0, 0, 100, 100, 0, 0}}}},
			 "point",
			 "is_in",
			 Vector2{2, 4},
			 Vector2{6, 8}
		 ),
		 Vector2{4, 6},
		 true}
	);
	cases.push_back(
		{Keys(
			 {"", "pc.quarternion_from_euler", "", {}, {}},
			 "euler_rotation",
			 "rotation",
			 Vector3{0, 0, 0},
			 Vector3{90, 0, 0}
		 ),
		 Vector3{45, 0, 0},
		 Vector4{-std::sin(std::numbers::pi / 8), 0, 0, std::cos(std::numbers::pi / 8)}}
	);
	cases.push_back(
		{Keys({"", "pc.vector_split", "", {}, {}}, "vector", "x", Vector4{2, 4, 6, 8}, Vector4{6, 8, 10, 12}),
		 Vector4{4, 6, 8, 10},
		 4.}
	);
	cases.push_back(
		{Keys(
			 {"", "pc.point_in_area", "", {}, {}},
			 "area",
			 "is_in",
			 Area{0, 0, 1, 1, 0, 0},
			 Area{0, 0, 3, 3, 0, 0}
		 ),
		 Area{0, 0, 2, 2, 0, 0},
		 true}
	);
	cases.push_back(
		{Keys({"", "pc.number", "", {}, {}}, "value", "number", Numbers({2, 4, 6}), Numbers({6, 8})),
		 Numbers({4, 6}),
		 Numbers({4, 6})}
	);
	cases.push_back(
		{Keys(
			 {"", "pc.gradient_palette", "", {}, {}}, "interpolation", "gradient", EnumValue{0}, EnumValue{1}
		 ),
		 .5,
		 Gradient{0, {{0, Colour{255, 255, 255, 255}}, {.5, Colour{0, 0, 0, 255}}}}}
	);
	for (auto &c : cases) {
		INFO(c.Graph.Nodes.front().Type);
		for (const auto kind : {KeyframeKind::Normal, KeyframeKind::Adder}) {
			for (auto &key : c.Graph.Keyframes)
				key.Kind = kind;
			const auto actual = Run(c.Graph, 2);
			if (const auto *expected = std::get_if<Vector4>(&c.ExpectedOutput)) {
				const auto &value = std::get<Vector4>(actual.Data);
				CHECK(value.X == Catch::Approx(expected->X));
				CHECK(value.Y == Catch::Approx(expected->Y));
				CHECK(value.Z == Catch::Approx(expected->Z));
				CHECK(value.W == Catch::Approx(expected->W));
			} else
				CHECK(actual.Data == c.ExpectedOutput);
			Plan plan;
			Diagnostic diagnostic;
			REQUIRE(Compile(c.Graph, plan, diagnostic) == Status::Ok);
			EvaluationRequest request;
			request.Tick = 2;
			std::vector<AuthoredValue> values;
			auto status = ResolveNodeValues(c.Graph, plan, "out", "n", request, values, diagnostic);
			INFO(diagnostic.Message);
			REQUIRE(status == Status::Ok);
			const auto control = std::find_if(values.begin(), values.end(), [&](const AuthoredValue &v) {
				return v.Port == c.Graph.Keyframes.front().Port;
			});
			REQUIRE(control != values.end());
			CHECK(control->Data == c.ExpectedControl);
		}
	}
}

TEST_CASE(
	"Adder quaternion markers preserve explicit raw and Euler source modes", "[imagegraph][keyframe_kind]"
) {
	auto d = Keys(
		{"", "pc.quarternion_to_euler", "", {}, {}},
		"rotation",
		"euler_angles",
		Quaternion{0, 0, 0, 0},
		Quaternion{90, 0, 0, 0}
	);
	d.Tracks.front().QuaternionMode = 1;
	Mark(d);
	CHECK((std::get<Vector3>(Run(d, 2).Data) == Vector3{-45, 0, 0}));
	d.Tracks.front().QuaternionMode = 0;
	d.Keyframes.front().Data = Quaternion{};
	d.Keyframes.back().Data = Quaternion{-std::sqrt(.5), 0, 0, std::sqrt(.5)};
	CHECK((std::get<Vector3>(Run(d, 2).Data) == Vector3{-45, 0, 0}));
}

TEST_CASE("Adder source markers retain drivers cuts and loop scheduling", "[imagegraph][keyframe_kind]") {
	const std::array<KeyframeSourceDriver, 6> drivers{
		KeyframeLinearDriver{2},
		KeyframeSnapDriver{3},
		KeyframeBounceDriver{},
		KeyframeElasticDriver{},
		KeyframeCurveDriver{},
		KeyframeSineDriver{1, 2, 0, 0}
	};
	const std::array<double, 6> expected{22, 6, 6, 14, 6, 6};
	for (size_t i = 0; i < drivers.size(); ++i) {
		auto d = Keys({"", "value.number", "", {}, {{"value", 0.}}}, "value", "number", 0., 10.);
		d.Keyframes.front().Tick = 2;
		d.Keyframes.back().Tick = 12;
		d.Timeline = TimelineSettings{16, 0, 15, "loop", 30};
		for (auto &k : d.Keyframes) {
			k.SourceDriver = drivers[i];
			k.Kind = KeyframeKind::Adder;
		}
		CHECK(std::abs(std::get<double>(Run(d, 8).Data) - expected[i]) < 1e-12);
	}
	auto d = Keys({"", "value.number", "", {}, {{"value", 0.}}}, "value", "number", 0., 10.);
	Mark(d);
	d.Tracks.front().End = "loop";
	CHECK(std::get<double>(Run(d, 5).Data) == 2.5);
	d.Tracks.front().End = "ping";
	CHECK(std::get<double>(Run(d, 5).Data) == 7.5);
	d.Tracks.front().End = "wrap";
	CHECK(std::get<double>(Run(d, 6).Data) == 5);
	d.Keyframes.front().Ease->OutType = "cut";
	CHECK(std::get<double>(Run(d, 2).Data) == 10);
	d.Keyframes.back().Ease->InType = "cut";
	CHECK(std::get<double>(Run(d, 2).Data) == 0);
	REQUIRE(SetFrameTime(d.Keyframes.front(), {2, 0, true}));
	d.Tracks.front().End = "hold";
	CHECK(std::get<double>(Run(d, -1.5).Data) == 0);
}

TEST_CASE(
	"Adder markers retain reachable native boolean and text step behavior", "[imagegraph][keyframe_kind]"
) {
	auto boolean = Keys({"", "value.boolean", "", {}, {{"value", false}}}, "value", "boolean", false, true);
	auto text = Keys(
		{"", "value.text", "", {}, {{"value", std::string{"before"}}}},
		"value",
		"text",
		std::string{"before"},
		std::string{"after"}
	);
	for (auto *d : {&boolean, &text}) {
		for (auto &k : d->Keyframes) {
			k.Interpolation = "step";
			k.Ease.reset();
		}
		const auto first = Run(*d, 2.5).Data;
		const auto last = Run(*d, 4).Data;
		Mark(*d);
		CHECK(Run(*d, 2.5).Data == first);
		CHECK(Run(*d, 4).Data == last);
	}
	CHECK_FALSE(std::get<bool>(Run(boolean, 2.5).Data));
	CHECK(std::get<bool>(Run(boolean, 4).Data));
	CHECK(std::get<std::string>(Run(text, 2.5).Data) == "before");
	CHECK(std::get<std::string>(Run(text, 4).Data) == "after");
}
