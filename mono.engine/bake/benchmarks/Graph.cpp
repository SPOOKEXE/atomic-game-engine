// In-memory authored bake document through graph construction and evaluation.
// File decoding and publishing are outside this device-free benchmark.

#include <engine/assets/Builtin.hpp>
#include <engine/assets/Mesh.hpp>
#include <engine/bake/GraphDocument.hpp>
#include <engine/bake/Model.hpp>
#include <engine/bakegraph/Schema.hpp>
#include <engine/core/Bytes.hpp>
#include <engine/core/HeapProfile.hpp>
#include <engine/graphcore/Schema.hpp>
#include <engine/testing/Bench.hpp>

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <vector>

TEST_SUITE_ID("engine.bake.bench.graph")

namespace {
	using namespace engine::bake;
	using engine::testing::Consume;

	Operation Named(OperationKind kind, std::string name) {
		Operation operation;
		operation.Kind = kind;
		operation.Text = std::move(name);
		return operation;
	}

	Operation Wire(uint32_t from, uint32_t to) {
		Operation operation;
		operation.Kind = OperationKind::Connect;
		operation.From = from;
		operation.To = to;
		return operation;
	}

	const std::vector<std::byte> &CanonicalCubeBytes() {
		static const std::vector<std::byte> bytes = [] {
			auto mesh = engine::assets::MakeBuiltin(engine::assets::BuiltinMesh::Cube);
			if (!FitMesh(mesh, 2.0f)) throw std::runtime_error("bake benchmark canonical cube fit refused");
			engine::core::ByteWriter writer;
			if (!engine::assets::Mesh::Write(writer, mesh))
				throw std::runtime_error("bake benchmark canonical cube export refused");
			return std::vector<std::byte>(writer.Bytes().begin(), writer.Bytes().end());
		}();
		return bytes;
	}

	Document Make(size_t branchCount) {
		Document document;
		for (size_t index = 0; index < branchCount; ++index) {
			const uint32_t first = static_cast<uint32_t>(index * 3 + 1);
			document.Record(Named(OperationKind::AddBuiltin, "engine.Cube"));
			Operation fit;
			fit.Kind = OperationKind::AddFit;
			fit.Number = 2.0f;
			document.Record(fit);
			document.Record(Named(OperationKind::AddWrite, "cube" + std::to_string(index) + ".amesh"));
			document.Record(Wire(first, first + 1));
			document.Record(Wire(first + 1, first + 2));
		}
		Graph graph;
		std::string offender;
		if (Build(document, graph, {}, offender) != DocumentStatus::Ok)
			throw std::runtime_error("bake benchmark build refused: " + offender);
		std::string failure;
		if (!graph.Run(failure) || graph.Baked().size() != branchCount)
			throw std::runtime_error("bake benchmark run refused: " + failure);
		for (size_t index = 0; index < branchCount; ++index) {
			const auto &canonical = CanonicalCubeBytes();
			if (graph.Baked()[index].Name != "cube" + std::to_string(index) + ".amesh" ||
				graph.Baked()[index].Kind != engine::assets::AssetKind::Mesh ||
				graph.Baked()[index].Bytes != canonical)
				throw std::runtime_error("bake benchmark export changed");
			engine::core::ByteReader reader(graph.Baked()[index].Bytes);
			engine::assets::MeshData mesh;
			if (!engine::assets::Mesh::Read(reader, mesh) || !mesh.IsValid())
				throw std::runtime_error("bake benchmark mesh export is invalid");
		}
		return document;
	}

	const Document EMPTY = Make(0);
	const Document SIMPLE = Make(1);
	const Document BUSY = Make(8);
	const Document COMPLEX = Make(32);

	void BuildAndRun(const Document &document) {
		Graph graph;
		std::string offender;
		if (Build(document, graph, {}, offender) != DocumentStatus::Ok)
			throw std::runtime_error("bake graph build refused: " + offender);
		std::string failure;
		if (!graph.Run(failure)) throw std::runtime_error("bake graph run refused: " + failure);
		Consume(graph.Baked().size());
		if (!graph.Baked().empty()) Consume(graph.Baked().front().Bytes.data());
	}

	void ValidateVocabulary() {
		engine::graphcore::Diagnostic diagnostic;
		constexpr engine::graphcore::Limits limits{
			.MaximumNodes = 4096,
			.MaximumConnections = 4096,
			.MaximumSchemas = 64,
			.MaximumPorts = 256,
			.MaximumIdentityBytes = 128 * 1024
		};
		if (engine::graphcore::ValidateSchemas(NodeSchemas(), limits, diagnostic) !=
			engine::graphcore::CompileStatus::Ok)
			throw std::runtime_error("bake node schema validation refused");
		Consume(NodeSchemas().size());
	}

	template <class Work> void ProbeHeap(const char *name, Work work) {
		using engine::core::HeapProfile;
		if (!HeapProfile::IsCompiledIn())
			throw std::runtime_error("bake heap probe requires MONO_HEAP_PROFILE");
		const auto before = HeapProfile::Totals();
		{
			ENGINE_HEAP_SCOPE(name);
			work();
		}
		const auto after = HeapProfile::Totals();
		int64_t peak = 0;
		bool found = false;
		for (uint32_t index = 1; index < HeapProfile::NodeCount(); ++index) {
			const auto node = HeapProfile::Node(index);
			if (node.Parent == HeapProfile::ROOT && node.Name == name) {
				found = true;
				peak = node.PeakBytes;
			}
		}
		if (!found) throw std::runtime_error("benchmark heap scope was not recorded");
		std::cout << "# b0 heap\t" << name << '\t' << (after.TotalBytes - before.TotalBytes) << '\t'
				  << (after.TotalBlocks - before.TotalBlocks) << '\t' << (after.LiveBytes - before.LiveBytes)
				  << '\t' << after.LiveBytes << '\t' << peak << '\n';
	}

	void ProbeMemoryOnce() {
		static const bool probed = [] {
			if (const char *setting = std::getenv("MONO_BENCH_MEMORY");
				setting != nullptr && setting[0] == '1' && setting[1] == '\0') {
				std::cout
					<< "# b0 heap "
					   "v1\tname\tallocated_bytes\tallocated_blocks\tretained_bytes\tlive_bytes\tscope_peak_"
					   "bytes\n";
				ProbeHeap("b0 bake vocabulary", [] { ValidateVocabulary(); });
				ProbeHeap("b0 bake empty", [] { BuildAndRun(EMPTY); });
				ProbeHeap("b0 bake simple", [] { BuildAndRun(SIMPLE); });
				ProbeHeap("b0 bake busy", [] { BuildAndRun(BUSY); });
				ProbeHeap("b0 bake complex", [] { BuildAndRun(COMPLEX); });
			}
			return true;
		}();
		(void)probed;
	}
}

BENCH("ValidateSchemas bake vocabulary", 1) {
	ProbeMemoryOnce();
	ValidateVocabulary();
}
BENCH("Build and Run empty accepted", 1) {
	BuildAndRun(EMPTY);
}
BENCH("Build and Run builtin mesh", 1) {
	BuildAndRun(SIMPLE);
}
BENCH("Build and Run busy 8 meshes", 1) {
	BuildAndRun(BUSY);
}
BENCH("Build and Run complex 32 meshes", 1) {
	BuildAndRun(COMPLEX);
}
