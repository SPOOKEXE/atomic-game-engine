#pragma once

#include "NodeExecutors.hpp"
#include "nodes/SourceParticle3DState.hpp"

#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/imagegraph/SourceParticle3DVertex.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace engine::imagegraph::testing {
	// grug fixed source inputs give independent line, motion and vertex oracles.
	// Particle cases measure CPU state and vertex helpers, before any raster draw.
	struct SourcePathParticleFixture {
		enum class Kind { PathLength, PathAmount, ParticleState, ParticleVertices, SpiralSamples };
		static constexpr uint32_t PoolCapacity = 512, SpawnPerFrame = 16, Frames = 16;
		static constexpr uint64_t Seed = 12345;
		static constexpr double PathLength = 128;
		static constexpr std::array<double, 16> OBJECT{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
		static constexpr std::array<MeshVertex3D, 6> QUAD{
			MeshVertex3D{{-1, -1, 0}, {0, 0, 1}, {0, 0}},
			MeshVertex3D{{1, -1, 0}, {0, 0, 1}, {1, 0}},
			MeshVertex3D{{1, 1, 0}, {0, 0, 1}, {1, 1}},
			MeshVertex3D{{-1, -1, 0}, {0, 0, 1}, {0, 0}},
			MeshVertex3D{{1, 1, 0}, {0, 0, 1}, {1, 1}},
			MeshVertex3D{{-1, 1, 0}, {0, 0, 1}, {0, 1}}
		};
		Kind Workload;
		Document Authored;
		Plan Compiled;
		EvaluatedValue Segments, SpiralPath, SpiralWeights;
		Node ParticleNode{"particles", "pc.3_d_particle", "", {}, {}};
		EvaluationRequest Request;
		detail::NodeContext ParticleContext{ParticleNode, *FindCatalogueEntry(ParticleNode.Type), Request};
		detail::SourceParticle3DControls Controls;
		detail::SourceParticle3DState State;
		std::vector<SourceParticle3DVertex> Vertices;
		uint64_t ExpectedHash = 0;
		explicit SourcePathParticleFixture(Kind kind) : Workload(kind) {
			ParticleContext.ByteBudget = Limits::MaximumEvaluationBytes;
			if (IsSpiral()) {
				BuildSpiral();
			} else if (IsPath()) {
				Path2D path;
				path.Anchors = {{{0, 0, 0, 0, 0, 0}, 0}, {{PathLength, 0, 0, 0, 0, 0}, 0}};
				Authored.FormatVersion = 9;
				Authored.Nodes = {
					{"bake",
					 "pc.path_bake",
					 "",
					 {},
					 {{"path", std::move(path)},
					  {"sample_type", EnumValue{kind == Kind::PathLength ? 0 : 1}},
					  {"segment_length", 1.0},
					  {"output_amount", int64_t{64}},
					  {"spread_single_path", true}}}
				};
				Authored.Outputs = {{"out", "bake", "segments"}};
				Diagnostic diagnostic;
				Check(Compile(Authored, Compiled, diagnostic), diagnostic);
			} else {
				Controls.Seed = uint32_t(Seed);
				Controls.PoolCapacity = PoolCapacity;
				Controls.TotalFrames = Frames;
				Controls.Loop = false;
				Controls.SpawnDelay = 1;
				Controls.SpawnAmount = {SpawnPerFrame, SpawnPerFrame};
				Controls.SpawnSpan = {};
				Controls.Lifespan = {64, 64};
				Controls.Velocity = {1, 1, .5, .5, 0, 0};
				Controls.Acceleration = {.25, .25, 0, 0, 0, 0};
				Controls.Billboard = true;
				if (Workload == Kind::ParticleVertices) RunState();
				Vertices.reserve((Frames * SpawnPerFrame - 1) * QUAD.size());
			}
			Run();
			ExpectedHash = Verify();
			Run();
			if (Verify() != ExpectedHash) Fail("source path/particle repeatability");
			CheckBudget();
		}
		unsigned ProfileKind() const {
			return 100 + unsigned(Workload);
		}
		uint32_t OutputSide() const {
			return 0;
		}
		const char *OutputFormat() const {
			return IsSpiral()						 ? "SpiralPathSamples64"
				   : IsPath()						 ? "PathSamples"
				   : Workload == Kind::ParticleState ? "ParticleSlots512Frames16"
													 : "PreparedVertices1530";
		}
		bool IsPalette() const {
			return false;
		}
		size_t ProfileEvaluations() const {
			return IsSpiral() ? 3 : IsPath() ? 1 : 0;
		}
		bool ProfileProcessors() const {
			return IsPath() || IsSpiral();
		}
		std::string_view ProfileWorkScope() const {
			return IsSpiral()						 ? "imagegraph.source.path_spiral"
				   : IsPath()						 ? "imagegraph.source.path_bake"
				   : Workload == Kind::ParticleState ? "imagegraph.particle3d.advance"
													 : "imagegraph.particle3d.vertices.workload";
		}
		double ProfileSeed() const {
			return IsPath() || IsSpiral() ? 0 : Seed;
		}
		unsigned ProfileIterations() const {
			return Workload == Kind::ParticleState ? Frames : 1;
		}
		bool IsSpiral() const {
			return Workload == Kind::SpiralSamples;
		}
		static double SpiralRatio(size_t index) {
			return .005 + double(index) * .98 / 63;
		}
		void BuildSpiral() {
			Path2D line;
			line.Anchors = {{{0, 0, 0, 0, 0, 0}, 0}, {{PathLength, 0, 0, 0, 0, 0}, 0}};
			line.Weights = {{0, 2}, {100, 2}};
			Curve amplitude;
			amplitude.Header = {0, 1, 0, 0, 1, 0};
			amplitude.Anchors = {{0, 0, 0, 1, 1. / 3, 0}, {-1. / 3, 0, 1, 1, 0, 0}};
			ArrayValue ratios{ValueType::Scalar, {}};
			for (size_t i = 0; i < 64; ++i)
				ratios.Elements.emplace_back(SpiralRatio(i));
			Authored.FormatVersion = 9;
			Authored.Nodes = {
				{"spiral",
				 "pc.path_spiral",
				 "",
				 {},
				 {{"path", std::move(line)},
				  {"frequency", 4.},
				  {"amplitude", 4.},
				  {"spiral", .5},
				  {"phase", 30.},
				  {"direction", EnumValue{0}},
				  {"amplitude_curve", std::move(amplitude)},
				  {"use_weight", true},
				  {"weight_mode", EnumValue{0}},
				  {"range_2", Vector2{0, 1}}}},
				{"sample", "pc.path_sample", "", {}, {{"type", EnumValue{2}}}}
			};
			Authored.Junctions = {{"ratios", "", ValueType::Array, std::move(ratios)}};
			Authored.Links = {{"spiral", "path", "sample", "path"}, {"ratios", "value", "sample", "ratio"}};
			Authored.Outputs = {
				{"path", "spiral", "path"}, {"out", "sample", "position"}, {"weights", "sample", "weight"}
			};
			Diagnostic diagnostic;
			Check(Compile(Authored, Compiled, diagnostic), diagnostic);
		}
		bool IsPath() const {
			return Workload == Kind::PathLength || Workload == Kind::PathAmount;
		}
		[[noreturn]] static void Fail(std::string_view message) {
			throw std::runtime_error(std::string(message));
		}
		static void Check(Status status, const Diagnostic &diagnostic) {
			if (status != Status::Ok)
				Fail(diagnostic.NodeId + ":" + diagnostic.Port + ":" + diagnostic.Message);
		}
		void CheckParticle(bool success) const {
			if (!success || ParticleContext.FailureCode != Status::Ok)
				Fail(ParticleContext.FailurePort + ":" + ParticleContext.FailureMessage);
		}
		void RunState() {
			CheckParticle(detail::BeginSourceParticle3DState(ParticleContext, Controls, 0, State));
			for (uint32_t frame = 1; frame < Frames; ++frame)
				CheckParticle(
					detail::AdvanceSourceParticle3DState(ParticleContext, Controls, State, frame, State)
				);
		}
		void Run() {
			if (IsSpiral()) {
				Diagnostic diagnostic;
				Check(EvaluateValue(Authored, Compiled, "path", Request, SpiralPath, diagnostic), diagnostic);
				Check(EvaluateValue(Authored, Compiled, "out", Request, Segments, diagnostic), diagnostic);
				Check(
					EvaluateValue(Authored, Compiled, "weights", Request, SpiralWeights, diagnostic),
					diagnostic
				);
				return;
			}
			if (IsPath()) {
				Diagnostic diagnostic;
				Check(EvaluateValue(Authored, Compiled, "out", Request, Segments, diagnostic), diagnostic);
				return;
			}
			if (Workload == Kind::ParticleState) {
				RunState();
				return;
			}
			ENGINE_PROFILE("imagegraph.particle3d.vertices.workload");
			const auto &slots = State.Buffers[State.BufferIndex];
			const size_t drawCount = detail::SourceParticle3DDrawCount(State);
			Vertices.resize(drawCount * QUAD.size());
			for (size_t slot = 0; slot < drawCount; ++slot)
				for (size_t vertex = 0; vertex < QUAD.size(); ++vertex)
					if (!PrepareSourceParticle3DVertex(
							QUAD[vertex],
							slots[slot].Transform,
							slots[slot].Particle,
							OBJECT,
							{0, 1, 0},
							Vertices[slot * QUAD.size() + vertex]
						))
						Fail("particle source vertex preparation");
			core::Metrics::Count("imagegraph.workload.particle3d.prepared_vertices", Vertices.size());
			core::Metrics::Count(
				"imagegraph.workload.particle3d.vertex_written_bytes",
				Vertices.size() * sizeof(SourceParticle3DVertex)
			);
		}
		static void Near(double actual, double expected) {
			if (!std::isfinite(actual) || std::abs(actual - expected) > 1e-5)
				Fail("source path/particle oracle");
		}
		uint64_t Verify() const {
			uint64_t hash = 14695981039346656037ull;
			const auto number = [&](double value) {
				uint64_t bits = std::bit_cast<uint64_t>(value);
				for (unsigned shift = 0; shift < 64; shift += 8)
					hash = (hash ^ uint8_t(bits >> shift)) * 1099511628211ull;
			};
			if (IsSpiral()) {
				const auto *path = std::get_if<Path2D>(&SpiralPath.Data);
				const auto *positions = std::get_if<ArrayValue>(&Segments.Data);
				const auto *weights = std::get_if<ArrayValue>(&SpiralWeights.Data);
				if (!path || !path->SourceOperation || path->SourceOperation->Inputs.size() != 1 ||
					!positions || !weights || positions->ElementType != ValueType::Vector2 ||
					weights->ElementType != ValueType::Scalar || positions->Elements.size() != 64 ||
					weights->Elements.size() != 64 || !positions->Nested.empty() ||
					!positions->Items.empty() || !weights->Nested.empty() || !weights->Items.empty())
					Fail("spiral complete path and downstream batch shape");
				const auto &operation = *path->SourceOperation;
				if (operation.Kind != SourcePathOperationKind::Spiral || !operation.Spiral)
					Fail("spiral public operation kind");
				const auto &controls = *operation.Spiral;
				if (controls.Frequency != 4 || controls.Amplitude != 4 || controls.Spiral != .5 ||
					controls.Phase != 30 || controls.Direction != 0 || !controls.UseWeight ||
					controls.WeightMode != 0 || controls.WeightRange != Vector2{0, 1} ||
					controls.AmplitudeCurve.size() != 129 || !controls.DirectionCurve.empty() ||
					!controls.Cache.empty())
					Fail("spiral public controls and maps");
				for (double amplitude : controls.AmplitudeCurve)
					Near(amplitude, 1);
				if (path->SourceOperation->Inputs[0] != std::get<Path2D>(Authored.Nodes[0].Values[0].Data))
					Fail("spiral owned child path");
				// grug weighted horizontal line fixes normal direction. pinned phase gives independent
				// ellipse offsets.
				for (size_t i = 0; i < 64; ++i) {
					const double ratio = SpiralRatio(i),
								 phase = (30. / 360 + ratio * 4) * 2 * std::numbers::pi;
					const auto *position = std::get_if<Vector2>(&positions->Elements[i]);
					const auto *weight = std::get_if<double>(&weights->Elements[i]);
					if (!position || !weight) Fail("spiral downstream typed sample");
					const auto lengthdir = [](double value) {
						const double nearest = std::round(value);
						return std::abs(value - nearest) < .0001 ? nearest : value;
					};
					Near(position->X, PathLength * ratio + lengthdir(2 * std::cos(phase)));
					Near(position->Y, lengthdir(-4 * std::sin(phase)));
					Near(*weight, .5 + .5 * std::cos(phase));
					number(position->X);
					number(position->Y);
					number(*weight);
				}
				Document retained;
				retained.FormatVersion = 9;
				retained.Nodes = {{"sample", "pc.path_sample", "", {}, {{"path", *path}}}};
				for (unsigned char value : Write(retained))
					hash = (hash ^ value) * 1099511628211ull;
				return hash;
			}
			if (IsPath()) {
				const auto *segments = std::get_if<ArrayValue>(&Segments.Data);
				const size_t count = Workload == Kind::PathLength ? 129 : 65;
				if (!segments || segments->Items.size() != count || !segments->Elements.empty() ||
					!segments->Nested.empty())
					Fail("baked path sample shape");
				for (size_t index = 0; index < count; ++index) {
					const auto *coordinates =
						std::get_if<std::vector<SourceArrayItem>>(&segments->Items[index].Data);
					if (!coordinates || coordinates->size() != 3) Fail("baked path coordinate shape");
					const double progress = double(index) / double(count - 1);
					const std::array expected{
						PathLength * (Workload == Kind::PathAmount ? std::min(progress, .999) : progress),
						0.0,
						progress
					};
					for (size_t component = 0; component < 3; ++component) {
						const auto *element = std::get_if<ElementValue>(&(*coordinates)[component].Data);
						const auto *value = element ? std::get_if<double>(element) : nullptr;
						if (!value) Fail("baked path numeric coordinate");
						Near(*value, expected[component]);
						number(*value);
					}
				}
				return hash;
			}
			const auto &slots = State.Buffers[State.BufferIndex];
			if (slots.size() != PoolCapacity || State.Frame != Frames - 1 ||
				State.SpawnIndex != Frames * SpawnPerFrame ||
				detail::SourceParticle3DDrawCount(State) != Frames * SpawnPerFrame - 1)
				Fail("particle source state shape");
			if (Workload == Kind::ParticleVertices &&
				Vertices.size() != (Frames * SpawnPerFrame - 1) * QUAD.size())
				Fail("particle prepared vertex count");
			for (size_t index = 0; index < slots.size(); ++index) {
				const auto &slot = slots[index];
				if (index >= Frames * SpawnPerFrame) {
					Near(slot.Particle.Active, 0);
					continue;
				}
				const double age = Frames - index / SpawnPerFrame;
				const double px = age + .25 * (age - 1) * age * (age + 1) / 6;
				Near(slot.Particle.Active, 1);
				Near(slot.Particle.LifeTime, age);
				Near(slot.Transform.Fields[0], px);
				Near(slot.Transform.Fields[1], age * .5);
				Near(slot.Transform.Fields[2], 0);
				for (double value : slot.Transform.Fields)
					number(value);
				for (double value : slot.Particle.Velocity)
					number(value);
				if (Workload != Kind::ParticleVertices || index >= Frames * SpawnPerFrame - 1) continue;
				for (size_t vertex = 0; vertex < QUAD.size(); ++vertex) {
					const auto &prepared = Vertices[index * QUAD.size() + vertex];
					if (!prepared.Active) Fail("particle prepared active vertex");
					Near(prepared.Position.X, px - QUAD[vertex].Position.X);
					Near(prepared.Position.Y, age * .5);
					Near(prepared.Position.Z, QUAD[vertex].Position.Y);
					Near(prepared.Normal.X, 0);
					Near(prepared.Normal.Y, 1);
					Near(prepared.Normal.Z, 0);
					Near(prepared.UV.X, QUAD[vertex].UV.X);
					Near(prepared.UV.Y, QUAD[vertex].UV.Y);
					for (double channel : prepared.Colour)
						Near(channel, 1);
					number(prepared.Position.X);
					number(prepared.Position.Y);
					number(prepared.Position.Z);
				}
			}
			return hash;
		}
		void CheckBudget() {
			const uint64_t before = Verify();
			if (IsSpiral()) {
				Diagnostic diagnostic;
				auto rejected = Authored;
				ArrayValue frequencies{ValueType::Scalar, {}};
				frequencies.Elements.assign(256, ElementValue{4.});
				rejected.Junctions.push_back({"frequencies", "", ValueType::Array, std::move(frequencies)});
				rejected.Links.push_back({"frequencies", "value", "spiral", "frequency"});
				Plan rejectedPlan;
				Check(Compile(rejected, rejectedPlan, diagnostic), diagnostic);
				if (EvaluateValue(rejected, rejectedPlan, "path", Request, SpiralPath, diagnostic) !=
						Status::LimitExceeded ||
					Verify() != before)
					Fail("spiral workload full batch refusal changed output");
				EvaluationSnapshot snapshot;
				Check(
					EvaluateNodeInputs(Authored, Compiled, "sample", Request, snapshot, diagnostic),
					diagnostic
				);
				const auto retained = snapshot.RetainedBytes();
				const std::vector<EvaluationInputValue> prior(
					snapshot.Values().begin(), snapshot.Values().end()
				);
				const auto format = snapshot.InheritedSurfaceFormat();
				const auto interpolation = snapshot.InheritedInterpolation();
				if (!retained || prior.empty() || !snapshot.Images().empty() ||
					!snapshot.ImageArrays().empty())
					Fail("spiral workload prior snapshot shape");
				if (EvaluateNodeInputs(Authored, Compiled, "sample", Request, snapshot, diagnostic, 1) !=
						Status::LimitExceeded ||
					snapshot.RetainedBytes() != retained || snapshot.Values().size() != prior.size() ||
					!snapshot.Images().empty() || !snapshot.ImageArrays().empty() ||
					snapshot.InheritedSurfaceFormat() != format ||
					snapshot.InheritedInterpolation() != interpolation || Verify() != before)
					Fail("spiral workload byte refusal changed snapshot or output");
				for (size_t i = 0; i < prior.size(); ++i) {
					const auto &value = snapshot.Values()[i];
					if (value.Port != prior[i].Port || value.Data != prior[i].Data ||
						value.Linked != prior[i].Linked || value.Domain != prior[i].Domain)
						Fail("spiral workload byte refusal changed resolved input");
				}
			} else if (IsPath()) {
				auto rejected = Authored;
				rejected.Nodes[0].Values[1].Data = EnumValue{1};
				rejected.Nodes[0].Values[3].Data = int64_t{1000000};
				Plan rejectedPlan;
				Diagnostic diagnostic;
				Check(Compile(rejected, rejectedPlan, diagnostic), diagnostic);
				if (EvaluateValue(rejected, rejectedPlan, "out", Request, Segments, diagnostic) !=
					Status::LimitExceeded)
					Fail("baked path workload limit refusal");
			} else {
				detail::EvaluationBudget budget(1);
				detail::NodeContext limited{
					ParticleNode, *FindCatalogueEntry(ParticleNode.Type), Request, budget
				};
				limited.ByteBudget = 1;
				if (detail::AdvanceSourceParticle3DState(limited, Controls, State, Frames, State) ||
					limited.FailureCode != Status::LimitExceeded || budget.Used() != 0)
					Fail("particle workload byte refusal");
			}
			if (Verify() != before) Fail("failed path/particle workload changed output");
		}
	};
}
