#pragma once

// Host data for one GPU-authored mesh LOD choice.
//
// Each level is packed against its own mesh bounds before upload. Its immutable
// mesh clusters become indirect commands, then the compute pass chooses one
// resident level and enables only that level's cluster commands. The result
// remains on the device, so meshes with different authored centres and extents
// still fill the same scene Bounds without a CPU readback.

#include "InstancePacking.hpp"

#include <engine/render/MeshTable.hpp>
#include <engine/scene/DrawInstance.hpp>

#include <SDL3/SDL_gpu.h>
#include <glm/mat3x3.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace engine::render {

	inline constexpr uint32_t NO_LOD_DRAW = std::numeric_limits<uint32_t>::max();

	// std430 input consumed by lod-select.comp.
	struct alignas(16) GpuLodSelection {
		glm::vec4 CentreTarget;
		glm::vec4 ExtentLevels;
		glm::uvec4 Triangles;
		glm::uvec4 FirstArguments;
		glm::uvec4 ArgumentCounts;
	};

	static_assert(sizeof(GpuLodSelection) == 80);

	struct LodDrawLevel {
		const MeshEntry *Mesh = nullptr;
		uint32_t FirstArgument = 0;
		uint32_t ArgumentCount = 0;
		uint32_t Row = 0;
	};

	struct LodDrawRange {
		MeshRange Range;
		uint32_t Material = std::numeric_limits<uint32_t>::max();
	};

	struct LodDraw {
		uint32_t Slot = 0;
		uint8_t LevelCount = 0;
		std::array<LodDrawLevel, scene::LOD_LEVELS> Levels{};
		std::array<std::vector<LodDrawRange>, scene::LOD_LEVELS> Clusters;
	};

	struct LodPlan {
		std::vector<GpuLodSelection> Selections;
		std::vector<GpuInstance> Instances;
		std::vector<uint32_t> Indices;
		std::vector<uint32_t> SkinOffsets;
		std::vector<SDL_GPUIndexedIndirectDrawCommand> Commands;
		std::vector<LodDraw> Draws;

		void Clear() {
			Selections.clear();
			Instances.clear();
			Indices.clear();
			SkinOffsets.clear();
			Commands.clear();
			Draws.clear();
		}
	};

	struct LodTransferLayout {
		uint32_t Selections = 0;
		uint32_t Instances = 0;
		uint32_t Indices = 0;
		uint32_t SkinOffsets = 0;
		uint32_t Arguments = 0;
		uint32_t Bytes = 0;
	};

	inline LodTransferLayout TransferLayoutOf(const LodPlan &plan) {
		LodTransferLayout layout;
		layout.Instances = static_cast<uint32_t>(plan.Selections.size() * sizeof(GpuLodSelection));
		layout.Indices =
			layout.Instances + static_cast<uint32_t>(plan.Instances.size() * sizeof(GpuInstance));
		layout.SkinOffsets = layout.Indices + static_cast<uint32_t>(plan.Indices.size() * sizeof(uint32_t));
		layout.Arguments =
			layout.SkinOffsets + static_cast<uint32_t>(plan.SkinOffsets.size() * sizeof(uint32_t));
		layout.Bytes =
			layout.Arguments +
			static_cast<uint32_t>(plan.Commands.size() * sizeof(SDL_GPUIndexedIndirectDrawCommand));
		return layout;
	}

	// Adds one resolved automatic/custom ladder. The caller resolves only resident meshes and
	// stops at the first missing level, so the GPU never chooses fallback cube
	// geometry for content that is still arriving.
	inline bool AppendAuthoredLod(
		LodPlan &plan,
		uint32_t slot,
		const scene::DrawInstance &instance,
		std::span<const MeshEntry *const> meshes
	) {
		const uint32_t levelCount =
			static_cast<uint32_t>(std::min(meshes.size(), static_cast<size_t>(scene::LOD_LEVELS)));
		if (instance.LodStrategyMode == scene::LodStrategy::None || levelCount < 2) {
			return false;
		}

		LodDraw draw;
		draw.Slot = slot;
		draw.LevelCount = static_cast<uint8_t>(levelCount);

		GpuLodSelection selection{};
		selection.CentreTarget = glm::vec4{
			instance.Frame.Position.X,
			instance.Frame.Position.Y,
			instance.Frame.Position.Z,
			instance.LodTargetQuadArea > 0.0f ? instance.LodTargetQuadArea : scene::DEFAULT_TARGET_QUAD_AREA,
		};

		const glm::mat3 rotation{instance.Frame.ToMatrix()};
		const glm::vec3 half{instance.HalfExtent.X, instance.HalfExtent.Y, instance.HalfExtent.Z};
		const glm::vec3 extent =
			glm::abs(rotation[0]) * half.x + glm::abs(rotation[1]) * half.y + glm::abs(rotation[2]) * half.z;
		selection.ExtentLevels = glm::vec4{extent, static_cast<float>(levelCount)};

		for (uint32_t level = 0; level < levelCount; ++level) {
			const MeshEntry &mesh = *meshes[level];
			const uint32_t row = static_cast<uint32_t>(plan.Instances.size());
			const uint32_t firstArgument = static_cast<uint32_t>(plan.Commands.size());

			plan.Instances.push_back(ToGpu(instance, mesh));
			plan.Indices.push_back(row);
			plan.SkinOffsets.push_back(
				instance.SkinCount != 0 && mesh.JointCount == instance.SkinCount
					? instance.SkinFirst
					: std::numeric_limits<uint32_t>::max()
			);

			const auto addCommand = [&](const MeshRange &range, uint32_t material) {
				if (range.IndexCount == 0) {
					return;
				}
				plan.Commands.push_back(
					SDL_GPUIndexedIndirectDrawCommand{
						range.IndexCount,
						0,
						range.FirstIndex,
						range.VertexOffset,
						row,
					}
				);
				draw.Clusters[level].push_back({range, material});
			};
			if (mesh.Clusters.empty()) {
				if (mesh.Runs.empty()) {
					addCommand(mesh.Whole, std::numeric_limits<uint32_t>::max());
				} else {
					for (uint32_t material = 0; material < mesh.Runs.size(); material++) {
						addCommand(mesh.Runs[material], material);
					}
				}
			} else {
				for (const MeshCluster &cluster : mesh.Clusters) {
					addCommand(cluster.Range, cluster.Material);
				}
			}

			const uint32_t argumentCount = static_cast<uint32_t>(plan.Commands.size()) - firstArgument;
			draw.Levels[level] = {&mesh, firstArgument, argumentCount, row};
			selection.Triangles[level] = std::max(mesh.Whole.IndexCount / 3u, 1u);
			selection.FirstArguments[level] = firstArgument;
			selection.ArgumentCounts[level] = argumentCount;
		}

		plan.Selections.push_back(selection);
		plan.Draws.push_back(draw);
		return true;
	}
}
