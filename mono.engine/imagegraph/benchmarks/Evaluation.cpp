// Measures bounded deterministic CPU evaluation of an authored image graph.

#include <engine/imagegraph/Document.hpp>
#include <engine/testing/Bench.hpp>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.imagegraph.bench.evaluation")

namespace {
	using engine::imagegraph::Compile;
	using engine::imagegraph::Diagnostic;
	using engine::imagegraph::Document;
	using engine::imagegraph::Evaluate;
	using engine::imagegraph::Image;
	using engine::imagegraph::Node;
	using engine::imagegraph::Plan;
	using engine::imagegraph::Status;

	constexpr uint32_t IMAGE_SIDE = 256;

	struct EvaluationFixture {
		Document Source;
		Plan Compiled;
		Image Output;
		Diagnostic Failure;

		EvaluationFixture() {
			Node noise;
			noise.Id = "noise";
			noise.Type = "image.noise_simplex";
			noise.Values = {
				{"width", int64_t{IMAGE_SIDE}},
				{"height", int64_t{IMAGE_SIDE}},
				{"iterations", int64_t{3}},
				{"seed", 42.0},
			};
			Source.Nodes.push_back(std::move(noise));
			Source.Outputs.push_back({"output", "noise", "image"});

			if (Compile(Source, Compiled, Failure) != Status::Ok) {
				throw std::runtime_error("imagegraph benchmark compile failed: " + Failure.Message);
			}

			Image first;
			Image second;
			if (Evaluate(Source, Compiled, "output", first, Failure) != Status::Ok ||
				Evaluate(Source, Compiled, "output", second, Failure) != Status::Ok) {
				throw std::runtime_error("imagegraph benchmark preflight failed: " + Failure.Message);
			}
			if (first.Width != IMAGE_SIDE || first.Height != IMAGE_SIDE || first.Pixels != second.Pixels ||
				first.Hash != second.Hash) {
				throw std::runtime_error("imagegraph benchmark preflight was not deterministic");
			}
			Output = std::move(second);
		}

		void Run() {
			if (Evaluate(Source, Compiled, "output", Output, Failure) != Status::Ok) {
				throw std::runtime_error("imagegraph benchmark evaluation failed: " + Failure.Message);
			}
			engine::testing::Consume(Output.Hash);
		}
	};

	EvaluationFixture &Fixture() {
		static EvaluationFixture fixture;
		return fixture;
	}
}

BENCH("256x256 three-octave simplex evaluation", 1) {
	Fixture().Run();
}
