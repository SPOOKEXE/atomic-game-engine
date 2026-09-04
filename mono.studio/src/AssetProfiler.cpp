#include "AssetProfiler.hpp"

#include <engine/assets/AssetKind.hpp>
#include <engine/render/Renderer.hpp>

#include <algorithm>
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

		std::vector<const ContentAssetProfile *> rows;
		rows.reserve(ContentAssetProfiles.size());
		for (const auto &[id, profile] : ContentAssetProfiles) {
			(void)id;
			rows.push_back(&profile);
		}
		std::sort(
			rows.begin(), rows.end(), [](const ContentAssetProfile *left, const ContentAssetProfile *right) {
				const uint64_t leftBytes = left->GpuResidentBytes + left->CpuResidentBytes;
				const uint64_t rightBytes = right->GpuResidentBytes + right->CpuResidentBytes;
				return leftBytes != rightBytes ? leftBytes > rightBytes
											   : left->Name.Text() < right->Name.Text();
			}
		);

		if (ImGui::BeginTable(
				"asset-profile",
				9,
				ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
					ImGuiTableFlags_SizingFixedFit
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
			for (const ContentAssetProfile *profile : rows) {
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				const std::string_view name = profile->Name.Text();
				ImGui::TextUnformatted(name.data(), name.data() + name.size());
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(engine::assets::Describe(profile->Kind));
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(Readable(profile->PulledBytes).c_str());
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(Readable(profile->DecodedBytes).c_str());
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(Readable(profile->CpuResidentBytes).c_str());
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(Readable(profile->GpuResidentBytes).c_str());
				ImGui::TableNextColumn();
				ImGui::Text("%u", profile->Updates);
				ImGui::TableNextColumn();
				ImGui::Text("%u", profile->ResidentInstances);
				ImGui::TableNextColumn();
				ImGui::Text("%u / %s", profile->StagedInstances, Readable(profile->StagedBytes).c_str());
				if (profile->Failures > 0) {
					ImGui::SameLine();
					ImGui::TextDisabled("%u failed", profile->Failures);
				}
			}
			ImGui::EndTable();
		}
		ImGui::End();
	}
}
