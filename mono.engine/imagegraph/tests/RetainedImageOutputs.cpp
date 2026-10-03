#include <engine/imagegraph/Document.hpp>
#include <engine/imagegraph/FeedbackHost.hpp>
#include <engine/imagegraph/StatefulReplay.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
TEST_SUITE_ID("engine.imagegraph.retained_image_outputs")
using namespace engine::imagegraph;
namespace {
	Document SmearGraph() {
		Document d;
		d.FormatVersion = 9;
		d.Nodes = {
			{"source", "image.captured", "", {}, {{"source_id", std::string("source")}}},
			{"smear", "pc.smear", "", {}, {{"invert", true}, {"strength", 0.}}}
		};
		d.Links = {{"source", "image", "smear", "surface_in"}};
		d.Outputs = {{"colour", "smear", "surface_out"}, {"depth", "smear", "depth_pass"}};
		return d;
	}
	Image Solid(uint32_t w, uint32_t h, std::array<uint8_t, 4> color) {
		Image image{w, h, std::vector<uint8_t>(size_t(w) * h * 4), 0};
		for (size_t i = 0; i < image.Pixels.size(); ++i)
			image.Pixels[i] = color[i % 4];
		return image;
	}
	void Set(Node &node, std::string port, Value value) {
		for (auto &v : node.Values)
			if (v.Port == port) {
				v.Data = std::move(value);
				return;
			}
		node.Values.push_back({std::move(port), std::move(value)});
	}
	Plan Compiled(const Document &d) {
		Plan p;
		Diagnostic diag;
		const auto s = Compile(d, p, diag);
		INFO(diag.Message);
		REQUIRE(s == Status::Ok);
		return p;
	}
	StatefulOutputEvaluationResult
	Run(const Document &d,
		EvaluationRequest request,
		std::span<const std::string> outputs,
		uint64_t bytes = Limits::MaximumEvaluationBytes) {
		auto p = Compiled(d);
		StatefulOutputEvaluationResult result;
		Diagnostic diag;
		const auto s = EvaluateStatefulOutputs(d, p, outputs, request, result, diag, bytes);
		INFO(diag.Message);
		REQUIRE(s == Status::Ok);
		return result;
	}
	const Image &PixelOutput(const StatefulOutputEvaluationResult &r, std::string_view id) {
		for (const auto &o : r.Outputs)
			if (o.Id == id) return std::get<Image>(o.Output);
		throw std::runtime_error("missing image");
	}
	const ImageArray &ArrayOutput(const StatefulOutputEvaluationResult &r, std::string_view id) {
		for (const auto &o : r.Outputs)
			if (o.Id == id) return std::get<ImageArray>(o.Output);
		throw std::runtime_error("missing array");
	}
}
TEST_CASE("Smear inactive color changes while a copied previous depth remains owned", "[retained_images]") {
	auto d = SmearGraph();
	std::array images{RequestImageSource{"source", Solid(2, 1, {255, 255, 255, 255})}};
	EvaluationRequest request;
	request.ImageSources = images;
	const std::array<std::string, 2> outputs{"colour", "depth"};
	const auto first = Run(d, request, outputs);
	REQUIRE(first.Data.Entries.size() == 1);
	Diagnostic validDiagnostic;
	CHECK(ValidateDataReplay(first.Data, Limits::MaximumEvaluationBytes, validDiagnostic) == Status::Ok);
	Set(d.Nodes[1], "active", false);
	images[0].Data = Solid(3, 2, {32, 64, 128, 192});
	request.Tick = 1;
	request.DataReplay = &first.Data;
	const auto second = Run(d, request, outputs);
	CHECK(PixelOutput(second, "colour").Pixels == images[0].Data.Pixels);
	CHECK(PixelOutput(second, "depth").Pixels == PixelOutput(first, "depth").Pixels);
	CHECK(PixelOutput(second, "depth").Width == 2);
	CHECK(first.Data == second.Data);
}
TEST_CASE(
	"Smear inactive whole depth array retains its prior count despite changed input rows", "[retained_images]"
) {
	auto d = SmearGraph();
	d.Nodes.push_back({"second", "image.captured", "", {}, {{"source_id", std::string("second")}}});
	Node array{"array", "value.array", "", {}, {{"spread", false}}};
	array.DynamicInputs = {{"a", ValueType::Image, std::nullopt}, {"b", ValueType::Image, std::nullopt}};
	d.Nodes.push_back(array);
	d.Links.erase(d.Links.begin());
	d.Links.insert(
		d.Links.end(),
		{{"source", "image", "array", "a"},
		 {"second", "image", "array", "b"},
		 {"array", "array", "smear", "surface_in"}}
	);
	std::array images{
		RequestImageSource{"source", Solid(2, 1, {255, 255, 255, 255})},
		RequestImageSource{"second", Solid(4, 2, {255, 255, 255, 255})}
	};
	EvaluationRequest request;
	request.ImageSources = images;
	const std::array<std::string, 2> outputs{"colour", "depth"};
	const auto first = Run(d, request, outputs);
	REQUIRE(ArrayOutput(first, "depth").Images.size() == 2);
	Set(d.Nodes[1], "active", false);
	d.Links.erase(
		std::remove_if(
			d.Links.begin(),
			d.Links.end(),
			[](const auto &link) { return link.ToNode == "smear" && link.ToPort == "surface_in"; }
		),
		d.Links.end()
	);
	d.Links.push_back({"source", "image", "smear", "surface_in"});
	images[0].Data = Solid(1, 3, {64, 128, 192, 255});
	request.Tick = 1;
	request.DataReplay = &first.Data;
	const auto second = Run(d, request, outputs);
	CHECK(PixelOutput(second, "colour").Height == 3);
	CHECK(ArrayOutput(second, "depth").Images == ArrayOutput(first, "depth").Images);
	CHECK(ArrayOutput(second, "depth").Items == ArrayOutput(first, "depth").Items);
}
TEST_CASE(
	"Smear active undefined depth invalidates metadata rather than retaining an older defined target",
	"[retained_images]"
) {
	auto d = SmearGraph();
	std::array images{RequestImageSource{"source", Solid(2, 1, {255, 255, 255, 255})}};
	EvaluationRequest request;
	request.ImageSources = images;
	const std::array<std::string, 1> colour{"colour"};
	const auto first = Run(d, request, colour);
	Set(d.Nodes[1], "invert", false);
	request.Tick = 1;
	request.DataReplay = &first.Data;
	const auto undefined = Run(d, request, colour);
	Set(d.Nodes[1], "active", false);
	request.Tick = 2;
	request.DataReplay = &undefined.Data;
	auto p = Compiled(d);
	StatefulEvaluationResult result;
	Diagnostic diag;
	CHECK(EvaluateStateful(d, p, "depth", request, result, diag) == Status::UnsupportedExecution);
	CHECK(diag.Message.find("previous active") != std::string::npos);
	request.DataReplay = nullptr;
	CHECK(EvaluateStateful(d, p, "depth", request, result, diag) == Status::UnsupportedExecution);
	CHECK(diag.Message.find("fresh inactive") != std::string::npos);
}
TEST_CASE("Smear retained metadata clone refusal preserves old output and journal", "[retained_images]") {
	auto d = SmearGraph();
	std::array images{RequestImageSource{"source", Solid(32, 16, {255, 255, 255, 255})}};
	EvaluationRequest request;
	request.ImageSources = images;
	const std::array<std::string, 1> colour{"colour"};
	auto result = Run(d, request, colour);
	const auto prior = result;
	request.Tick = 1;
	request.DataReplay = &prior.Data;
	auto p = Compiled(d);
	Diagnostic diag;
	CHECK(
		EvaluateStatefulOutputs(
			d, p, colour, request, result, diag, RetainedStatefulOutputBytes(prior) + 1
		) == Status::LimitExceeded
	);
	CHECK(result.Data == prior.Data);
	CHECK(std::get<Image>(result.Outputs[0].Output) == std::get<Image>(prior.Outputs[0].Output));
}
namespace {
	Document CropGraph() {
		Document d = SmearGraph();
		d.Nodes[1] = {"crop", "pc.crop_content", "", {}, {}};
		d.Links = {{"source", "image", "crop", "surface_in"}};
		d.Outputs = {
			{"colour", "crop", "surface_out"},
			{"distance", "crop", "crop_distance"},
			{"atlas", "crop", "atlas"}
		};
		return d;
	}
	const Value &TypedOutput(const StatefulOutputEvaluationResult &r, std::string_view id) {
		for (const auto &out : r.Outputs)
			if (out.Id == id) return std::get<EvaluatedValue>(out.Output).Data;
		throw std::runtime_error("missing typed output");
	}
	Image Content(uint32_t w, uint32_t h, uint32_t x, uint32_t y) {
		auto image = Solid(w, h, {0, 0, 0, 0});
		const size_t start = (size_t(y) * w + x) * 4;
		image.Pixels[start] = 255;
		image.Pixels[start + 3] = 255;
		return image;
	}
}
TEST_CASE(
	"Crop source pixel bounds and padding retain original content and Atlas placement",
	"[retained_images][crop_content]"
) {
	auto d = CropGraph();
	Set(d.Nodes[1], "padding", Vector4{2, 1, 3, 4});
	std::array images{RequestImageSource{"source", Content(5, 4, 2, 1)}};
	EvaluationRequest request;
	request.ImageSources = images;
	const std::array<std::string, 3> outputs{"colour", "distance", "atlas"};
	const auto result = Run(d, request, outputs);
	const auto &out = PixelOutput(result, "colour");
	CHECK(out.Width == 6);
	CHECK(out.Height == 6);
	CHECK(out.Pixels[(size_t(1) * 6 + 3) * 4] == 255);
	CHECK(std::get<Vector4>(TypedOutput(result, "distance")) == Vector4{2, 1, 2, 2});
	const auto &atlas = std::get<AtlasValue>(TypedOutput(result, "atlas"));
	REQUIRE(atlas.Data);
	CHECK(atlas.Data->Kind == AtlasKind::SurfaceAtlas);
	CHECK(atlas.Data->Position == Vector2{2, 1});
	CHECK(atlas.Data->Dimension == Vector2{6, 6});
	CHECK(atlas.Data->OriginalDimension == Vector2{6, 6});
	CHECK(atlas.Data->Surface.Data.Pixels == out.Pixels);
}
TEST_CASE(
	"Crop small empty and large empty shortcut retain distinct source distances",
	"[retained_images][crop_content]"
) {
	auto d = CropGraph();
	std::array images{RequestImageSource{"source", Solid(8, 5, {0, 0, 0, 0})}};
	EvaluationRequest request;
	request.ImageSources = images;
	const std::array<std::string, 2> outputs{"colour", "distance"};
	auto result = Run(d, request, outputs);
	CHECK(PixelOutput(result, "colour").Width == 1);
	CHECK(PixelOutput(result, "colour").Height == 1);
	CHECK(std::get<Vector4>(TypedOutput(result, "distance")) == Vector4{8, 0, 0, 5});
	images[0].Data = Solid(65, 8, {0, 0, 0, 0});
	result = Run(d, request, outputs);
	CHECK(PixelOutput(result, "colour").Width == 1);
	CHECK(std::get<Vector4>(TypedOutput(result, "distance")) == Vector4{64, 0, 0, 7});
}
TEST_CASE(
	"Crop background removal changes only bounding readback rather than cropped pixels",
	"[retained_images][crop_content]"
) {
	auto d = CropGraph();
	Set(d.Nodes[1], "background", Colour{0, 255, 0, 255});
	std::array images{RequestImageSource{"source", Solid(4, 1, {0, 255, 0, 255})}};
	images[0].Data.Pixels[0] = 255;
	images[0].Data.Pixels[1] = 0;
	images[0].Data.Pixels[8] = 255;
	images[0].Data.Pixels[9] = 0;
	EvaluationRequest request;
	request.ImageSources = images;
	const std::array<std::string, 2> outputs{"colour", "distance"};
	const auto result = Run(d, request, outputs);
	CHECK(PixelOutput(result, "colour").Width == 3);
	CHECK(
		PixelOutput(result, "colour").Pixels ==
		std::vector<uint8_t>{255, 0, 0, 255, 0, 255, 0, 255, 255, 0, 0, 255}
	);
	CHECK(std::get<Vector4>(TypedOutput(result, "distance")) == Vector4{1, 0, 0, 0});
}
TEST_CASE(
	"Crop independent and union image arrays publish actual typed metadata arrays",
	"[retained_images][crop_content]"
) {
	auto d = CropGraph();
	d.Nodes.push_back({"second", "image.captured", "", {}, {{"source_id", std::string("second")}}});
	Node array{"array", "value.array", "", {}, {{"spread", false}}};
	array.DynamicInputs = {{"a", ValueType::Image, std::nullopt}, {"b", ValueType::Image, std::nullopt}};
	d.Nodes.push_back(array);
	d.Links = {
		{"source", "image", "array", "a"},
		{"second", "image", "array", "b"},
		{"array", "array", "crop", "surface_in"}
	};
	std::array images{
		RequestImageSource{"source", Content(4, 3, 0, 0)}, RequestImageSource{"second", Content(6, 4, 4, 2)}
	};
	EvaluationRequest request;
	request.ImageSources = images;
	const std::array<std::string, 3> outputs{"colour", "distance", "atlas"};
	const auto independent = Run(d, request, outputs);
	REQUIRE(ArrayOutput(independent, "colour").Images.size() == 2);
	CHECK(ArrayOutput(independent, "colour").Images[0].Width == 1);
	const auto &distance = std::get<ArrayValue>(TypedOutput(independent, "distance"));
	REQUIRE(distance.Elements.size() == 2);
	CHECK(std::get<Vector4>(distance.Elements[1]) == Vector4{1, 2, 4, 1});
	CHECK(std::get<ArrayValue>(TypedOutput(independent, "atlas")).ElementType == ValueType::Atlas);
	Set(d.Nodes[1], "array_sizing", EnumValue{0});
	const auto united = Run(d, request, outputs);
	CHECK(ArrayOutput(united, "colour").Images[0].Width == 5);
	CHECK(ArrayOutput(united, "colour").Images[1].Height == 3);
	CHECK(
		std::get<Vector4>(std::get<ArrayValue>(TypedOutput(united, "distance")).Elements[0]) ==
		Vector4{-1, 0, 0, 0}
	);
	Set(d.Nodes[1], "active", false);
	d.Links.back() = {"source", "image", "crop", "surface_in"};
	request.Tick = 1;
	request.DataReplay = &united.Data;
	const auto inactive = Run(d, request, outputs);
	CHECK(PixelOutput(inactive, "colour").Width == 4);
	CHECK(TypedOutput(inactive, "distance") == TypedOutput(united, "distance"));
	CHECK(TypedOutput(inactive, "atlas") == TypedOutput(united, "atlas"));
}
TEST_CASE(
	"Crop fresh inactive constructor metadata differs from retained active metadata",
	"[retained_images][crop_content]"
) {
	auto d = CropGraph();
	Set(d.Nodes[1], "active", false);
	std::array images{RequestImageSource{"source", Content(4, 3, 2, 1)}};
	EvaluationRequest request;
	request.ImageSources = images;
	const std::array<std::string, 3> outputs{"colour", "distance", "atlas"};
	const auto result = Run(d, request, outputs);
	CHECK(PixelOutput(result, "colour").Pixels == images[0].Data.Pixels);
	CHECK(std::get<Vector4>(TypedOutput(result, "distance")) == Vector4{});
	CHECK(std::get<ArrayValue>(TypedOutput(result, "atlas")).Elements.empty());
	CHECK(result.Data.Entries.empty());
}
TEST_CASE("Retained Smear host fixed seeks and reset reconstruct frame zero metadata", "[retained_images]") {
	auto d = SmearGraph();
	d.Keyframes = {{"smear", "active", 0, true, "step"}, {"smear", "active", 1, false, "step"}};
	std::array images{RequestImageSource{"source", Solid(2, 1, {255, 255, 255, 255})}};
	auto p = Compiled(d);
	CapturedFeedbackHost host;
	EvaluationRequest request;
	request.ImageSources = images;
	Diagnostic diag;
	request.Tick = 12;
	INFO(diag.Message);
	REQUIRE(host.Prepare(d, p, 1, 0, request, diag, Limits::MaximumEvaluationBytes, "depth"));
	const auto depth12 = *host.Output("depth");
	request.Tick = 0;
	request.DataReplay = nullptr;
	REQUIRE(host.Prepare(d, p, 1, 0, request, diag, Limits::MaximumEvaluationBytes, "depth"));
	CHECK(*host.Output("depth") == depth12);
	host.Clear();
	request.Tick = 12;
	request.DataReplay = nullptr;
	REQUIRE(host.Prepare(d, p, 1, 0, request, diag, Limits::MaximumEvaluationBytes, "depth"));
	CHECK(*host.Output("depth") == depth12);
}
TEST_CASE(
	"Crop retained metadata rejects a future or malformed record atomically",
	"[retained_images][crop_content]"
) {
	auto d = CropGraph();
	std::array images{RequestImageSource{"source", Content(4, 3, 2, 1)}};
	EvaluationRequest request;
	request.ImageSources = images;
	const std::array<std::string, 3> outputs{"colour", "distance", "atlas"};
	const auto active = Run(d, request, outputs);
	Set(d.Nodes[1], "active", false);
	auto p = Compiled(d);
	auto result = active;
	auto malformed = active.Data;
	malformed.Entries[0].Values[0].Frame = 2;
	request.Tick = 1;
	request.DataReplay = &malformed;
	Diagnostic diag;
	CHECK(EvaluateStatefulOutputs(d, p, outputs, request, result, diag) == Status::InvalidValue);
	CHECK(result.Data == active.Data);
	CHECK(result.Outputs.size() == active.Outputs.size());
	CHECK(TypedOutput(result, "atlas") == TypedOutput(active, "atlas"));
	malformed = active.Data;
	std::get<StructValue>(malformed.Entries[0].Values[0].Data).Data->Fields[0].first = "wrong_field";
	CHECK(EvaluateStatefulOutputs(d, p, outputs, request, result, diag) == Status::InvalidValue);
	CHECK(result.Data == active.Data);
	CHECK(PixelOutput(result, "colour") == PixelOutput(active, "colour"));
}
TEST_CASE(
	"Crop host seek and authored reset preserve actual Atlas placement rather than current pixels",
	"[retained_images][crop_content]"
) {
	auto d = CropGraph();
	d.Keyframes = {{"crop", "active", 0, true, "step"}, {"crop", "active", 1, false, "step"}};
	std::array images{RequestImageSource{"source", Content(4, 3, 2, 1)}};
	EvaluationRequest request;
	request.ImageSources = images;
	request.Tick = 12;
	Diagnostic diag;
	CapturedFeedbackHost host;
	auto p = Compiled(d);
	REQUIRE(host.Prepare(d, p, 1, 0, request, diag, Limits::MaximumEvaluationBytes, "atlas"));
	const auto original = std::get<EvaluatedValue>(host.Value("atlas")->Output).Data;
	request.Tick = 0;
	REQUIRE(host.Prepare(d, p, 1, 0, request, diag, Limits::MaximumEvaluationBytes, "atlas"));
	CHECK(std::get<EvaluatedValue>(host.Value("atlas")->Output).Data == original);
	images[0].Data = Content(4, 3, 0, 2);
	request.ImageSources = images;
	request.Tick = 12;
	REQUIRE(host.Prepare(d, p, 2, 1, request, diag, Limits::MaximumEvaluationBytes, "atlas"));
	const auto &atlas = std::get<AtlasValue>(std::get<EvaluatedValue>(host.Value("atlas")->Output).Data);
	CHECK(atlas.Data->Position == Vector2{0, 2});
	CHECK(std::get<EvaluatedValue>(host.Value("atlas")->Output).Data != original);
}
TEST_CASE(
	"Crop owned Atlas clone budget includes old journal and replacement images",
	"[retained_images][crop_content]"
) {
	auto d = CropGraph();
	std::array images{RequestImageSource{"source", Solid(32, 16, {255, 0, 0, 255})}};
	EvaluationRequest request;
	request.ImageSources = images;
	const std::array<std::string, 3> outputs{"colour", "distance", "atlas"};
	auto result = Run(d, request, outputs);
	const auto prior = result;
	auto p = Compiled(d);
	request.Tick = 1;
	request.DataReplay = &prior.Data;
	Diagnostic diag;
	const uint64_t oldBytes = RetainedStatefulOutputBytes(prior) + RetainedDataReplayBytes(prior.Data);
	CHECK(
		EvaluateStatefulOutputs(d, p, outputs, request, result, diag, oldBytes + 1) == Status::LimitExceeded
	);
	CHECK(result.Data == prior.Data);
	CHECK(TypedOutput(result, "atlas") == TypedOutput(prior, "atlas"));
	CHECK(PixelOutput(result, "colour") == PixelOutput(prior, "colour"));
}
TEST_CASE(
	"Crop source array admits complete pixel work before any cropped output",
	"[retained_images][crop_content]"
) {
	auto d = CropGraph();
	d.Nodes.push_back({"second", "image.captured", "", {}, {{"source_id", std::string("second")}}});
	Node array{"array", "value.array", "", {}, {{"spread", false}}};
	array.DynamicInputs = {{"a", ValueType::Image, std::nullopt}, {"b", ValueType::Image, std::nullopt}};
	d.Nodes.push_back(array);
	d.Links = {
		{"source", "image", "array", "a"},
		{"second", "image", "array", "b"},
		{"array", "array", "crop", "surface_in"}
	};
	std::array images{
		RequestImageSource{"source", Content(1, 1, 0, 0)},
		RequestImageSource{"second", Solid(16, 16, {255, 0, 0, 255})}
	};
	EvaluationRequest request;
	request.ImageSources = images;
	const std::array<std::string, 1> outputs{"colour"};
	auto result = Run(d, request, outputs);
	const auto prior = result;
	Set(d.Nodes[1], "padding", Vector4{2000, 2000, 2000, 2000});
	images[1].Data = Solid(1856, 1856, {0, 0, 0, 0});
	auto p = Compiled(d);
	Diagnostic diag;
	CHECK(EvaluateStatefulOutputs(d, p, outputs, request, result, diag) == Status::LimitExceeded);
	CHECK(diag.Message.find("complete source array exceeds work") != std::string::npos);
	CHECK(result.Data == prior.Data);
	CHECK(ArrayOutput(result, "colour").Images == ArrayOutput(prior, "colour").Images);
}
TEST_CASE(
	"Crop retained typed record cap includes Atlas pixels and record metadata before cloning",
	"[retained_images][crop_content]"
) {
	auto d = CropGraph();
	std::array images{RequestImageSource{"source", Solid(2, 2, {255, 0, 0, 255})}};
	EvaluationRequest request;
	request.ImageSources = images;
	const std::array<std::string, 1> outputs{"colour"};
	auto result = Run(d, request, outputs);
	const auto prior = result;
	images[0].Data = Solid(1024, 1024, {255, 0, 0, 255});
	auto p = Compiled(d);
	Diagnostic diag;
	CHECK(EvaluateStatefulOutputs(d, p, outputs, request, result, diag) == Status::LimitExceeded);
	CHECK(diag.Message.find("metadata record exceeds typed payload") != std::string::npos);
	CHECK(result.Data == prior.Data);
	CHECK(PixelOutput(result, "colour") == PixelOutput(prior, "colour"));
}
TEST_CASE(
	"Crop sixteen point taps retain an odd-size edge reached from the preceding cell",
	"[retained_images][crop_content]"
) {
	auto d = CropGraph();
	std::array images{RequestImageSource{"source", Content(65, 8, 64, 4)}};
	EvaluationRequest request;
	request.ImageSources = images;
	const std::array<std::string, 2> outputs{"colour", "distance"};
	const auto result = Run(d, request, outputs);

	CHECK(std::get<Vector4>(TypedOutput(result, "distance")) == Vector4{0, 4, 64, 3});
	CHECK(PixelOutput(result, "colour").Pixels == std::vector<uint8_t>{255, 0, 0, 255});
}
TEST_CASE(
	"Crop Atlas offsets and persisted downstream getters preserve typed source metadata",
	"[retained_images][crop_content]"
) {
	auto d = CropGraph();
	d.Nodes.erase(d.Nodes.begin());
	AtlasValue input;
	auto &a = input.Data.emplace();
	a.Kind = AtlasKind::SurfaceAtlas;
	a.Surface.Data = Content(5, 4, 2, 1);
	a.Dimension = {5, 4};
	a.OriginalDimension = {5, 4};
	a.Position = {9, -3};
	d.Junctions = {{"atlas_input", "", ValueType::Atlas, input}};
	d.Links = {{"atlas_input", "value", "crop", "surface_in"}};
	d.Nodes.push_back({"get", "pc.atlas_get", "", {}, {}});
	d.Links.push_back({"crop", "atlas", "get", "input_0"});
	d.Outputs.push_back({"position", "get", "position"});
	d.Outputs.push_back({"pixels", "get", "surface"});
	Document restored;
	Diagnostic diag;
	REQUIRE(Read(Write(d), restored, diag) == Status::Ok);
	CHECK(restored == d);
	const std::array<std::string, 3> outputs{"atlas", "position", "pixels"};
	const auto result = Run(restored, {}, outputs);
	const auto &atlas = std::get<AtlasValue>(TypedOutput(result, "atlas"));
	CHECK(atlas.Data->Position == Vector2{11, -2});
	CHECK(std::get<Vector2>(TypedOutput(result, "position")) == Vector2{11, -2});
	CHECK(PixelOutput(result, "pixels").Pixels == std::vector<uint8_t>{255, 0, 0, 255});
}
TEST_CASE(
	"Crop alpha readback quantization and selected inherited depth preserve defined formats",
	"[retained_images][crop_content]"
) {
	auto d = CropGraph();
	Image image{2, 1, std::vector<uint8_t>(32), 0, SurfaceFormat::RGBA32Float};
	REQUIRE(StoreSurfacePixel(image, 0, 0, {1, 0, 0, .001}));
	REQUIRE(StoreSurfacePixel(image, 1, 0, {0, 1, 0, 1}));
	std::array images{RequestImageSource{"source", image}};
	EvaluationRequest request;
	request.ImageSources = images;
	const std::array<std::string, 2> outputs{"colour", "distance"};
	auto result = Run(d, request, outputs);
	CHECK(PixelOutput(result, "colour").Format == SurfaceFormat::RGBA32Float);
	CHECK(PixelOutput(result, "colour").Width == 1);
	CHECK(std::get<Vector4>(TypedOutput(result, "distance")) == Vector4{0, 0, 1, 0});
	Set(d.Nodes[1], "attribute_color_depth", EnumValue{3});
	result = Run(d, request, outputs);
	CHECK(PixelOutput(result, "colour").Format == SurfaceFormat::RGBA8Unorm);
	Set(d.Nodes[1], "active", false);
	Set(d.Nodes[1], "attribute_color_depth", EnumValue{5});
	result = Run(d, request, outputs);
	CHECK(PixelOutput(result, "colour").Format == SurfaceFormat::RGBA8Unorm);
	CHECK(PixelOutput(result, "colour").Width == 2);
}
TEST_CASE(
	"Crop metadata shape validation refuses valid-but-wrong typed fields", "[retained_images][crop_content]"
) {
	auto d = CropGraph();
	std::array images{RequestImageSource{"source", Content(4, 3, 2, 1)}};
	EvaluationRequest request;
	request.ImageSources = images;
	const std::array<std::string, 3> outputs{"colour", "distance", "atlas"};
	const auto active = Run(d, request, outputs);
	auto result = active;
	Set(d.Nodes[1], "active", false);
	auto p = Compiled(d);
	auto metadata = active.Data;
	std::get<StructValue>(metadata.Entries[0].Values[0].Data).Data->Fields[0].second = double{7};
	request.DataReplay = &metadata;
	request.Tick = 1;
	Diagnostic diag;
	CHECK(EvaluateStatefulOutputs(d, p, outputs, request, result, diag) == Status::InvalidValue);
	CHECK(diag.Message.find("wrong source output shape") != std::string::npos);
	CHECK(result.Data == active.Data);
	CHECK(TypedOutput(result, "atlas") == TypedOutput(active, "atlas"));
}
TEST_CASE("Retained image metadata compares prior signed fractional clock exactly", "[retained_images]") {
	auto d = SmearGraph();
	std::array images{RequestImageSource{"source", Solid(2, 1, {255, 255, 255, 255})}};
	EvaluationRequest request;
	request.ImageSources = images;
	request.Tick = 1;
	request.Subframe = .75;
	const std::array<std::string, 2> outputs{"colour", "depth"};
	auto result = Run(d, request, outputs);
	const auto prior = result;
	Set(d.Nodes[1], "active", false);
	auto p = Compiled(d);
	request.DataReplay = &prior.Data;
	request.Subframe = .5;
	Diagnostic diag;
	CHECK(EvaluateStatefulOutputs(d, p, outputs, request, result, diag) == Status::InvalidValue);
	CHECK(result.Data == prior.Data);
	CHECK(PixelOutput(result, "depth") == PixelOutput(prior, "depth"));
	request.Subframe = .875;
	const auto later = Run(d, request, outputs);
	CHECK(PixelOutput(later, "depth") == PixelOutput(prior, "depth"));
	request.NegativeFrame = true;
	CHECK(EvaluateStatefulOutputs(d, p, outputs, request, result, diag) == Status::InvalidValue);
	CHECK(result.Data == prior.Data);
}
TEST_CASE(
	"Crop validates a whole Atlas list before an empty element loop", "[retained_images][crop_content]"
) {
	auto d = CropGraph();
	d.Nodes.erase(d.Nodes.begin());
	ArrayValue list;
	list.ElementType = ValueType::Atlas;
	d.Junctions = {{"atlas_list", "", ValueType::Array, list}};
	d.Links = {{"atlas_list", "value", "crop", "surface_in"}};
	const std::array<std::string, 3> outputs{"colour", "distance", "atlas"};
	const auto empty = Run(d, {}, outputs);
	CHECK(ArrayOutput(empty, "colour").Images.empty());
	CHECK(std::get<ArrayValue>(TypedOutput(empty, "atlas")).Elements.empty());
	AtlasValue atlas;
	auto &a = atlas.Data.emplace();
	a.Kind = AtlasKind::SurfaceAtlas;
	a.Surface.Data = Content(2, 2, 1, 1);
	a.Dimension = {2, 2};
	a.OriginalDimension = {2, 2};
	list.Nested = {{ElementValue{atlas}}};
	d.Junctions[0].Default = list;
	auto p = Compiled(d);
	auto result = empty;
	Diagnostic diag;
	CHECK(EvaluateStatefulOutputs(d, p, outputs, {}, result, diag) == Status::UnsupportedExecution);
	CHECK(diag.Message.find("flat Atlas array") != std::string::npos);
	CHECK(result.Data == empty.Data);
	CHECK(ArrayOutput(result, "colour").Images.empty());
}

TEST_CASE(
	"Crop refuses general Items rather than treating them as an empty Atlas list",
	"[retained_images][crop_content]"
) {
	auto d = CropGraph();
	d.Nodes.erase(d.Nodes.begin());
	ArrayValue list;
	list.ElementType = ValueType::Atlas;
	d.Junctions = {{"atlas_list", "", ValueType::Array, list}};
	d.Links = {{"atlas_list", "value", "crop", "surface_in"}};
	const std::array<std::string, 3> outputs{"colour", "distance", "atlas"};
	const auto empty = Run(d, {}, outputs);
	AtlasValue atlas;
	auto &a = atlas.Data.emplace();
	a.Kind = AtlasKind::SurfaceAtlas;
	a.Surface.Data = Content(2, 2, 1, 1);
	a.Dimension = {2, 2};
	a.OriginalDimension = {2, 2};
	list.ElementType = ValueType::Any;
	list.Items = {{ElementValue{atlas}}};
	d.Junctions[0].Default = list;
	auto p = Compiled(d);
	auto result = empty;
	Diagnostic diag;
	const auto status = EvaluateStatefulOutputs(d, p, outputs, {}, result, diag);
	INFO(diag.Message);
	CHECK(status == Status::InvalidValue);
	CHECK(diag.Message == "Crop source surface layout is invalid");
	CHECK(result.Data == empty.Data);
	CHECK(ArrayOutput(result, "colour").Images.empty());
}
