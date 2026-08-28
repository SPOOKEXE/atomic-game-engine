#include <cmath>
#include <studio/PluginReload.hpp>
#include <utility>

namespace studio {
	namespace {
		constexpr double DEFAULT_POLL_INTERVAL_SECONDS = 0.25;
		constexpr double DEFAULT_DEBOUNCE_INTERVAL_SECONDS = 0.25;
		constexpr const char *PLUGIN_MANIFEST_NAME = "plugin.json";

		double ValidInterval(double intervalSeconds, double fallbackSeconds) {
			if (!std::isfinite(intervalSeconds) || intervalSeconds < 0.0) {
				return fallbackSeconds;
			}
			return intervalSeconds;
		}

		PluginReloadIssue InvalidRootIssue(const PluginReloadRoot &root) {
			return PluginReloadIssue{
				.PluginId = root.Id,
				.Root = root.Path,
				.Error = std::make_error_code(std::errc::invalid_argument),
			};
		}
	}

	PluginReloadTracker::PluginReloadTracker(PluginReloadConfig config) : Config(config) {
		Config.PollIntervalSeconds = ValidInterval(Config.PollIntervalSeconds, DEFAULT_POLL_INTERVAL_SECONDS);
		Config.DebounceIntervalSeconds =
			ValidInterval(Config.DebounceIntervalSeconds, DEFAULT_DEBOUNCE_INTERVAL_SECONDS);
	}

	PluginReloadTracker::RootSnapshot PluginReloadTracker::Capture(const std::filesystem::path &root) const {
		RootSnapshot snapshot;
		snapshot.Path = root.lexically_normal();

		std::error_code error;
		const std::filesystem::file_status rootStatus = std::filesystem::status(snapshot.Path, error);
		if (error == std::errc::no_such_file_or_directory) {
			return snapshot;
		}
		if (error) {
			snapshot.Error = error;
			return snapshot;
		}
		if (!std::filesystem::exists(rootStatus)) {
			return snapshot;
		}

		snapshot.Exists = true;
		if (!std::filesystem::is_directory(rootStatus)) {
			snapshot.Error = std::make_error_code(std::errc::not_a_directory);
			return snapshot;
		}

		std::filesystem::recursive_directory_iterator walk(
			snapshot.Path, std::filesystem::directory_options::none, error
		);
		const std::filesystem::recursive_directory_iterator end;
		if (error) {
			snapshot.Error = error;
			return snapshot;
		}

		while (walk != end) {
			const bool regular = walk->is_regular_file(error);
			if (error) {
				snapshot.Error = error;
				return snapshot;
			}

			if (regular) {
				const std::filesystem::path relative = walk->path().lexically_relative(snapshot.Path);
				const std::uintmax_t size = walk->file_size(error);
				if (error) {
					snapshot.Error = error;
					return snapshot;
				}

				const std::filesystem::file_time_type modifiedAt = walk->last_write_time(error);
				if (error) {
					snapshot.Error = error;
					return snapshot;
				}

				snapshot.Files[relative.generic_string()] = FileMetadata{
					.Size = size,
					.ModifiedAt = modifiedAt,
				};
			}

			walk.increment(error);
			if (error) {
				snapshot.Error = error;
				return snapshot;
			}
		}

		return snapshot;
	}

	PluginReloadTracker::ChangeKind
	PluginReloadTracker::Classify(const RootSnapshot &before, const RootSnapshot &after) const {
		if (before.Path != after.Path || before.Exists != after.Exists || before.Error != after.Error) {
			return ChangeKind::FullRescan;
		}
		if (after.Error) {
			return ChangeKind::None;
		}
		if (before.Files == after.Files) {
			return ChangeKind::None;
		}

		const auto manifestBefore = before.Files.find(PLUGIN_MANIFEST_NAME);
		const auto manifestAfter = after.Files.find(PLUGIN_MANIFEST_NAME);
		if (manifestBefore == before.Files.end() || manifestAfter == after.Files.end() ||
			manifestBefore->second != manifestAfter->second) {
			return ChangeKind::FullRescan;
		}
		return ChangeKind::Targeted;
	}

	void PluginReloadTracker::QueueTargeted(const std::string &pluginId, double nowSeconds) {
		if (FullRescanPending) {
			return;
		}
		TargetedReadySeconds[pluginId] = nowSeconds + Config.DebounceIntervalSeconds;
	}

	void PluginReloadTracker::QueueFullRescan(double nowSeconds) {
		FullRescanPending = true;
		FullRescanReadySeconds = nowSeconds + Config.DebounceIntervalSeconds;
		TargetedReadySeconds.clear();
	}

	void PluginReloadTracker::Poll(
		std::span<const PluginReloadRoot> roots, double nowSeconds, PluginReloadBatch &batch
	) {
		std::map<std::string, std::filesystem::path> requestedRoots;
		for (const PluginReloadRoot &root : roots) {
			if (root.Id.empty() || root.Path.empty()) {
				batch.Issues.push_back(InvalidRootIssue(root));
				continue;
			}

			const auto [_, inserted] = requestedRoots.emplace(root.Id, root.Path.lexically_normal());
			if (!inserted) {
				batch.Issues.push_back(InvalidRootIssue(root));
			}
		}

		std::map<std::string, RootSnapshot> captured;
		for (const auto &[pluginId, root] : requestedRoots) {
			RootSnapshot snapshot = Capture(root);
			if (snapshot.Error) {
				batch.Issues.push_back(
					PluginReloadIssue{
						.PluginId = pluginId,
						.Root = root,
						.Error = snapshot.Error,
					}
				);
			}
			captured.emplace(pluginId, std::move(snapshot));
		}

		if (!Initialized) {
			Snapshots = std::move(captured);
			Initialized = true;
			return;
		}

		bool rootsChanged = Snapshots.size() != captured.size();
		for (const auto &[pluginId, after] : captured) {
			const auto before = Snapshots.find(pluginId);
			if (before == Snapshots.end()) {
				rootsChanged = true;
				continue;
			}
			if (before->second.Path != after.Path) {
				rootsChanged = true;
			}
		}
		if (rootsChanged) {
			QueueFullRescan(nowSeconds);
		}

		for (const auto &[pluginId, after] : captured) {
			const auto before = Snapshots.find(pluginId);
			if (before == Snapshots.end() || before->second.Path != after.Path) {
				continue;
			}

			switch (Classify(before->second, after)) {
			case ChangeKind::None:
				break;
			case ChangeKind::Targeted:
				QueueTargeted(pluginId, nowSeconds);
				break;
			case ChangeKind::FullRescan:
				QueueFullRescan(nowSeconds);
				break;
			}
		}

		Snapshots = std::move(captured);
	}

	void PluginReloadTracker::EmitReady(double nowSeconds, PluginReloadBatch &batch) {
		if (FullRescanPending && nowSeconds >= FullRescanReadySeconds) {
			batch.Action = PluginReloadAction::RescanPlugins;
			FullRescanPending = false;
			TargetedReadySeconds.clear();
			return;
		}

		for (auto pending = TargetedReadySeconds.begin(); pending != TargetedReadySeconds.end();) {
			if (nowSeconds < pending->second) {
				++pending;
				continue;
			}
			batch.PluginIds.push_back(pending->first);
			pending = TargetedReadySeconds.erase(pending);
		}
		if (!batch.PluginIds.empty()) {
			batch.Action = PluginReloadAction::ReloadPlugins;
		}
	}

	PluginReloadBatch PluginReloadTracker::Pump(std::span<const PluginReloadRoot> roots, double nowSeconds) {
		PluginReloadBatch batch;
		if (!Initialized || nowSeconds >= NextPollSeconds) {
			Poll(roots, nowSeconds, batch);
			NextPollSeconds = nowSeconds + Config.PollIntervalSeconds;
		}
		EmitReady(nowSeconds, batch);
		return batch;
	}

	void PluginReloadTracker::RequestRescan(double nowSeconds) {
		if (!FullRescanPending) {
			QueueFullRescan(nowSeconds);
		}
	}

	void PluginReloadTracker::Reset() {
		Initialized = false;
		NextPollSeconds = 0.0;
		FullRescanPending = false;
		FullRescanReadySeconds = 0.0;
		TargetedReadySeconds.clear();
		Snapshots.clear();
	}
}
