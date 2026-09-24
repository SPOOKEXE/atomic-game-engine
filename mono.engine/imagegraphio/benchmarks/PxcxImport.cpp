// Measures source-backed projection of a bounded synthetic PXCX graph.

#include <engine/bake/Pxcx.hpp>
#include <engine/imagegraphio/PxcxImport.hpp>
#include <engine/testing/Bench.hpp>

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.imagegraphio.bench.pxcx-import")

namespace {
	using engine::bake::PxcxArchive;
	using engine::bake::ReadPxcx;
	using engine::bake::WritePxcx;
	using engine::imagegraphio::ImportPxcxImageGraph;
	using engine::imagegraphio::PxcxImport;

	constexpr size_t OPAQUE_NODE_COUNT = 256;

	std::string Value(std::string_view json) {
		return "{\"r\":{\"d\":" + std::string(json) + "}}";
	}

	std::string Wire(std::string_view source) {
		return "{\"from_node\":\"" + std::string(source) + "\",\"from_index\":0}";
	}

	std::string Node(std::string_view id, std::string_view type, const std::vector<std::string> &inputs) {
		std::string text = "{\"id\":\"" + std::string(id) + "\",\"type\":\"" + std::string(type) +
						   "\",\"x\":1,\"y\":2,\"inputs\":[";
		for (size_t index = 0; index < inputs.size(); ++index) {
			if (index != 0) text.push_back(',');
			text += inputs[index];
		}
		text += "]}";
		return text;
	}

	std::vector<std::string> SolidInputs() {
		std::vector<std::string> inputs(6, Value("-4"));
		inputs[0] = R"JSON({"r":{"d":[1,1]},"attri":{"use_project_dimension":1}})JSON";
		inputs[1] = Value("4278850590");
		inputs[2] = Value("false");
		inputs[3] = R"JSON({"r":{"d":-4},"attri":{"mask_alpha_only":false}})JSON";
		inputs[4] = Value("true");
		return inputs;
	}

	PxcxArchive SourceArchive() {
		std::string graph = R"JSON({"attributes":{"surface_dimension":[64,64]},"nodes":[)JSON";
		graph += Node("solid", "Node_Solid", SolidInputs());

		std::string previous = "solid";
		for (size_t index = 0; index < OPAQUE_NODE_COUNT; ++index) {
			const std::string id = "opaque-" + std::to_string(index);
			graph.push_back(',');
			graph += Node(id, "Vendor_Stress", {Wire(previous)});
			previous = id;
		}
		graph.push_back(',');
		graph += Node("sink", "Node_Project_Output", {Wire(previous)});
		graph += "]}";

		PxcxArchive authored;
		authored.MetadataNumber = 121092;
		authored.MetadataText = "1.22.10.201";
		authored.GraphJson = std::move(graph);
		authored.GraphJson.push_back('\0');

		std::vector<std::byte> bytes;
		std::string failure;
		if (!WritePxcx(authored, bytes, failure)) {
			throw std::runtime_error("imagegraphio benchmark PXCX encode failed: " + failure);
		}

		PxcxArchive parsed;
		if (!ReadPxcx(bytes, parsed, failure)) {
			throw std::runtime_error("imagegraphio benchmark PXCX parse failed: " + failure);
		}
		return parsed;
	}

	bool SameImport(const PxcxImport &left, const PxcxImport &right) {
		return left.Graph == right.Graph && left.Diagnostics == right.Diagnostics &&
			   left.NativeNodes == right.NativeNodes &&
			   left.Source.OriginalBytes == right.Source.OriginalBytes &&
			   left.Source.MetadataPayload == right.Source.MetadataPayload &&
			   left.Source.MetadataNumber == right.Source.MetadataNumber &&
			   left.Source.MetadataText == right.Source.MetadataText &&
			   left.Source.GraphJson == right.Source.GraphJson && left.Source.Nodes == right.Source.Nodes &&
			   left.Source.Links == right.Source.Links;
	}

	struct ImportFixture {
		PxcxArchive Source = SourceArchive();
		PxcxImport Output;
		std::string Failure;

		ImportFixture() {
			PxcxImport first;
			PxcxImport second;
			if (!ImportPxcxImageGraph(Source, first, Failure) ||
				!ImportPxcxImageGraph(Source, second, Failure)) {
				throw std::runtime_error("imagegraphio benchmark preflight failed: " + Failure);
			}
			if (!SameImport(first, second) || first.Source.OriginalBytes != Source.OriginalBytes ||
				first.NativeNodes != 1 || first.Graph.Nodes.size() != OPAQUE_NODE_COUNT + 2 ||
				first.Graph.Links.size() != OPAQUE_NODE_COUNT + 1 || first.Graph.Outputs.size() != 1) {
				throw std::runtime_error("imagegraphio benchmark preflight parity failed");
			}
			Output = std::move(second);
		}

		void Run() {
			if (!ImportPxcxImageGraph(Source, Output, Failure)) {
				throw std::runtime_error("imagegraphio benchmark import failed: " + Failure);
			}
			engine::testing::Consume(Output.NativeNodes);
			engine::testing::Consume(Output.Graph.Nodes.size());
			engine::testing::Consume(Output.Source.OriginalBytes.size());
		}
	};

	ImportFixture &Fixture() {
		static ImportFixture fixture;
		return fixture;
	}
}

BENCH("project 256 opaque nodes and 257 links with source retention", 1) {
	Fixture().Run();
}
