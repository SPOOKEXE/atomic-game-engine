#include "NodeExecutors.hpp"

#include <engine/imagegraph/Catalogue.hpp>
#include <engine/imagegraph/HostCapture.hpp>
#include <engine/imagegraph/Surface.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>

TEST_SUITE_ID("engine.imagegraph.source_surface_buffer")
using namespace engine::imagegraph;
namespace {
	struct BufferFormatFixture {
		SurfaceFormat Native;
		uint8_t Source;
		std::vector<uint8_t> Samples;
	};
	const std::array<BufferFormatFixture, 7> BufferFormats{
		{{SurfaceFormat::RGBA8Unorm, 6, {1, 2, 3, 4, 251, 252, 253, 254}},
		 {SurfaceFormat::R16Float, 9, {0, 0x3c, 0, 0xb8}},
		 {SurfaceFormat::R32Float, 10, {0, 0, 0x80, 0x3f, 0, 0, 0, 0xbf}},
		 {SurfaceFormat::RGBA4Unorm, 11, {0x23, 0x14, 0xdc, 0xfe}},
		 {SurfaceFormat::R8Unorm, 12, {17, 251}},
		 {SurfaceFormat::RGBA16Float, 14, {0, 0, 0, 0x38, 0, 0xbc, 0, 0x3c, 0, 0x40, 0, 0, 0, 0x34, 0, 0x3c}},
		 {SurfaceFormat::RGBA32Float, 15, {0, 0, 0, 0,	  0, 0, 0, 0x3f, 0, 0, 0x80, 0xbf, 0, 0, 0x80, 0x3f,
										   0, 0, 0, 0x40, 0, 0, 0, 0,	 0, 0, 0x80, 0x3e, 0, 0, 0x80, 0x3f}}}
	};
	BufferValue
	LiteralBuffer(uint8_t format, uint16_t width, uint16_t height, const std::vector<uint8_t> &pixels) {
		// The oracle spells the source ABI independently of the implementation's
		// format table.
		BufferValue buffer;
		buffer.Bytes = {
			'P',
			'X',
			'C',
			'S',
			uint8_t(width),
			uint8_t(width >> 8),
			uint8_t(height),
			uint8_t(height >> 8),
			format
		};
		buffer.Bytes.resize(24, 0);
		buffer.Bytes.insert(buffer.Bytes.end(), pixels.begin(), pixels.end());
		return buffer;
	}
	Image BufferImage(const BufferFormatFixture &f) {
		Image image{2, 1, f.Samples};
		image.Format = f.Native;
		image.Hash = SurfaceHash(image);
		return image;
	}
	Document BufferGraph(bool decode = true) {
		Document d;
		d.FormatVersion = 9;
		d.Nodes = {
			{"capture", "image.captured", "", {}, {{"source_id", std::string{"source"}}}},
			{"encode", "pc.surface_to_buffer", "", {}, {}},
			{"decode", "pc.surface_from_buffer", "", {}, {}}
		};
		d.Links = {{"capture", "image", "encode", "surface"}, {"encode", "buffer", "decode", "input_0"}};
		d.Outputs = {{"out", decode ? "decode" : "encode", decode ? "surface" : "buffer"}};
		return d;
	}
	Plan BufferCompiled(const Document &d) {
		Plan plan;
		Diagnostic diagnostic;
		const auto status = Compile(d, plan, diagnostic);
		INFO(diagnostic.NodeId << ":" << diagnostic.Port << ":" << diagnostic.Message);
		REQUIRE(status == Status::Ok);
		return plan;
	}
	Document BufferRestored(const Document &d) {
		Document restored;
		Diagnostic diagnostic;
		const auto status = Read(Write(d), restored, diagnostic);
		INFO(diagnostic.NodeId << ":" << diagnostic.Port << ":" << diagnostic.Message);
		REQUIRE(status == Status::Ok);
		REQUIRE(restored == d);
		return restored;
	}
	Document BufferDecodeGraph() {
		Document d;
		d.FormatVersion = 9;
		d.Nodes = {
			{"bytes", "pc.byte_file_read", "", {}, {{"path", std::string{"recorded.pxcs"}}}},
			{"decode", "pc.surface_from_buffer", "", {}, {}}
		};
		d.Links = {{"bytes", "content", "decode", "input_0"}};
		d.Outputs = {{"out", "decode", "surface"}};
		return d;
	}
	HostNodeCapture BufferReceipt(
		const Document &d, const Plan &plan, BufferValue buffer, const EvaluationRequest &request = {}
	) {
		HostNodeCapture capture;
		Diagnostic diagnostic;
		auto status = PrepareHostCapture(d, plan, "bytes", request, capture, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(status == Status::Ok);
		capture.Outputs = {{"content", std::move(buffer)}, {"path", std::string{"recorded.pxcs"}}};
		return capture;
	}
	void BufferImageArray(Document &d, size_t count) {
		Node array{"array", "pc.array", "", {}, {{"type", EnumValue{1}}}};
		for (size_t i = 0; i < count; ++i)
			array.DynamicInputs.push_back({"input_" + std::to_string(i), ValueType::Image, {}});
		d.Nodes.push_back(array);
		d.Links.erase(d.Links.begin());
		d.Links.push_back({"array", "array", "encode", "surface"});
		for (size_t i = 0; i < count; ++i)
			d.Links.push_back(
				{i + 1 == count ? "large" : "capture", "image", "array", "input_" + std::to_string(i)}
			);
	}
} // namespace
TEST_CASE(
	"PXCS encoder spells all seven raw format headers and owns exact "
	"source bytes",
	"[source_surface_buffer]"
) {
	auto d = BufferRestored(BufferGraph(false));
	auto plan = BufferCompiled(d);
	for (const auto &f : BufferFormats) {
		INFO(int(f.Source));
		std::array sources{RequestImageSource{"source", BufferImage(f)}};
		EvaluationRequest request;
		request.ImageSources = sources;
		EvaluatedValue output;
		Diagnostic diagnostic;
		auto status = EvaluateValue(d, plan, "out", request, output, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(status == Status::Ok);
		const auto expected = LiteralBuffer(f.Source, 2, 1, f.Samples);
		CHECK(output.Data == Value{expected});
		sources[0].Data.Pixels[0] ^= 255;
		CHECK(output.Data == Value{expected});
	}
}
TEST_CASE(
	"PXCS seven typed surfaces roundtrip through persisted graph and "
	"independently owned pixels",
	"[source_surface_buffer]"
) {
	auto d = BufferRestored(BufferGraph());
	auto plan = BufferCompiled(d);
	for (const auto &f : BufferFormats) {
		INFO(int(f.Source));
		const auto expected = BufferImage(f);
		std::array sources{RequestImageSource{"source", expected}};
		EvaluationRequest request;
		request.ImageSources = sources;
		Image result;
		Diagnostic diagnostic;
		const auto status = Evaluate(d, plan, "out", request, result, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(status == Status::Ok);
		CHECK(result == expected);
		sources[0].Data.Pixels[0] ^= 255;
		CHECK(result == expected);
	}
}
TEST_CASE(
	"PXCS decoder accepts independently recorded literal headers for "
	"every native format",
	"[source_surface_buffer]"
) {
	auto d = BufferRestored(BufferDecodeGraph());
	auto plan = BufferCompiled(d);
	for (const auto &f : BufferFormats) {
		auto capture = BufferReceipt(d, plan, LiteralBuffer(f.Source, 2, 1, f.Samples));
		EvaluationRequest request;
		request.HostCaptures = std::span{&capture, 1};
		Image result;
		Diagnostic diagnostic;
		const auto status = Evaluate(d, plan, "out", request, result, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(status == Status::Ok);
		CHECK(result == BufferImage(f));
		std::get<BufferValue>(capture.Outputs[0].Data).Bytes[24] ^= 255;
		CHECK(result == BufferImage(f));
	}
}
TEST_CASE(
	"PXCS ignores reserved header bytes and trailing bytes while "
	"retaining offset 24",
	"[source_surface_buffer]"
) {
	auto d = BufferRestored(BufferDecodeGraph());
	auto plan = BufferCompiled(d);
	auto buffer = LiteralBuffer(6, 1, 1, {23, 45, 67, 89});
	for (size_t i = 9; i < 24; ++i)
		buffer.Bytes[i] = uint8_t(i * 3);
	buffer.Bytes.push_back(201);
	auto capture = BufferReceipt(d, plan, buffer);
	EvaluationRequest request;
	request.HostCaptures = std::span{&capture, 1};
	Image result;
	Diagnostic diagnostic;
	REQUIRE(Evaluate(d, plan, "out", request, result, diagnostic) == Status::Ok);
	CHECK(result.Pixels == std::vector<uint8_t>{23, 45, 67, 89});
	CHECK(result.Width == 1);
	CHECK(result.Height == 1);
}
TEST_CASE(
	"PXCS missing default operands refuse unrepresented noone rather "
	"than fabricate results",
	"[source_surface_buffer]"
) {
	for (const std::string type : {"pc.surface_to_buffer", "pc.surface_from_buffer"}) {
		Document d;
		d.FormatVersion = 9;
		d.Nodes = {{"missing", type, "", {}, {}}};
		d.Outputs = {{"out", "missing", type == "pc.surface_to_buffer" ? "buffer" : "surface"}};
		auto plan = BufferCompiled(d);
		Diagnostic diagnostic;
		EvaluatedValue output;
		output.Data = 7.;
		CHECK(EvaluateValue(d, plan, "out", {}, output, diagnostic) == Status::UnsupportedExecution);
		CHECK(output.Data == Value{7.});
		CHECK(diagnostic.NodeId == "missing");
	}
}
TEST_CASE(
	"PXCS invalid magic short zero unsupported and truncated buffers "
	"preserve caller image",
	"[source_surface_buffer]"
) {
	auto d = BufferRestored(BufferDecodeGraph());
	auto plan = BufferCompiled(d);
	std::vector<BufferValue> cases{
		BufferValue{{}},
		BufferValue{{'P', 'X', 'C', 'S'}},
		LiteralBuffer(6, 0, 1, {}),
		LiteralBuffer(6, 1, 0, {}),
		LiteralBuffer(13, 1, 1, {1, 2}),
		LiteralBuffer(7, 1, 1, {1, 2, 3, 4}),
		LiteralBuffer(6, 2, 1, {1, 2, 3, 4})
	};
	auto badMagic = LiteralBuffer(6, 1, 1, {1, 2, 3, 4});
	badMagic.Bytes[3] = 'Z';
	cases.push_back(badMagic);
	for (auto buffer : cases) {
		auto capture = BufferReceipt(d, plan, std::move(buffer));
		EvaluationRequest request;
		request.HostCaptures = std::span{&capture, 1};
		Image result{1, 1, {77, 88, 99, 111}};
		const auto previous = result;
		Diagnostic diagnostic;
		CHECK(Evaluate(d, plan, "out", request, result, diagnostic) == Status::UnsupportedExecution);
		INFO(diagnostic.Message);
		CHECK(result == previous);
		CHECK(diagnostic.NodeId == "decode");
		CHECK(diagnostic.Port == "input_0");
	}
}
TEST_CASE(
	"PXCS decoder rejects nonfinite IEEE16 and IEEE32 sample words atomically", "[source_surface_buffer]"
) {
	auto d = BufferDecodeGraph();
	auto plan = BufferCompiled(d);
	for (auto buffer : {LiteralBuffer(9, 1, 1, {0, 0x7c}), LiteralBuffer(10, 1, 1, {0, 0, 0xc0, 0x7f})}) {
		auto capture = BufferReceipt(d, plan, buffer);
		EvaluationRequest request;
		request.HostCaptures = std::span{&capture, 1};
		Image output{1, 1, {1, 2, 3, 4}};
		const auto previous = output;
		Diagnostic diagnostic;
		CHECK(Evaluate(d, plan, "out", request, output, diagnostic) == Status::UnsupportedExecution);
		CHECK(output == previous);
		CHECK(diagnostic.Message.find("nonfinite") != std::string::npos);
	}
}
TEST_CASE(
	"PXCS decoded dimensions and encoded byte bounds refuse before "
	"output storage",
	"[source_surface_buffer]"
) {
	auto d = BufferDecodeGraph();
	auto plan = BufferCompiled(d);
	auto capture = BufferReceipt(d, plan, LiteralBuffer(6, 4097, 1, {}));
	EvaluationRequest request;
	request.HostCaptures = std::span{&capture, 1};
	Image output{1, 1, {1, 2, 3, 4}};
	const auto previous = output;
	Diagnostic diagnostic;
	CHECK(Evaluate(d, plan, "out", request, output, diagnostic) == Status::LimitExceeded);
	CHECK(output == previous);
	const auto *entry = FindCatalogueEntry("pc.surface_to_buffer");
	REQUIRE(entry);
	auto execute = detail::FindExecutor(entry->Type);
	REQUIRE(execute);
	Node authored{"encode", "pc.surface_to_buffer", "", {}, {}};
	EvaluationRequest nativeRequest;
	detail::NodeContext c(authored, *entry, nativeRequest);
	c.ByteBudget = Limits::MaximumEvaluationBytes;
	Image huge{1024, 1024, std::vector<uint8_t>(4 * 1024 * 1024, 3)};
	c.Images = {{"surface", &huge}};
	CHECK_FALSE(execute(c));
	CHECK(c.FailureCode == Status::LimitExceeded);
	CHECK(c.OutputValues.empty());
}
TEST_CASE(
	"PXCS scalar conversion remains active when processor array "
	"processing is disabled",
	"[source_surface_buffer]"
) {
	auto d = BufferGraph();
	d.Nodes[1].Values = {{"attribute_process", false}};
	d.Nodes[2].Values = {{"attribute_process", false}};
	d = BufferRestored(d);
	auto plan = BufferCompiled(d);
	std::array sources{RequestImageSource{"source", BufferImage(BufferFormats[0])}};
	EvaluationRequest request;
	request.ImageSources = sources;
	Image output;
	Diagnostic diagnostic;
	REQUIRE(Evaluate(d, plan, "out", request, output, diagnostic) == Status::Ok);
	CHECK(output == sources[0].Data);
}
TEST_CASE(
	"PXCS processor arrays retain heterogeneous dimensions in all four "
	"schedules",
	"[source_surface_buffer]"
) {
	auto d = BufferGraph();
	d.Nodes.push_back({"large", "image.captured", "", {}, {{"source_id", std::string{"large"}}}});
	BufferImageArray(d, 2);
	Image first{1, 1, {1, 2, 3, 4}}, second{2, 1, {5, 6, 7, 8, 9, 10, 11, 12}};
	first.Hash = SurfaceHash(first);
	second.Hash = SurfaceHash(second);
	const std::array sources{RequestImageSource{"source", first}, RequestImageSource{"large", second}};
	EvaluationRequest request;
	request.ImageSources = sources;
	for (int64_t mode = 0; mode < 4; ++mode) {
		d.Nodes[1].Values = {{"attribute_array_process", EnumValue{mode}}};
		d.Nodes[2].Values = d.Nodes[1].Values;
		auto restored = BufferRestored(d);
		auto plan = BufferCompiled(restored);
		ImageArray output;
		Diagnostic diagnostic;
		auto status = EvaluateArray(restored, plan, "out", request, output, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(status == Status::Ok);
		REQUIRE(output.Images.size() == 2);
		CHECK(output.Images[0] == first);
		CHECK(output.Images[1] == second);
		restored.Outputs = {{"out", "encode", "buffer"}};
		plan = BufferCompiled(restored);
		EvaluatedValue value;
		REQUIRE(EvaluateValue(restored, plan, "out", request, value, diagnostic) == Status::Ok);
		const auto &array = std::get<ArrayValue>(value.Data);
		REQUIRE(array.Elements.size() == 2);
		CHECK(array.ElementType == ValueType::Buffer);
		CHECK(array.Elements[0] == ElementValue{LiteralBuffer(6, 1, 1, first.Pixels)});
		CHECK(array.Elements[1] == ElementValue{LiteralBuffer(6, 2, 1, second.Pixels)});
	}
}
TEST_CASE(
	"PXCS disabled processor rejects whole source image array without "
	"implicit flattening",
	"[source_surface_buffer]"
) {
	auto d = BufferGraph(false);
	d.Nodes.push_back({"large", "image.captured", "", {}, {{"source_id", std::string{"large"}}}});
	BufferImageArray(d, 2);
	d.Nodes[1].Values = {{"attribute_process", false}};
	auto plan = BufferCompiled(d);
	const std::array sources{
		RequestImageSource{"source", Image{1, 1, {1, 2, 3, 4}}},
		RequestImageSource{"large", Image{1, 1, {5, 6, 7, 8}}}
	};
	EvaluationRequest request;
	request.ImageSources = sources;
	EvaluatedValue output;
	output.Data = 19.;
	Diagnostic diagnostic;
	CHECK(EvaluateValue(d, plan, "out", request, output, diagnostic) == Status::UnsupportedExecution);
	CHECK(output.Data == Value{19.});
}
TEST_CASE(
	"PXCS dimensions follow real animated image producer and seek "
	"without stale buffer state",
	"[source_surface_buffer]"
) {
	auto d = BufferGraph(false);
	d.Nodes[0] = {
		"capture",
		"image.solid",
		"",
		{},
		{{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{12, 34, 56, 78}}}
	};
	d.Keyframes = {{"capture", "width", 0, int64_t{1}, "step"}, {"capture", "width", 2, int64_t{2}, "step"}};
	d = BufferRestored(d);
	auto plan = BufferCompiled(d);
	Diagnostic diagnostic;
	for (uint64_t tick : {0u, 2u, 1u}) {
		EvaluationRequest request;
		request.Tick = tick;
		EvaluatedValue output;
		auto status = EvaluateValue(d, plan, "out", request, output, diagnostic);
		INFO(diagnostic.Message);
		REQUIRE(status == Status::Ok);
		const auto &bytes = std::get<BufferValue>(output.Data).Bytes;
		const uint8_t width = tick == 2 ? 2 : 1;
		CHECK(bytes[4] == width);
		CHECK(bytes.size() == 24 + 4 * size_t(width));
		CHECK(
			std::vector<uint8_t>(bytes.begin() + 24, bytes.begin() + 28) ==
			std::vector<uint8_t>{12, 34, 56, 78}
		);
	}
}
TEST_CASE(
	"PXCS encoded buffer feeds real downstream hexadecimal text conversion", "[source_surface_buffer]"
) {
	auto d = BufferGraph(false);
	d.Nodes.push_back({"text", "pc.buffer_to_string", "", {}, {{"format", EnumValue{1}}}});
	d.Links.push_back({"encode", "buffer", "text", "input_0"});
	d.Outputs = {{"out", "text", "string_out"}};
	auto plan = BufferCompiled(d);
	const std::array sources{RequestImageSource{"source", Image{1, 1, {1, 2, 3, 4}}}};
	EvaluationRequest request;
	request.ImageSources = sources;
	EvaluatedValue output;
	Diagnostic diagnostic;
	REQUIRE(EvaluateValue(d, plan, "out", request, output, diagnostic) == Status::Ok);
	CHECK(output.Data == Value{std::string{"50584353010001000600000000000000000000000000000001020304"}});
}
TEST_CASE(
	"PXCS expensive later processor row refuses complete batch before "
	"first output allocation",
	"[source_surface_buffer]"
) {
	auto d = BufferGraph(false);
	d.Nodes.push_back({"large", "image.captured", "", {}, {{"source_id", std::string{"large"}}}});
	BufferImageArray(d, 40);
	auto plan = BufferCompiled(d);
	Image tiny{1, 1, {1, 2, 3, 4}}, huge{1000, 1000, std::vector<uint8_t>(4000000, 17)};
	const std::array sources{RequestImageSource{"source", tiny}, RequestImageSource{"large", huge}};
	EvaluationRequest request;
	request.ImageSources = sources;
	EvaluatedValue output;
	output.Data = 31.;
	Diagnostic diagnostic;
	CHECK(EvaluateValue(d, plan, "out", request, output, diagnostic) == Status::LimitExceeded);
	INFO(diagnostic.Message);
	CHECK(diagnostic.Message.find("complete processor batch") != std::string::npos);
	CHECK(output.Data == Value{31.});
	CHECK(diagnostic.NodeId == "encode");
	const auto *entry = FindCatalogueEntry("pc.surface_to_buffer");
	REQUIRE(entry);
	auto execute = detail::FindExecutor(entry->Type);
	REQUIRE(execute);
	ImageArray borrowed;
	borrowed.Images.assign(39, tiny);
	borrowed.Images.push_back(huge);
	detail::NodeContext c(d.Nodes[1], *entry, request);
	c.ByteBudget = Limits::MaximumEvaluationBytes;
	c.ProcessorCount = 40;
	c.Images = {{"surface", &tiny}};
	c.ImageArrays = {{"surface", &borrowed}};
	CHECK_FALSE(execute(c));
	CHECK(c.FailureCode == Status::LimitExceeded);
	CHECK(c.OutputValues.empty());
	CHECK(c.OutputImages.empty());
}
TEST_CASE(
	"PXCS allocation refusal preserves caller image and borrowed scalar "
	"outputs",
	"[source_surface_buffer]"
) {
	auto d = BufferGraph();
	auto plan = BufferCompiled(d);
	Image large{128, 128, std::vector<uint8_t>(65536, 19)};
	const std::array sources{RequestImageSource{"source", large}};
	EvaluationRequest request;
	request.ImageSources = sources;
	Image output{1, 1, {77, 77, 77, 77}};
	const auto previous = output;
	Diagnostic diagnostic;
	CHECK(Evaluate(d, plan, "out", request, output, diagnostic, 512) == Status::LimitExceeded);
	CHECK(output == previous);
	for (const std::string type : {"pc.surface_to_buffer", "pc.surface_from_buffer"}) {
		const auto *entry = FindCatalogueEntry(type);
		REQUIRE(entry);
		auto execute = detail::FindExecutor(type);
		REQUIRE(execute);
		Node node{"limited", type, "", {}, {}};
		detail::NodeContext c(node, *entry, request);
		c.ByteBudget = 1;
		const Value buffer = LiteralBuffer(6, 1, 1, {1, 2, 3, 4});
		c.Images = {{"surface", &large}};
		c.ValueViews = {{"input_0", &buffer}};
		CHECK_FALSE(execute(c));
		CHECK(c.FailureCode == Status::LimitExceeded);
		CHECK(c.OutputValues.empty());
		CHECK(c.OutputImages.empty());
	}
}
TEST_CASE(
	"PXCS unknown authored controls reject compilation instead of "
	"becoming invented header options",
	"[source_surface_buffer]"
) {
	auto d = BufferGraph();
	d.Nodes[1].Values = {{"header", false}};
	Plan plan;
	Diagnostic diagnostic;
	CHECK(Compile(d, plan, diagnostic) != Status::Ok);
	CHECK(diagnostic.NodeId == "encode");
	CHECK(diagnostic.Port == "header");
}
TEST_CASE(
	"PXCS two-byte width and height are little endian independently of "
	"sample encoding",
	"[source_surface_buffer]"
) {
	auto d = BufferGraph(false);
	auto plan = BufferCompiled(d);
	Image image{257, 258, std::vector<uint8_t>(257 * 258 * 4, 3)};
	const std::array sources{RequestImageSource{"source", image}};
	EvaluationRequest request;
	request.ImageSources = sources;
	EvaluatedValue output;
	Diagnostic diagnostic;
	REQUIRE(EvaluateValue(d, plan, "out", request, output, diagnostic) == Status::Ok);
	const auto &b = std::get<BufferValue>(output.Data).Bytes;
	REQUIRE(b.size() >= 24);
	CHECK(
		std::vector<uint8_t>(b.begin(), b.begin() + 9) ==
		std::vector<uint8_t>{'P', 'X', 'C', 'S', 1, 1, 2, 1, 6}
	);
}
TEST_CASE(
	"PXCS mixed array refusal distinguishes upstream policy from converter validation",
	"[source_surface_buffer]"
) {
	for (bool encode : {true, false})
		for (bool general : {false, true}) {
			INFO("encode=" << encode << ", general=" << general);
			auto d = encode ? BufferGraph(false) : BufferDecodeGraph();
			Node array{"mixed", "pc.array", "", {}, {{"type", EnumValue{0}}}};
			array.DynamicInputs = {
				{"input_0", encode ? ValueType::Image : ValueType::Buffer, {}},
				{"input_1", ValueType::Scalar, 0.}
			};
			d.Nodes.push_back(array);
			if (encode) {
				d.Links.erase(d.Links.begin());
				d.Links.push_back({"capture", "image", "mixed", "input_0"});
				d.Links.push_back({"mixed", "array", "encode", "surface"});
			} else {
				d.Links = {{"bytes", "content", "mixed", "input_0"}, {"mixed", "array", "decode", "input_0"}};
			}
			if (general) {
				d.Nodes.back().DynamicInputs[1] = {"input_1", ValueType::Any, {}};
				d.Nodes.push_back(
					{"json",
					 "pc.struct_json_parse",
					 "",
					 {},
					 {{"json_string", std::string{"[0,\"invalid\"]"}}}}
				);
				d.Links.push_back({"json", "struct", "mixed", "input_1"});
			}
			d = BufferRestored(d);
			auto plan = BufferCompiled(d);
			EvaluationRequest request;
			const std::array sources{RequestImageSource{"source", Image{1, 1, {1, 2, 3, 4}}}};
			HostNodeCapture capture;
			if (encode)
				request.ImageSources = sources;
			else {
				capture = BufferReceipt(d, plan, LiteralBuffer(6, 1, 1, {1, 2, 3, 4}));
				request.HostCaptures = std::span{&capture, 1};
			}
			if (general) {
				auto producer = d;
				producer.Outputs = {{"out", "mixed", "array"}};
				const auto producerPlan = BufferCompiled(producer);
				EvaluatedValue mixed;
				Diagnostic producerDiagnostic;
				const auto status =
					EvaluateValue(producer, producerPlan, "out", request, mixed, producerDiagnostic);
				INFO(producerDiagnostic.Message);
				REQUIRE(status == Status::Ok);
				const auto *items = std::get_if<ArrayValue>(&mixed.Data);
				REQUIRE(items);
				CHECK(items->ElementType == ValueType::Any);
				REQUIRE(items->Items.size() == 2);
				if (encode) {
					REQUIRE(std::holds_alternative<Image>(items->Items[0].Data));
					CHECK(std::get<Image>(items->Items[0].Data).Pixels == std::vector<uint8_t>{1, 2, 3, 4});
				} else {
					REQUIRE(std::holds_alternative<ElementValue>(items->Items[0].Data));
					CHECK(
						std::get<ElementValue>(items->Items[0].Data) ==
						ElementValue{LiteralBuffer(6, 1, 1, {1, 2, 3, 4})}
					);
				}
				REQUIRE(std::holds_alternative<std::vector<SourceArrayItem>>(items->Items[1].Data));
				CHECK(std::get<std::vector<SourceArrayItem>>(items->Items[1].Data).size() == 2);
				if (encode) {
					// The public image resolver refuses this generic transport first.
					// Borrow the actual producer result to exercise the kernel boundary too.
					const auto *entry = FindCatalogueEntry("pc.surface_to_buffer");
					REQUIRE(entry);
					const auto execute = detail::FindExecutor(entry->Type);
					REQUIRE(execute);
					detail::NodeContext context(d.Nodes[1], *entry, request);
					context.ByteBudget = Limits::MaximumEvaluationBytes;
					context.ProcessorCount = 2;
					const std::array<std::pair<std::string_view, const Value *>, 1> originals{
						{{"surface", &mixed.Data}}
					};
					context.ProcessorOriginalValues = originals;
					context.Images = {{"surface", &std::get<Image>(items->Items[0].Data)}};
					CHECK_FALSE(execute(context));
					CHECK(context.FailureCode == Status::UnsupportedExecution);
					CHECK(context.FailureMessage.find("unrepresented source operand") != std::string::npos);
					CHECK(context.OutputValues.empty());
					CHECK(context.OutputImages.empty());
				}
			}
			EvaluatedValue output;
			output.Data = 81.;
			Diagnostic diagnostic;
			CHECK(EvaluateValue(d, plan, "out", request, output, diagnostic) == Status::UnsupportedExecution);
			INFO(diagnostic.Message);
			CHECK(diagnostic.NodeId == (general ? (encode ? "encode" : "decode") : "mixed"));
			if (general && encode)
				CHECK(diagnostic.Message == "image input received an array");
			else if (general)
				CHECK(diagnostic.Message.find("unrepresented source operand") != std::string::npos);
			else
				CHECK(
					diagnostic.Message == (encode ? "Array cannot mix images and typed values"
												  : "Array requires homogeneous leaf types")
				);
			CHECK(output.Data == Value{81.});
		}
}
TEST_CASE(
	"PXCS decoder surveys original buffer rows before allocating a cheap "
	"selected row",
	"[source_surface_buffer]"
) {
	const auto *entry = FindCatalogueEntry("pc.surface_from_buffer");
	REQUIRE(entry);
	auto execute = detail::FindExecutor(entry->Type);
	REQUIRE(execute);
	Node authored{"decode", "pc.surface_from_buffer", "", {}, {}};
	EvaluationRequest request;
	detail::NodeContext c(authored, *entry, request);
	c.ByteBudget = Limits::MaximumEvaluationBytes;
	c.ProcessorCount = 20;
	const Value tiny = LiteralBuffer(6, 1, 1, {1, 2, 3, 4});
	const auto large = LiteralBuffer(6, 1000, 500, std::vector<uint8_t>(2000000, 7));
	ArrayValue rows{ValueType::Buffer, {std::get<BufferValue>(tiny), large}};
	const Value original = rows;
	std::array<std::pair<std::string_view, const Value *>, 1> views{{{"input_0", &original}}};
	c.ProcessorOriginalValues = views;
	c.ValueViews = {{"input_0", &tiny}};
	CHECK_FALSE(execute(c));
	CHECK(c.FailureCode == Status::LimitExceeded);
	CHECK(c.FailureMessage.find("complete processor batch") != std::string::npos);
	CHECK(c.OutputImages.empty());
	CHECK(c.OutputValues.empty());
}
TEST_CASE(
	"PXCS real recorded buffer array rejects an expensive later row as "
	"one batch",
	"[source_surface_buffer]"
) {
	auto d = BufferDecodeGraph();
	d.Nodes.push_back({"large", "pc.byte_file_read", "", {}, {{"path", std::string{"large.pxcs"}}}});
	Node array{"array", "pc.array", "", {}, {{"type", EnumValue{0}}}};
	for (size_t i = 0; i < 20; ++i)
		array.DynamicInputs.push_back({"input_" + std::to_string(i), ValueType::Buffer, {}});
	d.Nodes.push_back(array);
	d.Links = {{"array", "array", "decode", "input_0"}};
	for (size_t i = 0; i < 20; ++i)
		d.Links.push_back({i == 19 ? "large" : "bytes", "content", "array", "input_" + std::to_string(i)});
	d = BufferRestored(d);
	auto plan = BufferCompiled(d);
	Diagnostic diagnostic;
	EvaluationRequest request;
	auto small = BufferReceipt(d, plan, LiteralBuffer(6, 1, 1, {1, 2, 3, 4}));
	HostNodeCapture large;
	REQUIRE(PrepareHostCapture(d, plan, "large", request, large, diagnostic) == Status::Ok);
	large.Outputs = {
		{"content", LiteralBuffer(6, 1000, 500, std::vector<uint8_t>(2000000, 7))},
		{"path", std::string{"large.pxcs"}}
	};
	const std::array captures{small, large};
	request.HostCaptures = captures;
	Image output{1, 1, {11, 22, 33, 44}};
	const auto previous = output;
	CHECK(Evaluate(d, plan, "out", request, output, diagnostic) == Status::LimitExceeded);
	INFO(diagnostic.Message);
	CHECK(diagnostic.NodeId == "decode");
	CHECK(diagnostic.Message.find("complete processor batch") != std::string::npos);
	CHECK(output == previous);
}
