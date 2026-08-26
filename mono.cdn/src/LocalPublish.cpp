#include <engine/assets/LocalStore.hpp>
#include <engine/core/Log.hpp>

#include <cdn/LocalPublish.hpp>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>

namespace cdn {
	namespace {
		// Missing counts as empty because the caller is asking whether there is
		// anything available to publish.
		bool IsEmptyDirectory(const std::filesystem::path &path) {
			std::error_code failure;
			for (const auto &entry : std::filesystem::recursive_directory_iterator(path, failure)) {
				if (failure) {
					break;
				}
				if (entry.is_regular_file(failure)) {
					return false;
				}
			}
			return true;
		}
	}

	std::optional<PublishReport> PublishLocal(
		const engine::assets::LocalPaths &paths,
		const engine::assets::SigningKey &signing,
		uint64_t seconds,
		const PublishSettings &settings
	) {
		if (!engine::assets::EnsureLocalStore(paths)) {
			return std::nullopt;
		}

		if (IsEmptyDirectory(paths.Baked) && !IsEmptyDirectory(paths.Raw)) {
			ENGINE_ERROR(
				"content store: {} is empty and {} is not", paths.Baked.string(), paths.Raw.string()
			);
			ENGINE_ERROR("bake before publishing: `contentimport --publish` and the studio both do");
			return std::nullopt;
		}

		const std::optional<PublishReport> report = Publish(paths.Baked, paths.Processed, signing, settings);
		if (!report.has_value()) {
			return std::nullopt;
		}

		engine::assets::LogEntry entry;
		entry.Seconds = seconds;
		entry.Action = "publish";
		entry.Subject = std::to_string(report->Assets) + " asset(s)";
		entry.Hash = report->Root.ToHex();
		entry.Bytes = report->StoredBytes;
		(void)engine::assets::AppendLog(paths, entry);

		return report;
	}
}
