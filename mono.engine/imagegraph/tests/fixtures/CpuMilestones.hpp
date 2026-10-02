#pragma once

#include "ValuePayload.hpp"

#include <engine/imagegraph/FeedbackHost.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace engine::imagegraph::testing {
	// Authored workloads share setup with the correctness suite. Each timed run
	// starts fresh replay.
	struct CpuMilestoneFixture {
		enum class Kind { Static2D, Animation, Feedback, Simulation, Mesh3D };
		static constexpr uint32_t Side = 128;
		static constexpr uint64_t Frames = 16, Seed = 42;
		Kind Workload;
		Document Authored;
		Plan Compiled;
		StatefulOutputEvaluationResult Evaluated;
		CapturedFeedbackHost Replay;
		EvaluationRequest Request;
		Diagnostic Failure;
		std::array<std::array<uint8_t, 4>, Frames> FirstPixels{};
		std::array<std::string, 1> Outputs{"out"};

		explicit CpuMilestoneFixture(Kind kind) : Workload(kind) {
			Authored.FormatVersion = 9;
			Authored.Project = ProjectSettings{};
			Authored.Project->SurfaceWidth = Authored.Project->SurfaceHeight = Side;
			Authored.Timeline = TimelineSettings{Frames, 0, Frames - 1, "loop", 24};
			if (kind == Kind::Static2D || kind == Kind::Animation) {
				Authored.Nodes = {
					{"solid",
					 "image.solid",
					 "",
					 {},
					 {{"width", int64_t{Side}},
					  {"height", int64_t{Side}},
					  {"colour", Colour{64, 128, 192, 255}}}},
					{"invert", "image.invert", "", {}, {{"include_alpha", false}}}
				};
				Authored.Links = {{"solid", "image", "invert", "image"}};
				Authored.Outputs = {{"out", "invert", "image"}};
				if (kind == Kind::Animation) {
					Keyframe first;
					first.NodeId = "solid";
					first.Port = "colour";
					first.Data = Colour{0, 64, 128, 255};
					first.Interpolation = "linear";
					Keyframe last = first;
					last.Tick = Frames - 1;
					last.Data = Colour{255, 64, 128, 255};
					Authored.Keyframes = {first, last};
				}
			} else if (kind == Kind::Feedback) {
				Authored.Nodes = {
					{"prior", "image.captured", "", {}, {{"source_id", std::string{"feedback:out"}}}},
					{"invert", "image.invert", "", {}, {{"include_alpha", true}}}
				};
				Authored.Links = {{"prior", "image", "invert", "image"}};
				Authored.Outputs = {{"out", "invert", "image"}};
			} else if (kind == Kind::Simulation) {
				Authored.Nodes = {
					{"grid",
					 "pc.verlet_sim_mesh_grid",
					 "",
					 {},
					 {{"subdivision", Vector2{8, 8}},
					  {"area_unit", EnumValue{0}},
					  {"area", Area{16, 16, 16, 16}}}},
					{"step",
					 "image.verlet_simple",
					 "",
					 {},
					 {{"substep", int64_t{4}}, {"gravity", Vector2{0, .25}}}}
				};
				Authored.Links = {{"grid", "mesh", "step", "mesh"}};
				Authored.Outputs = {{"out", "step", "mesh"}};
			} else {
				Authored.Nodes = {
					{"sphere",
					 "pc.3_d_mesh_sphere_ico",
					 "",
					 {},
					 {{"subdivision", int64_t{2}}, {"smooth_normal", true}}},
					{"transform",
					 "pc.3_d_transform",
					 "",
					 {},
					 {{"position", Vector3{2, 3, 4}}, {"scale", Vector3{1, 1, 1}}}}
				};
				Authored.Links = {{"sphere", "mesh", "transform", "mesh"}};
				Authored.Outputs = {{"out", "transform", "mesh"}};
			}
			if (Compile(Authored, Compiled, Failure) != Status::Ok) Fail("compile");
		}
		[[noreturn]] void Fail(const char *stage) const {
			throw std::runtime_error(std::string("CPU milestone ") + stage + ": " + Failure.Message);
		}
		const std::variant<Image, ImageArray, EvaluatedValue> &Output() const {
			if (Workload == Kind::Feedback || Workload == Kind::Simulation) {
				const auto *output = Replay.Value("out");
				if (!output) Fail("missing host output");
				return output->Output;
			}
			if (Evaluated.Outputs.size() != 1) Fail("missing evaluated output");
			return Evaluated.Outputs.front().Output;
		}
		const Image &ImageOutput() const {
			return std::get<Image>(Output());
		}
		const MeshValue2D &SimulationOutput() const {
			return std::get<MeshValue2D>(std::get<EvaluatedValue>(Output()).Data);
		}
		const MeshValue3D &MeshOutput() const {
			return std::get<MeshValue3D>(std::get<EvaluatedValue>(Output()).Data);
		}
		void Run() {
			// Clear is part of the measured reset-and-stream operation, not graph
			// compilation.
			Replay.Clear();
			Request = {};
			Request.Seed = Seed;
			const uint64_t count = Workload == Kind::Static2D || Workload == Kind::Mesh3D ? 1 : Frames;
			for (uint64_t tick = 0; tick < count; ++tick) {
				Request.Tick = tick;
				if (Workload == Kind::Feedback || Workload == Kind::Simulation) {
					if (!Replay.Prepare(
							Authored, Compiled, 1, 1, Request, Failure, Limits::MaximumEvaluationBytes, "out"
						))
						Fail("replay");
				} else if (EvaluateStatefulOutputs(
							   Authored, Compiled, Outputs, Request, Evaluated, Failure
						   ) != Status::Ok)
					Fail("evaluate");
				if (Workload == Kind::Static2D || Workload == Kind::Animation || Workload == Kind::Feedback)
					std::copy_n(ImageOutput().Pixels.begin(), 4, FirstPixels[tick].begin());
			}
		}
		uint64_t RetainedOutputBytes() const {
			return std::visit(
				[](const auto &value) -> uint64_t {
					using T = std::decay_t<decltype(value)>;
					if constexpr (std::is_same_v<T, EvaluatedValue>)
						return std::visit(
							[](const auto &payload) { return detail::RetainedPayloadBytes(payload); },
							value.Data
						);
					else
						return detail::RetainedPayloadBytes(value);
				},
				Output()
			);
		}
		uint64_t RetainedReplayBytes() const {
			return Request.SimulationReplay ? RetainedSimulationReplayBytes(*Request.SimulationReplay) : 0;
		}
		uint64_t Fingerprint() const {
			uint64_t hash = 14695981039346656037ULL;
			const auto word = [&](uint64_t bits) {
				for (size_t byte = 0; byte < 8; ++byte) {
					hash ^= (bits >> (byte * 8)) & 255;
					hash *= 1099511628211ULL;
				}
			};
			if (Workload == Kind::Simulation) {
				for (const auto &point : SimulationOutput().Data->Simulation.Points) {
					word(std::bit_cast<uint64_t>(point.Position.X));
					word(std::bit_cast<uint64_t>(point.Position.Y));
				}
			} else if (Workload == Kind::Mesh3D) {
				for (const auto &part : MeshOutput().Data->Parts)
					for (const auto &vertex : part.Vertices) {
						word(std::bit_cast<uint64_t>(vertex.Position.X));
						word(std::bit_cast<uint64_t>(vertex.Position.Y));
						word(std::bit_cast<uint64_t>(vertex.Position.Z));
					}
			} else
				for (uint8_t byte : ImageOutput().Pixels) {
					hash ^= byte;
					hash *= 1099511628211ULL;
				}
			return hash;
		}
	};
} // namespace engine::imagegraph::testing
