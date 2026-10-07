#include "Families.hpp"
#include "SourceMeshFlatten.hpp"
#include "SourceParticle3DRecipe.hpp"
#include "SourceParticle3DReplay.hpp"

#include <engine/imagegraph/DataReplay.hpp>

#include <new>
#include <stdexcept>

namespace engine::imagegraph::detail {
	namespace source_particle3d_executor {
		bool Draw(NodeContext &c) try {
			ENGINE_PROFILE("imagegraph.particle3d.executor");
			if (c.Request.Subframe != 0 || c.Request.Tick > Limits::MaximumTick)
				return c.Fail(
					Status::UnsupportedExecution, "Particle 3D requires bounded integer timeline frames"
				);
			const int64_t frame =
				c.Request.NegativeFrame ? -int64_t(c.Request.Tick) : int64_t(c.Request.Tick);
			SourceParticle3DPrepared prepared;
			if (!PrepareSourceParticle3DControls(c, prepared)) return false;
			const DataReplayState *owner = c.CurrentData ? c.CurrentData : c.Request.DataReplay;
			const DataReplayEntry *previous = nullptr;
			if (owner) {
				if (owner->Entries.size() > Limits::MaximumArrayElements)
					return c.Fail(Status::LimitExceeded, "Particle 3D replay rows exceed bounds");
				for (const auto &entry : owner->Entries)
					if (entry.NodeId == c.Authored.Id && entry.ProcessorRow == c.ProcessorRow) {
						if (previous)
							return c.Fail(Status::InvalidValue, "Particle 3D replay row is duplicated");
						previous = &entry;
					}
			}
			SourceParticle3DReceipt receipt;
			SourceParticle3DState state;
			if (previous) {
				if (!previous->Initialized || previous->Values.size() != 1 ||
					previous->Values.front().Frame != previous->Tick || previous->Subframe != 0)
					return c.Fail(Status::InvalidValue, "Particle 3D replay row is malformed");
				const auto *encoded = std::get_if<StructValue>(&previous->Values.front().Data);
				if (!encoded)
					return c.Fail(Status::InvalidValue, "Particle 3D receipt requires constructor schema");
				if (!DecodeSourceParticle3DReceipt(c, *encoded, receipt)) return false;
				if (previous->Tick > Limits::MaximumTick ||
					receipt.State.Frame !=
						(previous->NegativeFrame ? -int64_t(previous->Tick) : int64_t(previous->Tick)))
					return c.Fail(Status::InvalidValue, "Particle 3D replay frame identity differs");
				if (!AdvanceSourceParticle3DState(c, prepared.Controls, receipt.State, frame, state))
					return false;
			} else {
				const Value *source = c.Find("mesh");
				const auto *mesh = source ? std::get_if<MeshValue3D>(source) : nullptr;
				const auto *scene = source ? std::get_if<SceneValue3D>(source) : nullptr;
				if (source && !mesh && !scene)
					return c.Fail(Status::TypeMismatch, "Particle 3D base requires mesh or scene", "mesh");
				if ((!mesh || !mesh->Data) && (!scene || !scene->Data)) {
					c.SetValue("mesh", MeshValue3D{});
					return c.FailureCode == Status::Ok;
				}
				uint64_t flattenWork = 0;
				if (!std::visit(
						[&](const auto &item) { return SourceMeshFlattenWork(item, flattenWork); }, *source
					) ||
					flattenWork > 64000000 / std::max<size_t>(c.ProcessorCount, 1))
					return c.Fail(
						Status::LimitExceeded, "Particle 3D base exceeds whole processor work bounds", "mesh"
					);
				if (!ValidRuntimeValue(*source))
					return c.Fail(Status::InvalidValue, "Particle 3D base mesh is invalid", "mesh");
				SourceMeshFlatCost cost;
				std::array<const MeshTransform3D *, 128> chain{};
				if (!std::visit(
						[&](const auto &item) { return SourceMeshFlatten(item, chain, 0, cost, nullptr); },
						*source
					))
					return c.Fail(
						Status::LimitExceeded, "Particle 3D geometry flatten exceeds bounds", "mesh"
					);
				if (cost.Vertices * 512 > 64000000 / std::max<size_t>(c.ProcessorCount, 1))
					return c.Fail(
						Status::LimitExceeded, "Particle 3D flatten exceeds whole processor work bounds"
					);
				auto charge = c.ReserveWorkspace(cost.Bytes, "mesh");
				if (!charge) return false;
				receipt.BaseCharge = std::move(*charge);
				auto &base = receipt.BaseMesh.Data.emplace();
				base.Instanced = base.ParticleInstanced = true;
				base.CpuVerticesPresent = base.CpuEdgesPresent = false;
				base.ParticleTransparent = c.Boolean("transparent", false);
				base.InstanceObjectTransform =
					mesh ? mesh->Data->LocalTransforms.front() : scene->Data->Transform;
				base.LocalTransforms.emplace_back();
				base.Parts.reserve(cost.Parts);
				base.Materials.reserve(cost.Parts);
				SourceMeshFlatCost emitting;
				if (!std::visit(
						[&](const auto &item) { return SourceMeshFlatten(item, chain, 0, emitting, &base); },
						*source
					))
					return c.Fail(Status::InvalidValue, "Particle 3D flattened source is invalid", "mesh");
				if (!BeginSourceParticle3DState(c, prepared.Controls, frame, state)) return false;
			}
			const double blend = c.SourceChoice("blend_mode");
			if (!std::isfinite(blend) || blend < 0 || blend > 3 || std::floor(blend) != blend)
				return c.Fail(Status::InvalidValue, "Particle 3D blend mode is invalid", "blend_mode");
			receipt.BaseMesh.Data->ParticleBlend = ParticleBlend3D(int(blend));
			StructValue encoded;
			AllocationReservation encodedCharge;
			if (!EncodeSourceParticle3DReceipt(c, state, receipt.BaseMesh, encoded, encodedCharge))
				return false;
			const size_t count = SourceParticle3DDrawCount(state);
			const uint64_t baseBytes = MeshStorageBytes<false>(receipt.BaseMesh);
			const uint64_t historyBytes = sizeof(DataReplayEntry) + sizeof(DataReplayValueFrame) +
										  std::max(c.Authored.Id.size(), std::string{}.capacity());
			if (!c.ReserveOutput(
					baseBytes + count * (sizeof(MeshInstance3D) + sizeof(ParticleRecord3D)) + historyBytes +
						64,
					"mesh"
				))
				return false;
			MeshValue3D result = receipt.BaseMesh;
			result.Data->Instances.reserve(count);
			result.Data->ParticleRecords.reserve(count);
			const auto &slots = state.Buffers[state.BufferIndex];
			for (size_t i = 0; i < count; ++i) {
				result.Data->Instances.push_back(slots[i].Transform);
				result.Data->ParticleRecords.push_back(slots[i].Particle);
			}
			if (!ValidMeshPayload(result))
				return c.Fail(Status::InvalidValue, "Particle 3D publication is malformed", "mesh");
			DataReplayEntry update;
			update.NodeId = c.Authored.Id;
			update.ProcessorRow = c.ProcessorRow;
			update.Tick = c.Request.Tick;
			update.NegativeFrame = c.Request.NegativeFrame;
			update.Initialized = true;
			update.Values.reserve(1);
			update.Values.push_back({c.Request.Tick, Value{std::move(encoded)}});
			if (!c.ReserveOutput(RetainedDataReplayEntryBytes(update) - historyBytes, "mesh")) return false;
			// The history admission includes one DataReplayEntry before its vector allocation.
			c.DataUpdates.reserve(1);
			if (c.DataUpdates.capacity() > 1 &&
				!c.ReserveOutput((c.DataUpdates.capacity() - 1) * sizeof(DataReplayEntry), "mesh"))
				return false;
			c.SetValue("mesh", std::move(result));
			if (c.FailureCode != Status::Ok) return false;
			c.DataUpdates.push_back(std::move(update));
			return true;
		} catch (const std::bad_alloc &) {
			c.ClearOutputs();
			return c.Fail(Status::LimitExceeded, "Particle 3D executor allocation failed");
		} catch (const std::length_error &) {
			c.ClearOutputs();
			return c.Fail(Status::LimitExceeded, "Particle 3D executor container bounds exceeded");
		}
	}
	std::span<const ExecutorEntry> SourceParticle3DExecutors() {
		static constexpr ExecutorEntry entries[]{{"pc.3_d_particle", source_particle3d_executor::Draw, true}};
		return entries;
	}
}
