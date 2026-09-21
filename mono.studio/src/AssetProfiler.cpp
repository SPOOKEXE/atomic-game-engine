#include "AssetProfiler.hpp"

#include "AssetProfilerSort.hpp"

#include <engine/assets/AssetKind.hpp>
#include <engine/render/Renderer.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <imgui.h>
#include <string>
#include <studio/Editor.hpp>
#include <vector>

namespace {
	std::string Readable(uint64_t bytes) {
		static constexpr const char *UNITS[] = {"B", "KB", "MB", "GB", "TB"};
		double scaled = static_cast<double>(bytes);
		size_t unit = 0;
		while (scaled >= 1024.0 && unit + 1 < std::size(UNITS)) {
			scaled /= 1024.0;
			unit++;
		}
		char text[32];
		std::snprintf(text, sizeof(text), unit == 0 ? "%.0f %s" : "%.1f %s", scaled, UNITS[unit]);
		return text;
	}
}

namespace studio {
	AssetFootprint MeshFootprint(const engine::assets::MeshData &mesh) {
		const uint64_t bytes =
			static_cast<uint64_t>(mesh.Vertices.size()) * sizeof(engine::assets::MeshVertex) +
			static_cast<uint64_t>(mesh.Indices.size()) * sizeof(uint32_t);
		return {.DecodedBytes = bytes, .CpuResidentBytes = bytes, .GpuResidentBytes = bytes};
	}

	AssetFootprint TextureFootprint(const engine::assets::TextureData &texture) {
		uint64_t decoded = texture.Pixels.size();
		for (const std::vector<std::byte> &level : texture.Mips) {
			decoded += level.size();
		}

		uint64_t gpu = 0;
		for (uint32_t level = 0; level < texture.LevelCount(); level++) {
			gpu += static_cast<uint64_t>(engine::assets::MipExtent(texture.Width, level)) *
				   engine::assets::MipExtent(texture.Height, level) * 4;
		}
		return {.DecodedBytes = decoded, .CpuResidentBytes = 0, .GpuResidentBytes = gpu};
	}

	void SortAssetProfiles(std::vector<AssetProfileSortRow> &rows, std::span<const AssetProfileSort> sorts) {
		// The content map does not promise an iteration order. Establishing the
		// name order first keeps rows with equal measured values still between
		// frames, then each stable pass applies one requested column.
		std::stable_sort(
			rows.begin(), rows.end(), [](const AssetProfileSortRow &left, const AssetProfileSortRow &right) {
				return left.Name < right.Name;
			}
		);

		for (auto sort = sorts.rbegin(); sort != sorts.rend(); ++sort) {
			std::stable_sort(
				rows.begin(),
				rows.end(),
				[sort](const AssetProfileSortRow &left, const AssetProfileSortRow &right) {
					auto less = [sort](const AssetProfileSortRow &first, const AssetProfileSortRow &second) {
						switch (sort->Column) {
						case AssetProfileColumn::Asset:
							return first.Name < second.Name;
						case AssetProfileColumn::Kind:
							return first.Kind < second.Kind;
						case AssetProfileColumn::Pulled:
							return first.PulledBytes < second.PulledBytes;
						case AssetProfileColumn::Decoded:
							return first.DecodedBytes < second.DecodedBytes;
						case AssetProfileColumn::Cpu:
							return first.CpuResidentBytes < second.CpuResidentBytes;
						case AssetProfileColumn::Gpu:
							return first.GpuResidentBytes < second.GpuResidentBytes;
						case AssetProfileColumn::Updates:
							return first.Updates < second.Updates;
						case AssetProfileColumn::Resident:
							return first.ResidentInstances < second.ResidentInstances;
						case AssetProfileColumn::Delta:
							return first.StagedBytes != second.StagedBytes
									   ? first.StagedBytes < second.StagedBytes
									   : first.StagedInstances < second.StagedInstances;
						case AssetProfileColumn::TotalResident:
							return first.CpuResidentBytes + first.GpuResidentBytes <
								   second.CpuResidentBytes + second.GpuResidentBytes;
						}
						return false;
					};
					return sort->Ascending ? less(left, right) : less(right, left);
				}
			);
		}
	}

	void Editor::RecordContentAssetPull(
		const engine::core::Name &name, engine::assets::AssetKind kind, uint64_t bytes
	) {
		if (!name.IsValid()) {
			return;
		}
		ContentAssetProfile &profile = ContentAssetProfiles[name.Id()];
		profile.Name = name;
		profile.Kind = kind;
		profile.PulledBytes += bytes;
	}

	void Editor::RecordContentAssetFootprint(
		const engine::core::Name &name,
		uint64_t decodedBytes,
		uint64_t cpuResidentBytes,
		uint64_t gpuResidentBytes
	) {
		const auto found = ContentAssetProfiles.find(name.Id());
		if (found == ContentAssetProfiles.end()) {
			return;
		}
		ContentAssetProfile &profile = found->second;
		profile.DecodedBytes = decodedBytes;
		profile.CpuResidentBytes = cpuResidentBytes;
		profile.GpuResidentBytes = gpuResidentBytes;
		profile.Updates++;
	}

	void Editor::RecordContentAssetFailure(const engine::core::Name &name) {
		const auto found = ContentAssetProfiles.find(name.Id());
		if (found != ContentAssetProfiles.end()) {
			found->second.Failures++;
		}
	}

	void Editor::DrawAssetProfiler() {
		if (!ShowAssetProfiler) {
			return;
		}
		if (!ImGui::Begin("Asset Profiler", &ShowAssetProfiler)) {
			ImGui::End();
			return;
		}

		for (auto &[id, profile] : ContentAssetProfiles) {
			(void)id;
			profile.ResidentInstances = 0;
			profile.StagedInstances = 0;
			profile.StagedBytes = 0;
		}
		for (const engine::render::AssetResidencyStatistics &resident : Renderer.AssetResidencies()) {
			const auto found = ContentAssetProfiles.find(resident.Name.Id());
			if (found == ContentAssetProfiles.end()) {
				continue;
			}
			found->second.ResidentInstances = resident.ResidentInstances;
			found->second.StagedInstances = resident.StagedInstances;
			found->second.StagedBytes = resident.StagedBytes;
		}

		uint64_t pulled = 0;
		uint64_t cpu = 0;
		uint64_t gpu = 0;
		for (const auto &[id, profile] : ContentAssetProfiles) {
			(void)id;
			pulled += profile.PulledBytes;
			cpu += profile.CpuResidentBytes;
			gpu += profile.GpuResidentBytes;
		}
		ImGui::Text(
			"%zu cdn item(s)  pulled %s  cpu %s  gpu %s",
			ContentAssetProfiles.size(),
			Readable(pulled).c_str(),
			Readable(cpu).c_str(),
			Readable(gpu).c_str()
		);
		ImGui::TextDisabled(
			"cpu is retained decoded mesh payload. delta is the latest staged resident-row upload."
		);

		std::vector<AssetProfileSortRow> rows;
		rows.reserve(ContentAssetProfiles.size());
		for (const auto &[id, profile] : ContentAssetProfiles) {
			(void)id;
			rows.push_back(
				{.Name = profile.Name.Text(),
				 .Kind = engine::assets::Describe(profile.Kind),
				 .PulledBytes = profile.PulledBytes,
				 .DecodedBytes = profile.DecodedBytes,
				 .CpuResidentBytes = profile.CpuResidentBytes,
				 .GpuResidentBytes = profile.GpuResidentBytes,
				 .Updates = profile.Updates,
				 .ResidentInstances = profile.ResidentInstances,
				 .StagedInstances = profile.StagedInstances,
				 .StagedBytes = profile.StagedBytes,
				 .Failures = profile.Failures}
			);
		}
		if (ImGui::BeginTable(
				"asset-profile",
				9,
				ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
					ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Sortable | ImGuiTableFlags_SortMulti |
					ImGuiTableFlags_SortTristate
			)) {
			ImGui::TableSetupColumn("asset", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("kind");
			ImGui::TableSetupColumn("pulled");
			ImGui::TableSetupColumn("decoded");
			ImGui::TableSetupColumn("cpu");
			ImGui::TableSetupColumn("gpu");
			ImGui::TableSetupColumn("updates");
			ImGui::TableSetupColumn("resident");
			ImGui::TableSetupColumn("delta");
			ImGui::TableHeadersRow();
			std::vector<AssetProfileSort> sorts;
			if (const ImGuiTableSortSpecs *specs = ImGui::TableGetSortSpecs(); specs != nullptr) {
				sorts.reserve(specs->SpecsCount);
				for (int index = 0; index < specs->SpecsCount; index++) {
					const ImGuiTableColumnSortSpecs &spec = specs->Specs[index];
					sorts.push_back(
						{.Column = static_cast<AssetProfileColumn>(spec.ColumnIndex),
						 .Ascending = spec.SortDirection == ImGuiSortDirection_Ascending}
					);
				}
			}
			const std::array defaultSort{
				AssetProfileSort{.Column = AssetProfileColumn::TotalResident, .Ascending = false}
			};
			const std::span<const AssetProfileSort> requested =
				sorts.empty() ? std::span<const AssetProfileSort>(defaultSort)
							  : std::span<const AssetProfileSort>(sorts);
			SortAssetProfiles(rows, requested);
			for (const AssetProfileSortRow &row : rows) {
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(row.Name.data(), row.Name.data() + row.Name.size());
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(row.Kind.data(), row.Kind.data() + row.Kind.size());
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(Readable(row.PulledBytes).c_str());
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(Readable(row.DecodedBytes).c_str());
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(Readable(row.CpuResidentBytes).c_str());
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(Readable(row.GpuResidentBytes).c_str());
				ImGui::TableNextColumn();
				ImGui::Text("%u", row.Updates);
				ImGui::TableNextColumn();
				ImGui::Text("%u", row.ResidentInstances);
				ImGui::TableNextColumn();
				ImGui::Text("%u / %s", row.StagedInstances, Readable(row.StagedBytes).c_str());
				if (row.Failures > 0) {
					ImGui::SameLine();
					ImGui::TextDisabled("%u failed", row.Failures);
				}
			}
			ImGui::EndTable();
		}
		ImGui::End();
	}
}
