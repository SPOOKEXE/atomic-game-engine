#pragma once
#include <engine/scene/ImageGraphBinding.hpp>

#include <filesystem>
#include <span>
namespace client::detail {
	struct SourceSkyboxCaptureView {
		uint64_t StoreIdentity = 0;
		std::span<const engine::scene::ImageGraphBinding> Selectors;
		std::span<const engine::ecs::Entity> Entities;
		std::span<const std::filesystem::file_time_type> Modified;
		std::span<const uintmax_t> FileBytes;
	};
	// Advancing world ticks do not invalidate a submitted cohort; authored controls and source revisions do.
	inline bool SameSourceSkyboxCapture(SourceSkyboxCaptureView old, SourceSkyboxCaptureView current) {
		if (old.StoreIdentity != current.StoreIdentity || old.Selectors.size() != 6 ||
			current.Selectors.size() != 6 || old.Entities.size() != 6 || current.Entities.size() != 6 ||
			old.Modified.size() != 6 || current.Modified.size() != 6 || old.FileBytes.size() != 6 ||
			current.FileBytes.size() != 6)
			return false;
		for (size_t i = 0; i < 6; ++i) {
			const auto &a = old.Selectors[i], &b = current.Selectors[i];
			if (a.Graph != b.Graph || a.Output != b.Output || a.Texture != b.Texture || a.Seed != b.Seed ||
				a.FixedTick != b.FixedTick || a.TickPolicy != b.TickPolicy || a.ColorSpace != b.ColorSpace ||
				old.Entities[i] != current.Entities[i] || old.Modified[i] != current.Modified[i] ||
				old.FileBytes[i] != current.FileBytes[i])
				return false;
		}
		return true;
	}
}
