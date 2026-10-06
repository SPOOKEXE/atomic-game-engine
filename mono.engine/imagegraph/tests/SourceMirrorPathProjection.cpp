#include "../src/SourceMirrorPathProjection.hpp"

#include "../src/SourceSeparatedVec2.hpp"

#include <engine/imagegraph/Document.hpp>
#include <engine/imagegraph/FeedbackHost.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.imagegraph.source_mirror_path_projection")
using namespace engine::imagegraph;
namespace {
	Path2D Line() {
		Path2D path;
		path.Anchors = {{{0, 2, 0, 0, 0, 0}}, {{8, 2, 0, 0, 0, 0}}};
		return path;
	}
	Image Coordinates() {
		Image image{8, 8, std::vector<uint8_t>(256), 0};
		for (uint32_t y = 0; y < 8; ++y)
			for (uint32_t x = 0; x < 8; ++x) {
				const size_t index = (y * 8 + x) * 4;
				image.Pixels[index] = uint8_t(x * 20);
				image.Pixels[index + 1] = uint8_t(y * 20);
				image.Pixels[index + 3] = 255;
			}
		return image;
	}
	Document Graph() {
		Document d;
		d.FormatVersion = 9;
		d.Project = ProjectSettings{};
		d.Project->SurfaceWidth = d.Project->SurfaceHeight = 8;
		d.Nodes = {
			{"input",
			 "pc.group_input",
			 "group",
			 {},
			 {{"input_type", EnumValue{11}}, {"subtype", EnumValue{0}}, {"vector_size", EnumValue{0}}}},
			{"source", "image.captured", "group", {}, {{"source_id", std::string("source")}}},
			{"mirror",
			 "pc.mirror_polar",
			 "group",
			 {},
			 {{"center", Vector2{.25, .99}}, {"scale", Vector2{0, 0}}, {"interpolate", EnumValue{1}}}}
		};
		Group group{"group", "Group"};
		group.Ports = {{"input", "input/parent", PortDirection::Input, "input"}};
		d.Groups = {group};
		d.Junctions = {{"input/parent", "group", ValueType::Any, Line()}};
		d.Links = {
			{"input/parent", "value", "input", "parent_value"},
			{"input", "value", "mirror", "center"},
			{"source", "image", "mirror", "surface_in"}
		};
		d.Outputs = {{"out", "mirror", "surface_out"}};
		return d;
	}
	Plan Compiled(const Document &d) {
		Plan p;
		Diagnostic diag;
		const auto status = Compile(d, p, diag);
		INFO(diag.Message << " " << diag.NodeId << ":" << diag.Port);
		REQUIRE(status == Status::Ok);
		return p;
	}
	Image Sample(
		const Document &d, uint64_t tick = 0, double subframe = 0, const GroupReplayState *replay = nullptr
	) {
		const auto source = Coordinates();
		const std::array sources{RequestImageSource{"source", source}};
		EvaluationRequest request;
		request.ImageSources = sources;
		request.Tick = tick;
		request.Subframe = subframe;
		request.GroupReplay = replay;
		request.GroupAuthoringRevision = replay ? 1 : 0;
		Diagnostic diag;
		Image image;
		const auto status = Evaluate(d, Compiled(d), "out", request, image, diag);
		INFO(diag.Message << " " << diag.NodeId << ":" << diag.Port);
		REQUIRE(status == Status::Ok);
		return image;
	}
	void SolidPixel(const Image &image, uint8_t red, uint8_t green) {
		REQUIRE(image.Width == 8);
		REQUIRE(image.Height == 8);
		for (size_t i = 0; i < image.Pixels.size(); i += 4) {
			CHECK(image.Pixels[i] == red);
			CHECK(image.Pixels[i + 1] == green);
			CHECK(image.Pixels[i + 2] == 0);
			CHECK(image.Pixels[i + 3] == 255);
		}
	}
}
TEST_CASE("Mirror Any-carried path reads consumer local X and bypasses Reference units", "[mirror_path]") {
	auto d = Graph();
	SolidPixel(Sample(d), 40, 40);
	d.Nodes.back().Values[0].Data = Vector2{.75, -.123};
	SolidPixel(Sample(d), 120, 40);
	d.Nodes.back().Values[0].Data = Vector2{.75, 1000};
	SolidPixel(Sample(d), 120, 40);
	Document restored;
	Diagnostic diag;
	REQUIRE(Read(Write(d), restored, diag) == Status::Ok);
	CHECK(restored == d);
	SolidPixel(Sample(restored), 120, 40);
}
TEST_CASE("Mirror path ratio animation replays fractional time and backseek", "[mirror_path]") {
	auto d = Graph();
	d.Keyframes = {
		{"mirror", "center", 0, Vector2{.25, 500}, "linear"},
		{"mirror", "center", 2, Vector2{.75, -500}, "linear"}
	};
	SolidPixel(Sample(d, 1), 80, 40);
	SolidPixel(Sample(d, 0, .5), 60, 40);
	SolidPixel(Sample(d, 2), 120, 40);
	SolidPixel(Sample(d, 0), 40, 40);
}
TEST_CASE("Mirror snapshot owns prepared physical coordinates and original Any domain", "[mirror_path]") {
	auto d = Graph();
	const auto source = Coordinates();
	const std::array sources{RequestImageSource{"source", source}};
	EvaluationRequest request;
	request.ImageSources = sources;
	EvaluationSnapshot snapshot;
	Diagnostic diag;
	REQUIRE(EvaluateNodeInputs(d, Compiled(d), "mirror", request, snapshot, diag) == Status::Ok);
	const auto center = std::find_if(snapshot.Values().begin(), snapshot.Values().end(), [](const auto &v) {
		return v.Port == "center";
	});
	REQUIRE(center != snapshot.Values().end());
	CHECK(center->Data == Value{Vector2{2, 2}});
	CHECK(center->Linked);
	REQUIRE(center->Domain);
	CHECK(center->Domain->Kind == SourceSocketKind::Any);
	const auto retained = snapshot.RetainedBytes();
	EvaluationSnapshot moved = std::move(snapshot);
	CHECK(moved.RetainedBytes() == retained);
	d.Junctions[0].Default = Path2D{};
	const auto copied = std::find_if(moved.Values().begin(), moved.Values().end(), [](const auto &v) {
		return v.Port == "center";
	});
	REQUIRE(copied != moved.Values().end());
	CHECK(copied->Data == Value{Vector2{2, 2}});
	CHECK(copied->Domain == center->Domain);
}
TEST_CASE("Mirror typed Path direct link retains source compatibility rejection", "[mirror_path]") {
	auto d = Graph();
	d.Nodes.push_back({"path", "pc.path", "group", {}, {}});
	d.Links[1] = {"path", "path_data", "mirror", "center"};
	Plan p;
	Diagnostic diag;
	CHECK(Compile(d, p, diag) == Status::TypeMismatch);
	CHECK(diag.NodeId == "mirror");
	CHECK(diag.Port == "center");
}
TEST_CASE("Mirror getter workspace refusal preserves published image and snapshot", "[mirror_path]") {
	auto d = Graph();
	const auto p = Compiled(d);
	const auto source = Coordinates();
	const std::array sources{RequestImageSource{"source", source}};
	EvaluationRequest request;
	request.ImageSources = sources;
	Image output = source;
	Diagnostic diag;
	CHECK(Evaluate(d, p, "out", request, output, diag, 1) == Status::LimitExceeded);
	CHECK(output.Pixels == source.Pixels);
}
TEST_CASE(
	"Mirror instance inherits animation flag while retaining its local ratio records", "[mirror_path]"
) {
	auto d = Graph();
	d.Nodes.back().SourceStaticInputs = {"center"};
	Node instance = d.Nodes.back();
	instance.Id = "instance";
	instance.InstanceBase = "mirror";
	instance.SourceStaticInputs.clear();
	instance.SourceAnimatedInputs = {"center"};
	instance.Values[0].Data = Vector2{.75, 0};
	d.Nodes.push_back(instance);
	d.Keyframes = {
		{"instance", "center", 0, Vector2{.25, 0}, "linear"},
		{"instance", "center", 2, Vector2{.75, 0}, "linear"}
	};
	d.Outputs[0].NodeId = "instance";
	SolidPixel(Sample(d, 2), 40, 40);
	// A local connection disables source useInstance and restores this getter's own animated flag.
	d.Links.push_back({"input", "value", "instance", "center"});
	SolidPixel(Sample(d, 2), 120, 40);
	Document restored;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(d), restored, diagnostic) == Status::Ok);
	CHECK(restored == d);
	SolidPixel(Sample(restored, 2), 120, 40);
}
TEST_CASE("Mirror path source endpoints distinguish clamp loop and negative ratios", "[mirror_path]") {
	auto d = Graph();
	d.Nodes.back().Values[0].Data = Vector2{-.25, 0};
	SolidPixel(Sample(d), 120, 40);
	d.Nodes.back().Values[0].Data = Vector2{1.25, 0};
	SolidPixel(Sample(d), 140, 40);
	std::get<Path2D>(*d.Junctions[0].Default).Loop = true;
	// Closing adds length8: length16 * frac(1.25) =4, so source samples x4.
	SolidPixel(Sample(d), 80, 40);
	d.Nodes.back().Values[0].Data = Vector2{1.125, 0};
	SolidPixel(Sample(d), 40, 40);
	d.Nodes.back().Values[0].Data = Vector2{1.875, 0};
	SolidPixel(Sample(d), 40, 40);
}
TEST_CASE(
	"Mirror feedback input capture preserves prepared ratios through seek and failed refresh", "[mirror_path]"
) {
	auto d = Graph();
	d.Nodes[1].Values[0].Data = std::string("feedback:out");
	d.Keyframes = {
		{"mirror", "center", 0, Vector2{.25, 0}, "linear"}, {"mirror", "center", 2, Vector2{.75, 0}, "linear"}
	};
	const auto p = Compiled(d);
	CapturedFeedbackHost host;
	Diagnostic diagnostic;
	const auto center = [&]() -> const EvaluationInputValue & {
		const auto values = host.Snapshot().Values();
		const auto found =
			std::find_if(values.begin(), values.end(), [](const auto &v) { return v.Port == "center"; });
		REQUIRE(found != values.end());
		return *found;
	};
	EvaluationRequest request;
	{
		const bool prepared = host.PrepareNodeInputs(d, p, 1, 0, "mirror", request, diagnostic);
		INFO(
			"tick=" << request.Tick << " code=" << int(diagnostic.Code) << " " << diagnostic.NodeId << ":"
					<< diagnostic.Port << " " << diagnostic.Message
		);
		REQUIRE(prepared);
	}
	CHECK(center().Data == Value{Vector2{2, 2}});
	request = {};
	request.Tick = 1;
	{
		const bool prepared = host.PrepareNodeInputs(d, p, 1, 0, "mirror", request, diagnostic);
		INFO(
			"tick=" << request.Tick << " code=" << int(diagnostic.Code) << " " << diagnostic.NodeId << ":"
					<< diagnostic.Port << " " << diagnostic.Message
		);
		REQUIRE(prepared);
	}
	CHECK(center().Data == Value{Vector2{4, 2}});
	const auto before = center();
	const auto bytes = host.Snapshot().RetainedBytes();
	request = {};
	request.Tick = 2;
	CHECK_FALSE(host.PrepareNodeInputs(d, p, 1, 0, "mirror", request, diagnostic, 1));
	CHECK(center().Data == before.Data);
	CHECK(center().Linked == before.Linked);
	CHECK(center().Domain == before.Domain);
	CHECK(host.Snapshot().RetainedBytes() == bytes);
	request = {};
	{
		const bool prepared = host.PrepareNodeInputs(d, p, 1, 0, "mirror", request, diagnostic);
		INFO(
			"tick=" << request.Tick << " code=" << int(diagnostic.Code) << " " << diagnostic.NodeId << ":"
					<< diagnostic.Port << " " << diagnostic.Message
		);
		REQUIRE(prepared);
	}
	CHECK(center().Data == Value{Vector2{2, 2}});
}

TEST_CASE("Mirror undefined local path ratio preserves the previous image", "[mirror_path]") {
	auto d = Graph();
	const auto image = Coordinates();
	const auto &entry = *FindCatalogueEntry("pc.mirror_polar");
	EvaluationRequest request;
	detail::NodeContext context{d.Nodes.back(), entry, request};
	context.ByteBudget = Limits::MaximumEvaluationBytes;
	const Value carried = Line();
	const Value empty = ArrayValue{ValueType::Scalar, {}};
	context.ValueViews.emplace_back("center", &carried);
	context.MirrorRawAnimators[3] = &empty;
	context.OutputImages.emplace_back("surface_out", image);
	const auto before = context.ValueViews;
	detail::SourceMirrorPathProjection projection(context);
	CHECK_FALSE(projection.Prepare());
	CHECK(context.FailureCode == Status::UnsupportedExecution);
	CHECK(context.FailurePort == "center");
	CHECK(context.ValueViews == before);
	CHECK(context.OutputImages[0].second == image);
}
TEST_CASE("Mirror shared local animator evaluates one-key drivers before path sampling", "[mirror_path]") {
	auto d = Graph();
	d.Nodes.back().SourceAnimatedInputs = {"center"};
	Keyframe key{"mirror", "center", 0, Vector2{.25, 20}, "source", KeyframeEase{}};
	key.SourceDriver = KeyframeLinearDriver{.25};
	d.Keyframes = {key};
	d.Tracks = {{"mirror", "center", "hold"}};
	SolidPixel(Sample(d, 1), 80, 40);
	SolidPixel(Sample(d, 2), 120, 40);
	d.Nodes.back().SourceAnimatedInputs.clear();
	d.Nodes.back().SourceStaticInputs = {"center"};
	SolidPixel(Sample(d, 2), 40, 40);
}

TEST_CASE(
	"Mirror bound group animator keeps shared key ownership separate from its getter", "[mirror_path]"
) {
	auto d = Graph();
	d.Nodes.back().SourceAnimatedInputs = {"center"};
	Node instance = d.Nodes.back();
	instance.Id = "instance";
	instance.InstanceBase = "mirror";
	instance.Values[0].Data = Vector2{.75, 0};
	instance.SourceAnimatedInputs.clear();
	instance.SourceStaticInputs = {"center"};
	d.Nodes.push_back(instance);
	d.Keyframes = {
		{"mirror", "center", 0, Vector2{.25, 0}, "linear"}, {"mirror", "center", 2, Vector2{.75, 0}, "linear"}
	};
	d.Outputs[0].NodeId = "instance";
	GroupReplayState empty, local, bound;
	Diagnostic diagnostic;
	REQUIRE(RebindGroupReplay(d, empty, 1, local, diagnostic) == Status::Ok);
	const std::array bindings{GroupSubtypeBinding{
		"instance", "mirror", GroupSubtypeAnimator::Animated, GroupSubtypeAnimator::Animated, "center"
	}};
	REQUIRE(BindGroupReplay(d, bindings, local, 1, bound, diagnostic) == Status::Ok);
	SolidPixel(Sample(d, 1, 0, &bound), 80, 40);
	SolidPixel(Sample(d, 0, 0, &bound), 40, 40);
}
TEST_CASE("Mirror lazy combined path always samples source line zero", "[mirror_path]") {
	auto d = Graph();
	Path2D combined;
	auto &operation = combined.SourceOperation.emplace();
	operation.Kind = SourcePathOperationKind::Combine;
	auto other = Line();
	for (auto &anchor : other.Anchors)
		anchor.Controls[1] += 4;
	operation.Inputs = {Line(), other};
	d.Junctions[0].Default = combined;
	SolidPixel(Sample(d), 40, 40);
	Document restored;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(d), restored, diagnostic) == Status::Ok);
	CHECK(restored == d);
	SolidPixel(Sample(restored), 40, 40);
}
TEST_CASE("Mirror path getter preadmits replacement views before growing them", "[mirror_path]") {
	auto d = Graph();
	EvaluationRequest request;
	detail::NodeContext context{d.Nodes.back(), *FindCatalogueEntry("pc.mirror_polar"), request};
	context.ByteBudget = 1;
	const Value path = Line();
	context.ValueViews.emplace_back("center", &path);
	const auto before = context.ValueViews;
	const auto capacity = context.ValueViews.capacity();
	detail::SourceMirrorPathProjection projection(context);
	CHECK_FALSE(projection.Prepare());
	CHECK(context.FailureCode == Status::LimitExceeded);
	CHECK(context.FailurePort == "source_path_getter");
	CHECK(context.ValueViews == before);
	CHECK(context.ValueViews.capacity() == capacity);
	CHECK_FALSE(context.MirrorPathSamples[3]);
}

TEST_CASE("Mirror separated X drives Any Path independently of dormant tuple and Y", "[mirror_axes]") {
	auto document = Graph();
	auto &node = document.Nodes.back();
	node.SourceStaticInputs = {"center"};
	node.SourceSeparatedVec2Animators.emplace().Inputs.push_back({"center", {}});
	auto &axes = node.SourceSeparatedVec2Animators->Inputs.front().Axes;
	axes[0].Keys = {{"mirror", "center", 0, .75, "source", KeyframeEase{}}};
	axes[1].Keys = {{"mirror", "center", 0, -.123, "source", KeyframeEase{}}};
	SolidPixel(Sample(document), 120, 40);
	axes[1].Keys[0].Data = 1000.;
	SolidPixel(Sample(document), 120, 40);
	Document restored;
	Diagnostic diagnostic;
	REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
	CHECK(restored == document);
	SolidPixel(Sample(restored), 120, 40);
}
TEST_CASE("Mirror separated animated axes seek while snapshots own physical Path samples", "[mirror_axes]") {
	auto document = Graph();
	auto &node = document.Nodes.back();
	node.SourceAnimatedInputs = {"center"};
	node.SourceSeparatedVec2Animators.emplace().Inputs.push_back({"center", {}});
	auto &axes = node.SourceSeparatedVec2Animators->Inputs.front().Axes;
	axes[0].Keys = {
		{"mirror", "center", 0, .25, "source", KeyframeEase{}},
		{"mirror", "center", 10, .75, "source", KeyframeEase{}}
	};
	axes[1].Keys = {
		{"mirror", "center", 0, .9, "source", KeyframeEase{}},
		{"mirror", "center", 10, .1, "source", KeyframeEase{}}
	};
	SolidPixel(Sample(document, 0), 40, 40);
	SolidPixel(Sample(document, 5), 80, 40);
	SolidPixel(Sample(document, 10), 120, 40);
	SolidPixel(Sample(document, 0), 40, 40);
	const auto source = Coordinates();
	const std::array sources{RequestImageSource{"source", source}};
	EvaluationRequest request;
	request.Tick = 5;
	request.ImageSources = sources;
	EvaluationSnapshot snapshot;
	Diagnostic diagnostic;
	REQUIRE(
		EvaluateNodeInputs(document, Compiled(document), "mirror", request, snapshot, diagnostic) ==
		Status::Ok
	);
	const auto found =
		std::find_if(snapshot.Values().begin(), snapshot.Values().end(), [](const auto &input) {
			return input.Port == "center";
		});
	REQUIRE(found != snapshot.Values().end());
	CHECK(std::get<Vector2>(found->Data) == Vector2{4, 2});
	REQUIRE(found->Domain);
	CHECK(found->Domain->Type == ValueType::Any);
	CHECK(found->Domain->Kind == SourceSocketKind::Any);
	document.Nodes.back().SourceSeparatedVec2Animators->Inputs[0].Axes[0].Keys[0].Data = .9;
	CHECK(std::get<Vector2>(found->Data) == Vector2{4, 2});
}
TEST_CASE("Mirror unlinked separated numeric axes retain Reference units", "[mirror_axes]") {
	auto document = Graph();
	document.Links.erase(document.Links.begin() + 1);
	auto &node = document.Nodes.back();
	node.SourceStaticInputs = {"center"};
	node.SourceSeparatedVec2Animators.emplace().Inputs.push_back({"center", {}});
	auto &axes = node.SourceSeparatedVec2Animators->Inputs.front().Axes;
	axes[0].Keys = {{"mirror", "center", 0, .25, "source", KeyframeEase{}}};
	axes[1].Keys = {{"mirror", "center", 0, .5, "source", KeyframeEase{}}};
	SolidPixel(Sample(document), 40, 80);
}
TEST_CASE(
	"Split GroupReplay edits share one original writer and project both axes without changing dormant "
	"storage",
	"[mirror_axes]"
) {
	auto d = Graph();
	auto &original = d.Nodes.back();
	original.SourceStaticInputs = {"center"};
	original.SourceSeparatedVec2Animators.emplace().Inputs.push_back({"center", {}});
	auto &axes = original.SourceSeparatedVec2Animators->Inputs[0].Axes;
	axes[0].Keys = {
		{"mirror", "center", 0, .25, "source", KeyframeEase{}},
		{"mirror", "center", 10, .5, "source", KeyframeEase{}}
	};
	axes[1].Keys = {{"mirror", "center", 0, 8., "source", KeyframeEase{}}};
	d.Keyframes = {{"mirror", "center", 0, Vector2{.123, .456}, "source", KeyframeEase{}}};
	d.Tracks = {{"mirror", "center", "hold"}};
	const Node prototype = original;
	for (const char *id : {"copy", "sibling"}) {
		Node copy = prototype;
		copy.Id = id;
		copy.InstanceBase = "mirror";
		for (auto &input : copy.SourceSeparatedVec2Animators->Inputs)
			for (auto &axis : input.Axes)
				for (auto &key : axis.Keys)
					key.NodeId = id;
		d.Nodes.push_back(std::move(copy));
	}
	d.Outputs[0].NodeId = "copy";
	GroupReplayState empty, local, bound;
	Diagnostic diagnostic;
	REQUIRE(RebindGroupReplay(d, empty, 1, local, diagnostic) == Status::Ok);
	const std::array bindings{
		GroupSubtypeBinding{
			"copy", "mirror", GroupSubtypeAnimator::Static, GroupSubtypeAnimator::Static, "center"
		},
		GroupSubtypeBinding{
			"sibling", "mirror", GroupSubtypeAnimator::Static, GroupSubtypeAnimator::Static, "center"
		}
	};
	REQUIRE(BindGroupReplay(d, bindings, local, 1, bound, diagnostic) == Status::Ok);
	SolidPixel(Sample(d, 10, 0, &bound), 40, 40);
	const Value edited = Vector2{.75, -100.};
	GroupRefreshEvent event;
	event.NodeId = "copy";
	event.EditedPort = "center";
	event.LocalValue = &edited;
	event.LocalAnimated = true;
	event.At.Tick = 5;
	GroupReplayState next;
	REQUIRE(ReplayGroupAnimatorEdits(d, {&event, 1}, bound, 1, next, diagnostic) == Status::Ok);
	REQUIRE(next.SharedSubtypes().size() == 1);
	const auto *shared = next.SharedSubtype("mirror", "center");
	REQUIRE(shared);
	REQUIRE(shared->SeparatedVec2);
	CHECK(shared->SeparatedVec2->Axes[0].Keys.size() == 2);
	CHECK(shared->SeparatedVec2->Axes[0].Keys[0].Data == Value{.75});
	CHECK(shared->SeparatedVec2->Axes[0].Keys[1].Data == Value{.5});
	CHECK(shared->SeparatedVec2->Axes[1].Keys[0].Data == Value{-100.});
	SolidPixel(Sample(d, 10, 0, &next), 120, 40);
	d.Outputs[0].NodeId = "sibling";
	SolidPixel(Sample(d, 10, 0, &next), 120, 40);
	Document projected;
	REQUIRE(ProjectGroupReplay(d, next, 1, projected, diagnostic) == Status::Ok);
	CHECK(projected.Keyframes == d.Keyframes);
	CHECK(projected.Nodes[2].Values == d.Nodes[2].Values);
	CHECK(projected.Nodes[2].SourceSeparatedVec2Animators->Inputs[0] == *shared->SeparatedVec2);
	GroupReplayState rebound;
	REQUIRE(RebindProjectedGroupReplay(projected, next, 1, rebound, diagnostic) == Status::Ok);
	CHECK(rebound.SharedSubtypes().empty());
	SolidPixel(Sample(projected, 10, 0, &rebound), 120, 40);
	Document restored;
	REQUIRE(Read(Write(projected), restored, diagnostic) == Status::Ok);
	CHECK(restored == projected);
	SolidPixel(Sample(restored, 10, 0, &rebound), 120, 40);
	Document dormantReplacement = d;
	dormantReplacement.Keyframes[0].Data = Vector2{.01, .02};
	const SourceAnimatorReplacement target{"mirror", "center"};
	GroupReplayState kept;
	REQUIRE(
		RebindGroupReplayWithAnimatorReplacements(
			dormantReplacement, {&target, 1}, next, 1, kept, diagnostic
		) == Status::Ok
	);
	REQUIRE(kept.SharedSubtype("mirror", "center"));
	REQUIRE(kept.SharedSubtype("mirror", "center")->SeparatedVec2);
	CHECK(*kept.SharedSubtype("mirror", "center")->SeparatedVec2 == *shared->SeparatedVec2);
	SolidPixel(Sample(dormantReplacement, 10, 0, &kept), 120, 40);
	Document keptProjection;
	REQUIRE(ProjectGroupReplay(dormantReplacement, kept, 1, keptProjection, diagnostic) == Status::Ok);
	CHECK(keptProjection.Keyframes == dormantReplacement.Keyframes);
	CHECK(keptProjection.Nodes[2].SourceSeparatedVec2Animators->Inputs[0] == *shared->SeparatedVec2);
	const auto priorBytes = next.RetainedBytes();
	CHECK(ReplayGroupAnimatorEdits(d, {&event, 1}, next, 1, next, diagnostic, 1) == Status::LimitExceeded);
	CHECK(next.RetainedBytes() == priorBytes);
	SolidPixel(Sample(d, 10, 0, &next), 120, 40);
}
TEST_CASE(
	"Split aliases keep independent getter flags and shared static lone-driver ownership", "[mirror_axes]"
) {
	auto d = Graph();
	auto &node = d.Nodes.back();
	node.SourceStaticInputs = {"center"};
	node.SourceSeparatedVec2Animators.emplace().Inputs.push_back({"center", {}});
	auto &axes = node.SourceSeparatedVec2Animators->Inputs[0].Axes;
	axes[0].Keys = {{"mirror", "center", 0, .25, "source", KeyframeEase{}}};
	axes[0].Keys[0].SourceDriver = KeyframeLinearDriver{.1};
	axes[1].Keys = {{"mirror", "center", 0, 0., "source", KeyframeEase{}}};
	Node copy = node;
	copy.Id = "copy";
	copy.InstanceBase = "mirror";
	copy.SourceSeparatedVec2Animators = {};
	d.Nodes.push_back(std::move(copy));
	d.Outputs[0].NodeId = "copy";
	GroupReplayState empty, local, bound;
	Diagnostic diagnostic;
	REQUIRE(RebindGroupReplay(d, empty, 1, local, diagnostic) == Status::Ok);
	const std::array bindings{GroupSubtypeBinding{
		"copy", "mirror", GroupSubtypeAnimator::Animated, GroupSubtypeAnimator::Static, "center"
	}};
	REQUIRE(BindGroupReplay(d, bindings, local, 1, bound, diagnostic) == Status::Ok);
	SolidPixel(Sample(d, 5, 0, &bound), 120, 40);
	SolidPixel(Sample(d, 0, 0, &bound), 40, 40);
	d.Outputs[0].NodeId = "mirror";
	SolidPixel(Sample(d, 5, 0, &bound), 40, 40);
}

TEST_CASE(
	"Split Mirror feedback owns both axes through selected-output replay and backseek", "[mirror_axes]"
) {
	auto d = Graph();
	d.Nodes[1].Values[0].Data = std::string("feedback:out");
	d.Nodes.back().SourceAnimatedInputs = {"center"};
	d.Nodes.back().SourceSeparatedVec2Animators.emplace().Inputs.push_back({"center", {}});
	auto &axes = d.Nodes.back().SourceSeparatedVec2Animators->Inputs[0].Axes;
	axes[0].Keys = {
		{"mirror", "center", 0, .25, "source", KeyframeEase{}},
		{"mirror", "center", 2, .75, "source", KeyframeEase{}}
	};
	axes[1].Keys = {{"mirror", "center", 0, -99., "source", KeyframeEase{}}};
	d.Tracks = {{"mirror", "center", "hold"}};
	const auto p = Compiled(d);
	CapturedFeedbackHost host;
	Diagnostic diagnostic;
	const auto center = [&]() -> const EvaluationInputValue & {
		const auto values = host.Snapshot().Values();
		const auto found =
			std::find_if(values.begin(), values.end(), [](const auto &v) { return v.Port == "center"; });
		REQUIRE(found != values.end());
		return *found;
	};
	EvaluationRequest request;
	REQUIRE(host.PrepareNodeInputs(d, p, 1, 0, "mirror", request, diagnostic));
	CHECK(center().Data == Value{Vector2{2, 2}});
	request = {};
	request.Tick = 1;
	REQUIRE(host.PrepareNodeInputs(d, p, 1, 0, "mirror", request, diagnostic));
	CHECK(center().Data == Value{Vector2{4, 2}});
	const auto before = center();
	const auto bytes = host.Snapshot().RetainedBytes();
	request = {};
	request.Tick = 2;
	CHECK_FALSE(host.PrepareNodeInputs(d, p, 1, 0, "mirror", request, diagnostic, 1));
	CHECK(center().Data == before.Data);
	CHECK(center().Linked == before.Linked);
	CHECK(center().Domain == before.Domain);
	CHECK(host.Snapshot().RetainedBytes() == bytes);
	request = {};
	REQUIRE(host.PrepareNodeInputs(d, p, 1, 0, "mirror", request, diagnostic));
	CHECK(center().Data == Value{Vector2{2, 2}});
}

TEST_CASE(
	"Animated split edits canonicalize stored duplicate times in the bounded native profile", "[mirror_axes]"
) {
	auto d = Graph();
	auto &node = d.Nodes.back();
	node.SourceAnimatedInputs = {"center"};
	node.SourceSeparatedVec2Animators.emplace().Inputs.push_back({"center", {}});
	auto &axes = node.SourceSeparatedVec2Animators->Inputs[0].Axes;
	axes[0].Keys = {
		{"mirror", "center", 0, .25, "source", KeyframeEase{}},
		{"mirror", "center", 0, .9, "source", KeyframeEase{}}
	};
	axes[0].Keys[0].SourceKeyId = "first-x";
	axes[0].Keys[1].SourceKeyId = "second-x";
	axes[1].Keys = {{"mirror", "center", 0, 8., "source", KeyframeEase{}}};
	d.Tracks = {{"mirror", "center", "hold"}};
	GroupReplayState empty, local, prior, next;
	Diagnostic diagnostic;
	REQUIRE(RebindGroupReplay(d, empty, 1, local, diagnostic) == Status::Ok);
	REQUIRE(BindGroupReplay(d, {}, local, 1, prior, diagnostic) == Status::Ok);
	REQUIRE(prior.InstancesBound());
	const Value edited = Vector2{.75, -99.};
	GroupRefreshEvent event;
	event.NodeId = "mirror";
	event.EditedPort = "center";
	event.LocalAnimated = true;
	event.LocalValue = &edited;
	event.At.Tick = 5;
	REQUIRE(ReplayGroupAnimatorEdits(d, {&event, 1}, prior, 1, next, diagnostic) == Status::Ok);
	const auto *shared = next.SharedSubtype("mirror", "center");
	REQUIRE(shared);
	REQUIRE(shared->SeparatedVec2);
	const auto &keys = shared->SeparatedVec2->Axes[0].Keys;
	REQUIRE(keys.size() == 2);
	CHECK(keys[0].SourceKeyId == "first-x");
	CHECK(keys[0].Data == Value{.25});
	CHECK(keys[1].Tick == 5);
	CHECK(keys[1].Data == Value{.75});
	SolidPixel(Sample(d, 0, 0, &next), 40, 40);
	SolidPixel(Sample(d, 5, 0, &next), 120, 40);
	Document projected;
	REQUIRE(ProjectGroupReplay(d, next, 1, projected, diagnostic) == Status::Ok);
	Document restored;
	REQUIRE(Read(Write(projected), restored, diagnostic) == Status::Ok);
	CHECK(restored == projected);
}

TEST_CASE("Inactive axes survive shared group edits and native reopen", "[mirror_axes]") {
	auto document = Graph();
	auto &original = document.Nodes.back();
	original.SourceStaticInputs = {"center"};
	original.SourceSeparatedVec2Animators.emplace().Inputs.push_back({"center", {}, false});
	auto &axes = original.SourceSeparatedVec2Animators->Inputs[0].Axes;
	axes[0].Keys = {{"mirror", "center", 0, .9, "source", KeyframeEase{}}};
	axes[1].Keys = {{"mirror", "center", 0, 8., "source", KeyframeEase{}}};
	document.Keyframes = {{"mirror", "center", 0, Vector2{.25, .5}, "source", KeyframeEase{}}};
	document.Tracks = {{"mirror", "center", "hold"}};
	Node alias = original;
	alias.Id = "copy";
	alias.InstanceBase = "mirror";
	alias.SourceSeparatedVec2Animators = {};
	document.Nodes.push_back(std::move(alias));
	document.Outputs[0].NodeId = "copy";
	Diagnostic diagnostic;
	Document restored;
	REQUIRE(Read(Write(document), restored, diagnostic) == Status::Ok);
	GroupReplayState empty, local, bound;
	REQUIRE(RebindGroupReplay(restored, empty, 1, local, diagnostic) == Status::Ok);
	const std::array bindings{GroupSubtypeBinding{
		"copy", "mirror", GroupSubtypeAnimator::Static, GroupSubtypeAnimator::Static, "center"
	}};
	REQUIRE(BindGroupReplay(restored, bindings, local, 1, bound, diagnostic) == Status::Ok);
	SolidPixel(Sample(restored, 10, 0, &bound), 40, 40);
	const Value edited = Vector2{.75, .5};
	GroupRefreshEvent event;
	event.NodeId = "copy";
	event.EditedPort = "center";
	event.LocalValue = &edited;
	event.At.Tick = 5;
	GroupReplayState next;
	REQUIRE(ReplayGroupAnimatorEdits(restored, {&event, 1}, bound, 1, next, diagnostic) == Status::Ok);
	SolidPixel(Sample(restored, 10, 0, &next), 120, 40);
	Document projected;
	REQUIRE(ProjectGroupReplay(restored, next, 1, projected, diagnostic) == Status::Ok);
	CHECK(projected.Nodes[2].SourceSeparatedVec2Animators == restored.Nodes[2].SourceSeparatedVec2Animators);
	CHECK(projected.Keyframes != restored.Keyframes);
	GroupReplayState rebound;
	REQUIRE(RebindProjectedGroupReplay(projected, next, 1, rebound, diagnostic) == Status::Ok);
	CHECK(rebound.SharedSubtypes().empty());
	SolidPixel(Sample(projected, 10, 0, &rebound), 120, 40);
	Document reopened;
	REQUIRE(Read(Write(projected), reopened, diagnostic) == Status::Ok);
	CHECK(reopened == projected);
	SolidPixel(Sample(reopened, 10, 0, &rebound), 120, 40);
}
