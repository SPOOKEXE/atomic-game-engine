#pragma once

#include "ValuePayload.hpp"

#include <engine/imagegraph/SourceBuiltinRandom.hpp>
#include <engine/imagegraph/StatefulReplay.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace engine::imagegraph::testing {
	struct SourcePipelineFixture {
		enum class Kind { Fft, Path, Strand };
		Kind Workload;
		Document Authored;
		Plan Compiled;
		std::array<Document, 9> PathGraphs;
		std::array<Plan, 9> PathPlans;
		StatefulEvaluationResult Evaluated;
		Diagnostic Failure;
		SourceBuiltinRandomCapture Capture;
		std::vector<double> Actual, Expected;
		uint64_t InputFingerprint = 14695981039346656037ull;
		static constexpr uint32_t Side = 256;
		static constexpr uint64_t Frames = 8, Seed = 123456;
		explicit SourcePipelineFixture(Kind kind) : Workload(kind) {
			Authored.FormatVersion = 9;
			Authored.Timeline = TimelineSettings{Frames, 0, Frames - 1, "loop", 24};
			if (kind == Kind::Fft) {
				ArrayValue samples{ValueType::Scalar, {}};
				for (size_t i = 0; i < 256; ++i)
					samples.Elements.emplace_back(i == 64 ? 1. : 0.);
				Authored.Nodes = {
					{"fft",
					 "pc.fft",
					 "",
					 {},
					 {{"data", samples},
					  {"preprocess_function", ArrayValue{ValueType::Scalar, {.5, 1.5, 0x1p32 + 1.5}}}}}
				};
				Authored.Outputs = {{"out", "fft", "array"}};
				for (double gain : {1., .5, .5})
					for (size_t i = 0; i < 129; ++i)
						Expected.push_back(gain);
			} else if (kind == Kind::Path) {
				Path2D line;
				line.Anchors = {{{2, 4, 0, 0, 0, 0}, 0}, {{12, 14, 0, 0, 0, 0}, 0}};
				Curve reverse;
				reverse.Header = {0, 1, 0, 0, 1, 0};
				reverse.Anchors = {{{0, 0, 0, 1, 1. / 3, -1. / 3}}, {{-1. / 3, 1. / 3, 1, 0, 0, 0}}};
				ArrayValue ratios{ValueType::Scalar, {}};
				for (size_t i = 0; i < 64; ++i) {
					const double r = (i + .5) / 64;
					ratios.Elements.emplace_back(r);
				}
				Authored.Nodes = {
					{"transform",
					 "pc.path_transform",
					 "",
					 {},
					 {{"path", line},
					  {"position", Vector2{2, 5}},
					  {"position_unit", EnumValue{0}},
					  {"anchor", Vector2{1, 3}},
					  {"anchor_unit", EnumValue{0}},
					  {"scale", Vector2{2, -1}},
					  {"rotation", 90.}}},
					{"map",
					 "pc.path_map_area",
					 "",
					 {},
					 {{"map_from", EnumValue{0}},
					  {"map_to", EnumValue{0}},
					  {"area", Area{100, 200, -10, 20, 1, 1}}}},
					{"remap", "pc.path_redistribute", "", {}, {{"curve", reverse}}},
					{"sample", "pc.path_sample", "", {}, {{"ratio", ratios}}}
				};
				Authored.Links = {
					{"transform", "path", "map", "path"},
					{"map", "path", "remap", "path"},
					{"remap", "path", "sample", "path"}
				};
				Authored.Outputs = {{"out", "sample", "position"}};
				for (size_t from = 0; from < 3; ++from)
					for (size_t to = 0; to < 3; ++to) {
						const size_t mode = from * 3 + to;
						auto &d = PathGraphs[mode];
						d = Authored;
						d.Nodes[1].Values = {
							{"map_from", EnumValue{int64_t(from)}},
							{"map_to", EnumValue{int64_t(to)}},
							{"dimension_from", Vector2{20, 40}},
							{"dimension_from_unit", EnumValue{0}},
							{"bbox_from", Vector4{-18, -34, 22, 46}},
							{"area", Area{100, 200, -10, 20, 1, 1}},
							{"dimension_to", Vector2{60, 100}},
							{"dimension_to_unit", EnumValue{0}},
							{"bbox_to", Vector4{5, 15, -15, 55}}
						};
						if (Compile(d, PathPlans[mode], Failure) != Status::Ok) Fail("path mode compile");
						const Vector2 minimum = from == 0	? Vector2{-8, -14}
												: from == 1 ? Vector2{0, 0}
															: Vector2{-18, -34};
						const Vector2 extent = from == 0   ? Vector2{10, 20}
											   : from == 1 ? Vector2{20, 40}
														   : Vector2{40, 80};
						const Vector2 center = to == 0	 ? Vector2{100, 200}
											   : to == 1 ? Vector2{30, 50}
														 : Vector2{-5, 35};
						const Vector2 half = to == 1 ? Vector2{30, 50} : Vector2{-10, 20};
						for (size_t i = 0; i < 64; ++i) {
							const double r = (i + .5) / 64;
							const Vector2 transformed{-8 + 10 * r, -14 + 20 * r};
							Expected.push_back(
								center.X - half.X + 2 * half.X * (transformed.X - minimum.X) / extent.X
							);
							Expected.push_back(
								center.Y - half.Y + 2 * half.Y * (transformed.Y - minimum.Y) / extent.Y
							);
						}
					}

			} else {
				Authored.Nodes = {
					{"create",
					 "pc.strand_create",
					 "",
					 {},
					 {{"strands", int64_t{64}},
					  {"segment", int64_t{1}},
					  {"position_unit", EnumValue{0}},
					  {"position", Vector2{0, 0}},
					  {"length", Vector2{0, 0}},
					  {"elasticity", 1.},
					  {"spring", 1.},
					  {"structure", 0.},
					  {"restitution", .01}}},
					{"first", "pc.strand_gravity", "", {}, {{"gravity", 1.}}},
					{"second", "pc.strand_gravity", "", {}, {{"gravity", 2.}}},
					{"update", "pc.strand_update", "", {}, {{"step", int64_t{1}}}}
				};
				Authored.Links = {
					{"create", "strands", "first", "input_0"},
					{"first", "strands", "second", "input_0"},
					{"second", "strands", "update", "input_0"}
				};
				Authored.Outputs = {{"out", "update", "strands"}};
				double y = 0, delta = 0;
				for (size_t frame = 0; frame < Frames; ++frame) {
					delta = 2 * delta + 3;
					y += delta;
					for (size_t hair = 0; hair < 64; ++hair)
						for (double v : {0., 0., 0., y, 0., delta})
							Expected.push_back(v);
				}
			}
			if (Compile(Authored, Compiled, Failure) != Status::Ok) Fail("compile");
			if (kind == Kind::Strand) {
				EvaluationRequest request;
				request.SimulationAuthoringRevision = 1;
				if (PrepareSourceBuiltinRandomCapture(
						Authored, Compiled, "create", request, Capture, Failure
					) != Status::Ok)
					Fail("capture controls");
				for (uint32_t i = 0; i < 64; ++i) {
					Capture.Draws.push_back({SourceBuiltinRandomOperation::RandomRange, 0, 0, 0});
					Capture.Draws.push_back(
						{SourceBuiltinRandomOperation::IRandomRange, 100000, 999999, double(Seed + i)}
					);
				}
			}

			const auto hashText = [&](const std::string &text) {
				for (unsigned char byte : text) {
					InputFingerprint ^= byte;
					InputFingerprint *= 1099511628211ull;
				}
			};
			hashText(Write(Authored));
			if (kind == Kind::Path)
				for (const auto &doc : PathGraphs)
					hashText(Write(doc));
			for (const auto &draw : Capture.Draws)
				hashText(std::to_string(draw.Result));
			Actual.reserve(Expected.size());
		}
		[[noreturn]] void Fail(std::string_view operation) const {
			throw std::runtime_error(std::string(operation) + ": " + Failure.Message);
		}
		void Run() {
			Actual.clear();
			Evaluated = {};
			EvaluationRequest request;
			request.Seed = Seed;
			request.SimulationAuthoringRevision = 1;
			for (uint64_t tick = 0; tick < (Workload == Kind::Strand ? Frames : 1); ++tick) {
				request.Tick = tick;
				request.DataReplay = tick ? &Evaluated.Data : nullptr;
				request.BuiltinRandomCaptures =
					tick || Workload != Kind::Strand
						? std::span<const SourceBuiltinRandomCapture>{}
						: std::span<const SourceBuiltinRandomCapture>{&Capture, 1};

				if (Workload == Kind::Path) {
					for (size_t mode = 0; mode < 9; ++mode) {
						if (EvaluateStateful(
								PathGraphs[mode], PathPlans[mode], "out", request, Evaluated, Failure
							) != Status::Ok)
							Fail("path modes evaluate");
						const auto &array =
							std::get<ArrayValue>(std::get<EvaluatedValue>(Evaluated.Output).Data);
						for (const auto &v : array.Elements) {
							const auto p = std::get<Vector2>(v);
							Actual.push_back(p.X);
							Actual.push_back(p.Y);
						}
					}
					continue;
				}
				if (EvaluateStateful(Authored, Compiled, "out", request, Evaluated, Failure) != Status::Ok)
					Fail("evaluate");
				const auto &value = std::get<EvaluatedValue>(Evaluated.Output).Data;
				if (Workload == Kind::Fft) {
					for (const auto &row : std::get<ArrayValue>(value).Nested)
						for (const auto &v : row)
							Actual.push_back(std::get<double>(v));
				} else {
					const auto &hairs = std::get<StrandValue>(value).Data->State.Hairs;
					if (hairs.size() != 64) Fail("strand count");
					for (const auto &hair : hairs) {
						if (hair.Points.size() != 2) Fail("strand segments");
						const auto &root = hair.Points[0], &end = hair.Points[1];
						for (double v :
							 {root.Position[0],
							  root.Position[1],
							  end.Position[0],
							  end.Position[1],
							  end.Delta[0],
							  end.Delta[1]})
							Actual.push_back(v);
					}
				}
			}
		}
		static uint64_t Hash(const std::vector<double> &values) {
			uint64_t hash = 14695981039346656037ull;
			for (double value : values) {
				// Analytical fixture coordinates lie on a binary grid, away from rounding ties.
				const uint64_t word = uint64_t(int64_t(std::llround(value * 1024)));
				for (unsigned byte = 0; byte < 8; ++byte) {
					hash ^= uint8_t(word >> (byte * 8));
					hash *= 1099511628211ull;
				}
			}
			return hash;
		}
		void Validate() const {
			if (Actual.size() != Expected.size()) Fail("output shape");
			for (size_t i = 0; i < Actual.size(); ++i)
				if (!std::isfinite(Actual[i]) || std::abs(Actual[i] - Expected[i]) > 1e-6)
					Fail("analytic output");
			if (Fingerprint() != Hash(Expected)) Fail("known output hash");
		}
		uint64_t Fingerprint() const {
			return Hash(Actual);
		}
		const auto &Output() const {
			return Actual;
		}
		uint64_t RetainedOutputBytes() const {
			return detail::RetainedPayloadBytes(std::get<EvaluatedValue>(Evaluated.Output).Data);
		}
		uint64_t RetainedReplayBytes() const {
			return RetainedDataReplayBytes(Evaluated.Data);
		}
	};
}
