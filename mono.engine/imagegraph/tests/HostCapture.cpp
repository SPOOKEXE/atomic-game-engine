#include "../src/HostCaptureReceipts.hpp"
#include "../src/NodeExecutors.hpp"
#include <engine/imagegraph/Catalogue.hpp>
#include <tuple>

#include <engine/imagegraph/HostCapture.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

TEST_SUITE_ID("engine.imagegraph.host_capture")
TEST_DEPENDS("engine.imagegraph.document")

TEST_CASE(
	"live host receipt ownership survives provider storage and obeys the shared byte cap",
	"[imagegraph][host]"
) {
	using namespace engine::imagegraph;
	HostNodeCapture capture;
	capture.Authored = {"live", "pc.text_file_read", "", {}, {{"path", std::string{"captured.txt"}}}};
	capture.Inputs = capture.Authored.Values;
	capture.Outputs = {{"content", std::string{"captured"}}};
	capture.Images = {{"pixels", Image{1, 1, {12, 34, 56, 255}}}};
	detail::EvaluationBudget budget(Limits::MaximumEvaluationBytes);
	{
		detail::HostCaptureReceiptSink sink(budget);
		REQUIRE(sink.Append(capture));
		REQUIRE(sink.Captures.size() == 1);
		capture.Outputs[0].Data = std::string{"changed"};
		capture.Images[0].Data.Pixels[0] = 99;
		CHECK(std::get<std::string>(sink.Captures[0].Outputs[0].Data) == "captured");
		CHECK(sink.Captures[0].Images[0].Data.Pixels[0] == 12);
		CHECK(budget.Used() > capture.Images[0].Data.Pixels.size());
	}
	CHECK(budget.Used() == 0);
	detail::EvaluationBudget small(1);
	detail::HostCaptureReceiptSink refused(small);
	CHECK_FALSE(refused.Append(capture));
	CHECK(refused.Captures.empty());
	CHECK(small.Used() == 0);
}

TEST_CASE(
	"recorded host inputs replay exact source ports without ambient file access", "[imagegraph][host]"
) {
	using namespace engine::imagegraph;
	Document document;
	document.Nodes.push_back(
		{"read", "pc.text_file_read", "", {}, {{"path", std::string{"host://recorded/file.txt"}}}}
	);
	document.Outputs.push_back({"text", "read", "content"});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	EvaluationRequest request;
	EvaluatedValue result;
	CHECK(EvaluateValue(document, plan, "text", request, result, diagnostic) == Status::UnsupportedExecution);
	CHECK(diagnostic.Message.find("host") != std::string::npos);

	HostNodeCapture capture;
	capture.Authored = document.Nodes[0];
	capture.Inputs = document.Nodes[0].Values;
	capture.Outputs = {
		{"content", std::string{"recorded bytes"}}, {"path", std::string{"host://recorded/file.txt"}}
	};
	request.HostCaptures = std::span<const HostNodeCapture>(&capture, 1);
	REQUIRE(EvaluateValue(document, plan, "text", request, result, diagnostic) == Status::Ok);
	CHECK(std::get<std::string>(result.Data) == "recorded bytes");
	capture.Inputs[0].Data = std::string{"different input"};
	CHECK(EvaluateValue(document, plan, "text", request, result, diagnostic) == Status::InvalidValue);
	capture.Inputs = document.Nodes[0].Values;
	capture.State = HostCaptureState::Refused;
	capture.Outputs.clear();
	capture.Failure = "file capability was denied by its host";
	CHECK(EvaluateValue(document, plan, "text", request, result, diagnostic) == Status::UnsupportedExecution);
	CHECK(diagnostic.Message == capture.Failure);
}

TEST_CASE("host replay rejects duplicate records and mismatched output payloads", "[imagegraph][host]") {
	using namespace engine::imagegraph;
	Document document;
	document.Nodes.push_back({"read", "pc.text_file_read", "", {}, {{"path", std::string{"recorded"}}}});
	document.Outputs.push_back({"text", "read", "content"});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	HostNodeCapture capture;
	capture.Authored = document.Nodes[0];
	capture.Inputs = document.Nodes[0].Values;
	capture.Outputs = {{"content", 1.0}, {"path", std::string{"recorded"}}};
	EvaluationRequest request;
	request.HostCaptures = std::span<const HostNodeCapture>(&capture, 1);
	EvaluatedValue value;
	CHECK(EvaluateValue(document, plan, "text", request, value, diagnostic) == Status::TypeMismatch);
	std::vector<HostNodeCapture> duplicates{capture, capture};
	request.HostCaptures = duplicates;
	CHECK(EvaluateValue(document, plan, "text", request, value, diagnostic) == Status::DuplicateId);
}

TEST_CASE("parsed layer content selects last matching name and places cropped pixels", "[imagegraph][host]") {
	using namespace engine::imagegraph;
	Document document;
	StructValue content;
	content.Data.emplace();
	ArrayValue layers;
	layers.ElementType = ValueType::Struct;
	for (const Colour colour : {Colour{255, 0, 0, 255}, Colour{0, 255, 0, 255}}) {
		Image image;
		image.Width = 2;
		image.Height = 1;
		image.Pixels = {
			colour.Red,
			colour.Green,
			colour.Blue,
			colour.Alpha,
			colour.Red,
			colour.Green,
			colour.Blue,
			colour.Alpha
		};
		image.Hash = SurfaceHash(image);
		StructValue layer;
		layer.Data.emplace().Fields = {
			{"name", std::string{"same"}},
			{"x", int64_t{-1}},
			{"y", int64_t{1}},
			{"image", SurfaceValue{image}}
		};
		layers.Elements.push_back(std::move(layer));
	}
	content.Data->Fields = {{"width", int64_t{2}}, {"height", int64_t{2}}, {"layerData", layers}};
	document.Nodes.push_back({"data", "pc.ora_file_read", "", {}, {{"path", std::string{"fixture.ora"}}}});
	document.Nodes.push_back({"layer", "pc.ora_layer", "", {}, {{"layer_name", std::string{"same"}}}});
	document.Links.push_back({"data", "content", "layer", "data"});
	HostNodeCapture source;
	source.Authored = document.Nodes[0];
	source.Inputs = {{"path", std::string{"fixture.ora"}}};
	source.Outputs = {{"content", content}, {"path", std::string{"fixture.ora"}}};
	Image merged;
	merged.Width = 2;
	merged.Height = 2;
	merged.Pixels.resize(16);
	merged.Hash = SurfaceHash(merged);
	source.Images.push_back({"merged_image", merged});
	document.Outputs.push_back({"image", "layer", "surface_out"});
	document.FormatVersion = 9;
	Plan plan;
	Diagnostic diagnostic;
	const auto compiled = Compile(document, plan, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(compiled == Status::Ok);
	EvaluationRequest request;
	request.HostCaptures = std::span<const HostNodeCapture>(&source, 1);
	Image result;
	const auto evaluated = Evaluate(document, plan, "image", request, result, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(evaluated == Status::Ok);
	REQUIRE(result.Width == 2);
	REQUIRE(result.Height == 2);
	CHECK(result.Pixels[8] == 0);
	CHECK(result.Pixels[9] == 255);
	CHECK(result.Pixels[10] == 0);
	CHECK(result.Pixels[11] == 255);
	CHECK(result.Pixels[15] == 0);
}

TEST_CASE(
	"Tile manual export receipts reject changed surface bytes dimensions and format",
	"[imagegraph][host][tile]"
) {
	using namespace engine::imagegraph;
	Node node;
	node.Id = "export";
	node.Type = "pc.tile_tilemap_export";
	Image map;
	map.Width = 2;
	map.Height = 1;
	map.Format = SurfaceFormat::RGBA16Float;
	map.Pixels.resize(16);
	REQUIRE(StoreSurfacePixel(map, 0, 0, {1, 0, 0, 1}));
	REQUIRE(StoreSurfacePixel(map, 1, 0, {2, 0, 0, 1}));
	HostNodeCapture capture;
	capture.Authored = node;
	capture.InputImages = {{"tilemap", SurfaceHash(map)}};
	EvaluationRequest request;
	request.HostCaptures = std::span<const HostNodeCapture>(&capture, 1);
	const auto *catalogue = FindCatalogueEntry(node.Type);
	const auto executor = detail::FindExecutor(node.Type);
	REQUIRE(catalogue);
	REQUIRE(executor);
	const auto run = [&] {
		detail::NodeContext context(node, *catalogue, request);
		context.Images = {{"tilemap", &map}};
		const bool accepted = executor(context);
		return std::tuple{accepted, context.FailureCode, context.FailurePort};
	};
	{
		const auto [accepted, status, port] = run();
		CHECK(accepted);
		CHECK(status == Status::Ok);
		CHECK(port.empty());
	}
	SECTION("changed bytes with caller hash unchanged") {
		REQUIRE(StoreSurfacePixel(map, 0, 0, {3, 0, 0, 1}));
	}
	SECTION("different dimensions with identical storage") {
		map.Width = 1;
		map.Height = 2;
	}
	SECTION("different format with equal storage size") {
		map.Width = 1;
		map.Height = 1;
		map.Format = SurfaceFormat::RGBA32Float;
	}
	const auto [accepted, status, port] = run();
	CHECK_FALSE(accepted);
	CHECK(status == Status::InvalidValue);
	CHECK(port == "tilemap");
}
