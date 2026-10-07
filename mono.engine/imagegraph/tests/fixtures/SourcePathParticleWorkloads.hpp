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
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace engine::imagegraph::testing {
	// grug fixed source inputs give independent line, motion and vertex oracles.
	// Particle cases measure CPU state and vertex helpers, before any raster draw.
	struct SourcePathParticleFixture {
		enum class Kind { PathLength, PathAmount, ParticleState, ParticleVertices };
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
		EvaluatedValue Segments;
		Node ParticleNode{"particles", "pc.3_d_particle", "", {}, {}};
		EvaluationRequest Request;
		detail::NodeContext ParticleContext{ParticleNode, *FindCatalogueEntry(ParticleNode.Type), Request};
		detail::SourceParticle3DControls Controls;
		detail::SourceParticle3DState State;
		std::vector<SourceParticle3DVertex> Vertices;
		uint64_t ExpectedHash = 0;
		explicit SourcePathParticleFixture(Kind kind) : Workload(kind) {
			ParticleContext.ByteBudget = Limits::MaximumEvaluationBytes;
			if (IsPath()) {
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
			return IsPath()							 ? "PathSamples"
				   : Workload == Kind::ParticleState ? "ParticleSlots512Frames16"
													 : "PreparedVertices1530";
		}
		bool IsPalette() const {
			return false;
		}
		size_t ProfileEvaluations() const {
			return IsPath() ? 1 : 0;
		}
		bool ProfileProcessors() const {
			return IsPath();
		}
		std::string_view ProfileWorkScope() const {
			return IsPath()							 ? "imagegraph.source.path_bake"
				   : Workload == Kind::ParticleState ? "imagegraph.particle3d.advance"
													 : "imagegraph.particle3d.vertices.workload";
		}
		double ProfileSeed() const {
			return IsPath() ? 0 : Seed;
		}
		unsigned ProfileIterations() const {
			return Workload == Kind::ParticleState ? Frames : 1;
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
			if (IsPath()) {
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
