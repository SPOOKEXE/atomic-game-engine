#include <engine/core/FrameGraph.hpp>
#include <engine/core/HeapProfile.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/imagegraph/Document.hpp>
#include <engine/testing/Bench.hpp>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>

TEST_SUITE_ID("engine.imagegraph.bench.source-camera")
namespace {
	using namespace engine::imagegraph;
	using engine::core::FrameGraph;
	using engine::core::HeapProfile;
	using engine::core::Metrics;
	constexpr uint64_t BUDGET = 4 * 1024 * 1024;
	void Check(bool valid, const char *message) {
		if (!valid) throw std::runtime_error(message);
	}
	enum class Kind {
		SpaceNearest,
		CameraLinear,
		TwoLayersNearest,
		TwoLayersLinear,
		Dof400,
		Float32,
		Refusal
	};
	struct Fixture {
		Kind Workload;
		Document Authored;
		Plan Compiled;
		std::array<RequestImageSource, 2> Inputs;
		EvaluationRequest Request;
		Image Output;
		Diagnostic Failure;
		uint32_t Side = 16;
		size_t Layers = 1;
		uint64_t ExpectedHash = 0, InputHash = 0;
		explicit Fixture(Kind kind) : Workload(kind) {
			Side = kind == Kind::Dof400 ? 8 : 16;
			Layers = kind == Kind::TwoLayersNearest || kind == Kind::TwoLayersLinear ? 2 : 1;
			Authored.FormatVersion = 9;
			Authored.Project.emplace();
			Authored.Project->SurfaceWidth = Authored.Project->SurfaceHeight = Side;
			Authored.Project->Interpolation = Linear() ? 1 : 0;
			for (size_t layer = 0; layer < Layers; ++layer) {
				auto &input = Inputs[layer];
				input.SourceId = "layer" + std::to_string(layer);
				input.Data.Width = input.Data.Height = Side;
				input.Data.Pixels.reserve(Side * Side * 4);
				for (uint32_t y = 0; y < Side; ++y)
					for (uint32_t x = 0; x < Side; ++x) {
						input.Data.Pixels.push_back(uint8_t((x * 17 + y * 11 + layer * 31) % 256));
						input.Data.Pixels.push_back(uint8_t((x * 7 + y * 23 + layer * 53) % 256));
						input.Data.Pixels.push_back(uint8_t((x * 29 + y * 3 + layer * 13) % 256));
						input.Data.Pixels.push_back(layer ? 128 : 255);
					}
				input.Data.Hash = SurfaceHash(input.Data);
				InputHash ^= input.Data.Hash;
				Authored.Nodes.push_back(
					{input.SourceId, "image.captured", {}, {}, {{"source_id", input.SourceId}}}
				);
			}
			Node camera{"camera", "pc.camera"};
			camera.Values = {
				{"camera_size", Vector2{double(Side), double(Side)}},
				{"camera_size_unit", EnumValue{0}},
				{"scene_size", Vector2{double(Side), double(Side)}},
				{"scene_size_unit", EnumValue{0}},
				{"focus_center", Vector2{double(Side) / 2, double(Side) / 2}},
				{"focus_center_unit", EnumValue{0}},
				{"zoom", kind == Kind::CameraLinear || kind == Kind::TwoLayersLinear ? 1.5 : 1.0},
				{"depth_of_field", kind == Kind::Dof400},
				{"focal_distance", 0.0},
				{"focal_range", 0.0},
				{"defocus", 1.0},
				{"attribute_color_depth", EnumValue{kind == Kind::Float32 ? 5 : 3}},
				{"attribute_array_process", EnumValue{0}},
				{"attribute_process", true}
			};
			for (size_t layer = 0; layer < Layers; ++layer) {
				const std::string suffix = "_" + std::to_string(layer);
				const int64_t positioning =
					kind == Kind::CameraLinear || kind == Kind::TwoLayersLinear ? 1 : 0;
				const int64_t mode =
					kind == Kind::SpaceNearest || kind == Kind::Float32 || kind == Kind::Refusal ? 0
					: kind == Kind::TwoLayersNearest											 ? 2
					: kind == Kind::TwoLayersLinear												 ? 3
																								 : 1;
				camera.DynamicInputs.push_back({"element" + suffix, ValueType::Image, std::nullopt});
				camera.DynamicInputs.push_back(
					{"positioning" + suffix, ValueType::Enum, EnumValue{positioning}}
				);
				camera.DynamicInputs.push_back(
					{"position" + suffix,
					 ValueType::Vector2,
					 positioning ? Vector2{-double(Side) / 2 + .25, -double(Side) / 2 + .25} : Vector2{}}
				);
				camera.DynamicInputs.push_back({"oversample" + suffix, ValueType::Enum, EnumValue{mode}});
				camera.DynamicInputs.push_back({"parallax" + suffix, ValueType::Vector2, Vector2{}});
				camera.DynamicInputs.push_back(
					{"depth" + suffix, ValueType::Scalar, kind == Kind::Dof400 ? 10.0 : 0.0}
				);
				camera.Values.push_back({"position" + suffix + "_unit", EnumValue{0}});
				Authored.Links.push_back({Inputs[layer].SourceId, "image", "camera", "element" + suffix});
			}
			Authored.Nodes.push_back(std::move(camera));
			Authored.Outputs = {{"result", "camera", "surface_out"}};
			Request.ImageSources = std::span(Inputs).first(Layers);
			const auto compilationStatus = Compile(Authored, Compiled, Failure);
			Check(compilationStatus == Status::Ok, Failure.Message.c_str());
			if (kind == Kind::Refusal) Output = Image{1, 1, {12, 34, 56, 78}};
			Run();
			Verify();
			ExpectedHash = SurfaceHash(Output);
			if (kind != Kind::Refusal && kind != Kind::SpaceNearest && kind != Kind::Float32)
				Check(
					Output.Pixels != Inputs[0].Data.Pixels,
					"camera transformed workload lost its image effect"
				);
			if (kind == Kind::SpaceNearest)
				Check(Output.Pixels == Inputs[0].Data.Pixels, "camera nearest identity changed input pixels");
		}
		bool Linear() const {
			return Workload == Kind::CameraLinear || Workload == Kind::TwoLayersLinear ||
				   Workload == Kind::Dof400;
		}
		uint64_t ExpectedTaps() const {
			return Workload == Kind::Refusal
					   ? 0
					   : uint64_t(Side) * Side * Layers * (Workload == Kind::Dof400 ? 400 : 1);
		}
		uint64_t ExpectedStores() const {
			return Workload == Kind::Refusal
					   ? 0
					   : uint64_t(Side) * Side * Layers * (Workload == Kind::Float32 ? 16 : 4);
		}
		void Run() {
			const auto status = Evaluate(
				Authored, Compiled, "result", Request, Output, Failure, Workload == Kind::Refusal ? 1 : BUDGET
			);
			Check(
				status == (Workload == Kind::Refusal ? Status::LimitExceeded : Status::Ok),
				Failure.Message.c_str()
			);
		}
		void Verify() const {
			if (Workload == Kind::Refusal) {
				Check(
					Output.Width == 1 && Output.Height == 1 &&
						Output.Pixels == std::vector<uint8_t>{12, 34, 56, 78},
					"camera budget refusal replaced prior pixels"
				);
			} else {
				Check(
					Output.Width == Side && Output.Height == Side && FiniteSurfaceSamples(Output),
					"camera output shape or samples changed"
				);
				Check(
					Output.Format ==
						(Workload == Kind::Float32 ? SurfaceFormat::RGBA32Float : SurfaceFormat::RGBA8Unorm),
					"camera typed output changed"
				);
			}
			if (ExpectedHash) Check(SurfaceHash(Output) == ExpectedHash, "camera output checksum changed");
		}
	};
	struct Reading {
		float Owner = 0, Unmarked = 0;
		engine::core::HeapTotals Before{}, After{};
		std::vector<std::pair<std::string, engine::core::FrameSpan>> Spans;
		std::vector<engine::core::Counter> Counters;
	};
	struct Profile {
		Fixture Graph;
		std::array<Reading, 13> Readings;
		size_t Count = 0;
		explicit Profile(Kind kind) : Graph(kind) {}
		void Measure() {
			Check(Count < Readings.size(), "camera profile requires eight warmups and at most five samples");
			struct Restore {
				bool Enabled = FrameGraph::IsEnabled();
				~Restore() {
					FrameGraph::SetEnabled(Enabled);
				}
			} restore;
			FrameGraph::SetEnabled(true);
			Metrics::Drain();
			auto &reading = Readings[Count];
			reading.Before = HeapProfile::Totals();
			FrameGraph::BeginFrame();
			try {
				ENGINE_PROFILE("imagegraph.camera_bench.sample");
				Graph.Run();
			} catch (...) {
				FrameGraph::EndFrame();
				throw;
			}
			FrameGraph::EndFrame();
			reading.After = HeapProfile::Totals();
			reading.Owner = FrameGraph::FrameMilliseconds();
			reading.Unmarked = FrameGraph::UnmarkedMilliseconds();
			Check(
				!FrameGraph::Dropped() && reading.Before.DroppedScopes == reading.After.DroppedScopes,
				"camera profiling dropped spans"
			);
			size_t roots = 0, evaluations = 0, cameras = 0;
			const auto &spans = FrameGraph::Spans();
			for (size_t index = 0; index < spans.size(); ++index) {
				const auto &span = spans[index];
				Check(!span.Reported, "camera CPU sample includes reported worker time");
				if (span.Parent == FrameGraph::NO_PARENT) {
					Check(
						span.Depth == 0 && span.Name == "imagegraph.camera_bench.sample",
						"camera sample root is incomplete"
					);
					++roots;
				} else
					Check(
						span.Parent < index && span.Depth == spans[span.Parent].Depth + 1,
						"camera sample hierarchy is invalid"
					);
				evaluations += span.Name == "imagegraph.evaluate";
				cameras += span.Name == "imagegraph.camera.render";
				reading.Spans.emplace_back(std::string(span.Name), span);
			}
			Check(
				roots == 1 && evaluations == 1 && cameras == (Graph.Workload == Kind::Refusal ? 0 : 1),
				"camera sample missed its actual compiled evaluation"
			);
			reading.Counters = Metrics::Drain();
			const auto total = [&](std::string_view name) {
				double value = 0;
				for (const auto &counter : reading.Counters)
					if (counter.Name.Text() == name) value += counter.Value;
				return value;
			};
			Check(
				total("imagegraph.camera.sample_taps") == Graph.ExpectedTaps(),
				"camera sample tap accounting changed"
			);
			Check(
				total("imagegraph.camera.output_bytes") == Graph.ExpectedStores(),
				"camera sample written-byte accounting changed"
			);
			Graph.Verify();
			++Count;
		}
		~Profile() {
			const char *preset = std::getenv("ATOMIC_IMAGEGRAPH_CAMERA_PRESET");
			for (size_t call = 0; call < Count; ++call) {
				const auto &reading = Readings[call];
				std::printf(
					"# camera-profile preset=%s backend=cpu kind=%u call=%zu warmup=%d side=%u layers=%zu "
					"filter=%s input_fnv=%llu output_fnv=%llu sample_taps=%llu written_bytes=%llu "
					"budget_bytes=%llu owner_ms=%.6f unmarked_ms=%.6f heap_compiled=%d allocated_bytes=%llu "
					"allocated_blocks=%llu live_bytes_delta=%lld process_live_bytes=%lld "
					"process_peak_bytes=%lld profiler_overhead_bytes=%lld\n",
					preset ? preset : "unreported",
					unsigned(Graph.Workload),
					call + 1,
					call < 8,
					Graph.Side,
					Graph.Layers,
					Graph.Linear() ? "bilinear" : "nearest",
					(unsigned long long)Graph.InputHash,
					(unsigned long long)Graph.ExpectedHash,
					(unsigned long long)Graph.ExpectedTaps(),
					(unsigned long long)Graph.ExpectedStores(),
					(unsigned long long)(Graph.Workload == Kind::Refusal ? 1 : BUDGET),
					reading.Owner,
					reading.Unmarked,
					HeapProfile::IsCompiledIn(),
					(unsigned long long)(reading.After.TotalBytes - reading.Before.TotalBytes),
					(unsigned long long)(reading.After.TotalBlocks - reading.Before.TotalBlocks),
					(long long)(reading.After.LiveBytes - reading.Before.LiveBytes),
					(long long)reading.After.LiveBytes,
					(long long)reading.After.PeakBytes,
					(long long)reading.After.OverheadBytes
				);
				for (size_t index = 0; index < reading.Spans.size(); ++index) {
					const auto &[name, span] = reading.Spans[index];
					std::printf(
						"# camera-span kind=%u call=%zu index=%zu parent=%u depth=%u start_ms=%.6f "
						"inclusive_ms=%.6f self_ms=%.6f idle_ms=%.6f reported=%d name=%s\n",
						unsigned(Graph.Workload),
						call + 1,
						index,
						span.Parent,
						span.Depth,
						span.StartMilliseconds,
						span.Milliseconds,
						span.SelfMilliseconds,
						span.IdleMilliseconds,
						span.Reported,
						name.c_str()
					);
				}
				for (const auto &counter : reading.Counters)
					std::printf(
						"# camera-counter kind=%u call=%zu name=%.*s value=%.0f samples=%u\n",
						unsigned(Graph.Workload),
						call + 1,
						int(counter.Name.Text().size()),
						counter.Name.Text().data(),
						counter.Value,
						counter.Samples
					);
			}
		}
	};
}
BENCH("CPU Camera 16x16 one layer Space Empty nearest compiled-plan", 1) {
	static Profile p(Kind::SpaceNearest);
	p.Measure();
}
BENCH("CPU Camera 16x16 one layer Camera Repeat bilinear compiled-plan", 1) {
	static Profile p(Kind::CameraLinear);
	p.Measure();
}
BENCH("CPU Camera 16x16 two layers Space Repeat-X nearest compiled-plan", 1) {
	static Profile p(Kind::TwoLayersNearest);
	p.Measure();
}
BENCH("CPU Camera 16x16 two layers Camera Repeat-Y bilinear compiled-plan", 1) {
	static Profile p(Kind::TwoLayersLinear);
	p.Measure();
}
BENCH("CPU Camera 8x8 one layer DOF 400-tap bilinear compiled-plan", 1) {
	static Profile p(Kind::Dof400);
	p.Measure();
}
BENCH("CPU Camera 16x16 RGBA32Float typed output compiled-plan", 1) {
	static Profile p(Kind::Float32);
	p.Measure();
}
BENCH("CPU Camera 16x16 atomic byte-budget refusal compiled-plan", 1) {
	static Profile p(Kind::Refusal);
	p.Measure();
}
