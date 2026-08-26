#include "ControlAutomation.hpp"

#include <algorithm>
#include <cctype>

namespace studio::automation {

	namespace {
		std::string FilenamePart(std::string_view scene) {
			std::string part;
			part.reserve(std::min<size_t>(scene.size(), 96));
			for (const unsigned char letter : scene) {
				if (part.size() == 96) {
					break;
				}
				part.push_back(
					std::isalnum(letter) || letter == '-' || letter == '_' ? static_cast<char>(letter) : '_'
				);
			}
			return part.empty() ? "unnamed" : part;
		}

		bool IncludesScenes(ScreenshotTarget target) {
			return target == ScreenshotTarget::Scene || target == ScreenshotTarget::All;
		}

		bool IncludesStudio(ScreenshotTarget target) {
			return target == ScreenshotTarget::Studio || target == ScreenshotTarget::All;
		}
	}

	bool ParseScreenshotTarget(std::string_view spelling, ScreenshotTarget &target) {
		if (spelling == "scene") {
			target = ScreenshotTarget::Scene;
			return true;
		}
		if (spelling == "studio") {
			target = ScreenshotTarget::Studio;
			return true;
		}
		if (spelling == "all") {
			target = ScreenshotTarget::All;
			return true;
		}
		return false;
	}

	std::vector<ScreenshotTask> PlanScreenshots(
		const std::filesystem::path &directory,
		ScreenshotTarget target,
		std::span<const VisibleScene> visible,
		std::string_view scene,
		std::string &failure
	) {
		failure.clear();
		std::vector<ScreenshotTask> tasks;

		if (!scene.empty() && !IncludesScenes(target)) {
			failure = "scene can only be used with target scene or all";
			return tasks;
		}

		if (IncludesScenes(target)) {
			for (const VisibleScene &view : visible) {
				if (!scene.empty() && view.Name != scene) {
					continue;
				}
				const bool alreadyPlanned =
					std::any_of(tasks.begin(), tasks.end(), [&](const ScreenshotTask &task) {
						return !task.Studio && task.Scene == view.Name;
					});
				if (alreadyPlanned) {
					continue;
				}

				std::string leaf = "scene-" + FilenamePart(view.Name);
				std::filesystem::path path = directory / (leaf + ".bmp");
				for (size_t suffix = 2; std::any_of(
						 tasks.begin(),
						 tasks.end(),
						 [&](const ScreenshotTask &task) { return task.Path == path; }
					 );
					 suffix++) {
					path = directory / (leaf + "-" + std::to_string(suffix) + ".bmp");
				}

				tasks.push_back(
					ScreenshotTask{
						.Path = std::move(path),
						.Scene = view.Name,
						.Slot = view.Slot,
					}
				);
			}

			if (!scene.empty() && tasks.empty()) {
				failure = "no visible viewport shows scene '" + std::string(scene) + "'";
				return {};
			}
			if (target == ScreenshotTarget::Scene && tasks.empty()) {
				failure = "there are no visible scene views to capture";
				return {};
			}
		}

		if (IncludesStudio(target)) {
			tasks.push_back(
				ScreenshotTask{
					.Path = directory / "studio.bmp",
					.Scene = {},
					.Slot = 0,
					.Studio = true,
				}
			);
		}
		return tasks;
	}
}
