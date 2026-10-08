#include <engine/bake/Pxcx.hpp>
#include <engine/imagegraph/Document.hpp>
#include <engine/imagegraphio/PxcxImport.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

TEST_SUITE_ID("engine.imagegraphio.pxcx_camera_visibility")

namespace {
	using namespace engine::bake;
	using namespace engine::imagegraph;
	using namespace engine::imagegraphio;

	std::string Value(std::string_view value) {
		return "{\"r\":{\"d\":" + std::string(value) + "}}";
	}

	PxcxArchive CameraArchive(std::string_view visibility = {}) {
		std::vector<std::string> inputs(14, Value("0"));
		inputs[0] = Value("[1,1]");
		inputs[1] = Value("1");
		inputs[2] = Value("false");
		inputs[3] = Value("0");
		inputs[4] = Value("1");
		inputs[5] = Value("0");
		inputs[6] = Value("[2,1]");
		inputs[7] = Value("[2,1]");
		inputs[8] = Value("-4");
		inputs[9] = Value("1");
		inputs[10] = Value("[0,0]");
		inputs[11] = Value("0");
		inputs[12] = Value("[0,0]");
		inputs[13] = Value("0");
		std::string node = "{\"id\":\"camera\",\"type\":\"Node_Camera\",\"x\":0,\"y\":0,\"inputs\":[";
		for (size_t index = 0; index < inputs.size(); ++index) {
			if (index) node.push_back(',');
			node += inputs[index];
		}
		node += "]";
		if (!visibility.empty()) node += ",\"attri\":{\"layer_visible\":" + std::string(visibility) + "}";
		node.push_back('}');

		PxcxArchive archive;
		archive.MetadataNumber = 121092;
		archive.MetadataText = "1.22.10.201";
		archive.GraphJson = "{\"nodes\":[" + node + "]}";
		archive.GraphJson.push_back('\0');
		std::vector<std::byte> bytes;
		std::string failure;
		if (!WritePxcx(archive, bytes, failure)) return {};
		PxcxArchive parsed;
		if (!ReadPxcx(bytes, parsed, failure)) return {};
		return parsed;
	}

	const Node *CameraNode(const Document &document) {
		const auto found = std::find_if(document.Nodes.begin(), document.Nodes.end(), [](const Node &node) {
			return node.Type == "pc.camera";
		});
		return found == document.Nodes.end() ? nullptr : &*found;
	}

	bool Visibility(const Node &node, std::vector<bool> &result) {
		const auto found =
			std::find_if(node.SourceProperties.begin(), node.SourceProperties.end(), [](const auto &value) {
				return value.Port == "layer_visible";
			});
		if (found == node.SourceProperties.end()) return false;
		const auto *array = std::get_if<ArrayValue>(&found->Data);
		if (!array) return false;
		for (const ElementValue &element : array->Elements) {
			const auto *flag = std::get_if<bool>(&element);
			if (!flag) return false;
			result.push_back(*flag);
		}
		return true;
	}
}

TEST_CASE(
	"Camera visibility imports with default and all-hidden state and survives native save", "[imagegraphio]"
) {
	for (const auto [sourceVisibility, expected] :
		 {std::pair{std::string_view{""}, std::vector<bool>{}},
		  std::pair{std::string_view{"[false]"}, std::vector<bool>{false}}}) {
		const PxcxArchive source = CameraArchive(sourceVisibility);
		REQUIRE(!source.GraphJson.empty());
		PxcxImport imported;
		std::string failure;
		REQUIRE(ImportPxcxImageGraph(source, imported, failure));
		const Node *camera = CameraNode(imported.Graph);
		REQUIRE(camera);
		CHECK(imported.Source.OriginalBytes == source.OriginalBytes);
		if (expected.empty()) {
			CHECK(
				std::none_of(
					camera->SourceProperties.begin(), camera->SourceProperties.end(), [](const auto &value) {
						return value.Port == "layer_visible";
					}
				)
			);
		} else {
			std::vector<bool> visibility;
			REQUIRE(Visibility(*camera, visibility));
			CHECK(visibility == expected);
		}
		const std::string text = Write(imported.Graph);
		Document restored;
		Diagnostic diagnostic;
		REQUIRE(Read(text, restored, diagnostic) == Status::Ok);
		const Node *reopened = CameraNode(restored);
		REQUIRE(reopened);
		if (expected.empty())
			CHECK(
				std::none_of(
					reopened->SourceProperties.begin(),
					reopened->SourceProperties.end(),
					[](const auto &value) { return value.Port == "layer_visible"; }
				)
			);
		else {
			std::vector<bool> visibility;
			REQUIRE(Visibility(*reopened, visibility));
			CHECK(visibility == expected);
		}
		std::vector<std::byte> bytes;
		REQUIRE(WritePxcx(imported.Source, bytes, failure));
		PxcxArchive sourceReopened;
		REQUIRE(ReadPxcx(bytes, sourceReopened, failure));
		CHECK(sourceReopened.GraphJson == source.GraphJson);
	}
}

TEST_CASE("Camera visibility length mismatch leaves the foreign node opaque", "[imagegraphio]") {
	const PxcxArchive source = CameraArchive("[false,false]");
	REQUIRE(!source.GraphJson.empty());
	PxcxImport imported;
	std::string failure;
	REQUIRE(ImportPxcxImageGraph(source, imported, failure));
	CHECK(CameraNode(imported.Graph) == nullptr);
	CHECK(std::any_of(imported.Graph.Nodes.begin(), imported.Graph.Nodes.end(), [](const Node &node) {
		return node.Type == "pxcx.opaque/Node_Camera";
	}));
	CHECK(imported.Source.OriginalBytes == source.OriginalBytes);
}
