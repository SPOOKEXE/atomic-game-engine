// Document validation and typed dependency compilation. The empty row is a
// documented refusal; accepted rows compile the complete authored graph.

#include <engine/core/HeapProfile.hpp>
#include <engine/imagegraph/Document.hpp>
#include <engine/testing/Bench.hpp>

#include <cstdlib>
#include <iostream>
#include <stdexcept>

TEST_SUITE_ID("engine.imagegraph.bench.compile")

namespace {
	using namespace engine::imagegraph;
	using engine::testing::Consume;

	Document Make(size_t count, bool parameterized) {
		Document graph;
		if (count == 0) return graph;
		graph.Nodes.push_back({"node0", Solid{1, 1, {17, 31, 47, 255}}, {}, {}});
		for (size_t index = 1; index < count; ++index)
			graph.Nodes.push_back(
				{"node" + std::to_string(index),
				 Flip{index % 2 == 0, false},
				 {"node" + std::to_string(index - 1)},
				 {}}
			);
		graph.Outputs.push_back({"main", graph.Nodes.back().Id});
		if (parameterized) {
			graph.Parameters.push_back({"colour", std::array<uint8_t, 4>{17, 31, 47, 255}});
			graph.Bindings.push_back({"node0", "colour", "colour"});
		}
		Plan plan;
		Diagnostic diagnostic;
		if (!Compile(graph, plan, diagnostic) || plan.Order.size() != count ||
			plan.Outputs != std::vector<size_t>{count - 1})
			throw std::runtime_error("imagegraph compile fixture refused: " + diagnostic.Message);
		for (size_t index = 0; index < count; ++index)
			if (plan.Order[index] != index ||
				(index > 0 && plan.Inputs[index] != std::vector<size_t>{index - 1}))
				throw std::runtime_error("imagegraph compile fixture order changed");
		Image pixels;
		if (!Evaluate(graph, plan, "main", {}, pixels, diagnostic) || pixels.Width != 1 ||
			pixels.Height != 1 ||
			pixels.Pixels !=
				std::vector<std::byte>{std::byte{17}, std::byte{31}, std::byte{47}, std::byte{255}})
			throw std::runtime_error("imagegraph fixture pixel output changed");
		return graph;
	}

	const Document EMPTY = Make(0, false);
	const Document SIMPLE = Make(1, true);
	const Document BUSY = Make(64, true);
	const Document COMPLEX = Make(128, true);

	void CompileAccepted(const Document &graph) {
		Plan plan;
		Diagnostic diagnostic;
		if (!Compile(graph, plan, diagnostic))
			throw std::runtime_error("imagegraph compile refused: " + diagnostic.Message);
		Consume(plan.Order.data());
		Consume(plan.Inputs.size());
	}

	void CompileRefused() {
		Plan plan;
		Diagnostic diagnostic;
		if (Compile(EMPTY, plan, diagnostic))
			throw std::runtime_error("imagegraph empty document unexpectedly accepted");
		Consume(diagnostic.Message.data());
	}

	void ResolveAndCompile(const Document &graph) {
		Document resolved;
		Diagnostic diagnostic;
		const std::array overrides{InputOverride{"colour", std::array<uint8_t, 4>{3, 5, 7, 255}}};
		if (!ResolveInputs(graph, overrides, resolved, diagnostic))
			throw std::runtime_error("imagegraph binding refused: " + diagnostic.Message);
		Plan plan;
		if (!Compile(resolved, plan, diagnostic))
			throw std::runtime_error("imagegraph resolved compile refused: " + diagnostic.Message);
		Consume(plan.Order.data());
	}

	[[maybe_unused]] const bool OVERRIDE_VERIFIED = [] {
		Document resolved;
		Diagnostic diagnostic;
		const std::array overrides{InputOverride{"colour", std::array<uint8_t, 4>{3, 5, 7, 255}}};
		Plan plan;
		Image pixels;
		if (!ResolveInputs(BUSY, overrides, resolved, diagnostic) || !Compile(resolved, plan, diagnostic) ||
			!Evaluate(resolved, plan, "main", {}, pixels, diagnostic) ||
			pixels.Pixels != std::vector<std::byte>{std::byte{3}, std::byte{5}, std::byte{7}, std::byte{255}})
			throw std::runtime_error("imagegraph bound pixel output changed");
		return true;
	}();

	template <class Work> void ProbeHeap(const char *name, Work work) {
		using engine::core::HeapProfile;
		if (!HeapProfile::IsCompiledIn())
			throw std::runtime_error("imagegraph heap probe requires MONO_HEAP_PROFILE");
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
				ProbeHeap("b0 imagegraph empty refusal", [] { CompileRefused(); });
				ProbeHeap("b0 imagegraph simple", [] { CompileAccepted(SIMPLE); });
				ProbeHeap("b0 imagegraph busy", [] { CompileAccepted(BUSY); });
				ProbeHeap("b0 imagegraph complex", [] { CompileAccepted(COMPLEX); });
				ProbeHeap("b0 imagegraph bound", [] { ResolveAndCompile(BUSY); });
			}
			return true;
		}();
		(void)probed;
	}
}

BENCH("Compile empty document refusal", 1) {
	ProbeMemoryOnce();
	CompileRefused();
}
BENCH("Compile simple solid output", 1) {
	CompileAccepted(SIMPLE);
}
BENCH("Compile busy 64 nodes", 1) {
	CompileAccepted(BUSY);
}
BENCH("Compile complex 128 nodes", 1) {
	CompileAccepted(COMPLEX);
}
BENCH("ResolveInputs plus Compile 64 nodes", 1) {
	ResolveAndCompile(BUSY);
}
