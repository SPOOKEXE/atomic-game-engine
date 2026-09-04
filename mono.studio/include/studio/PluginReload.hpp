#pragma once

// arch-waiver public-header: forward studio API. Plugin hosts share this
// complete reload lifecycle contract.

// Headless plugin source change tracking and debounce.
//
// The tracker reads filesystem metadata only when a caller-supplied monotonic
// deadline is due. It does not own a clock, thread, callback or plugin runtime,
// which keeps the decision usable from `Editor::PumpPlugins` and unit tests.
//
// Relative file names are the snapshot identity. Absolute plugin roots may move
// between sessions without changing the meaning of a file inside one root.
//
// @tier client

#include <cstdint>
#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <system_error>
#include <vector>

namespace studio {
	// One plugin folder to monitor.
	//
	// @since v0.20
	struct PluginReloadRoot {
		// Stable watched-root identity. Studio uses the normalized plugin path so
		// duplicate manifest ids remain independently observable.
		std::string Id;

		// Absolute plugin folder containing `plugin.json` and its source tree.
		std::filesystem::path Path;
	};

	// Timing policy for metadata polling and burst coalescing.
	//
	// Both durations use seconds. Zero requests work on every call; negative or
	// non-finite values are replaced with the defaults.
	//
	// @since v0.20
	struct PluginReloadConfig {
		// Minimum time between recursive metadata snapshots.
		double PollIntervalSeconds = 0.25;

		// Quiet time after the most recent detected change before it is reported.
		double DebounceIntervalSeconds = 0.25;
	};

	// Work the plugin host should perform after a tracker pump.
	//
	// @since v0.20
	enum class PluginReloadAction {
		// No debounced work is ready.
		None,

		// Restart only the plugins named by `PluginReloadBatch::PluginIds`.
		ReloadPlugins,

		// Rediscover plugin roots and manifests before restarting plugins.
		RescanPlugins,
	};

	// A filesystem problem observed during one metadata poll.
	//
	// Issues do not throw and do not stop other roots from being checked.
	//
	// @since v0.20
	struct PluginReloadIssue {
		// Stable identity of the root that could not be inspected.
		std::string PluginId;

		// Folder that was being inspected.
		std::filesystem::path Root;

		// Filesystem or input-validation error.
		std::error_code Error;
	};

	// Deterministic output from one tracker pump.
	//
	// Plugin ids are sorted and unique. They are populated only for
	// `ReloadPlugins`; a `RescanPlugins` action supersedes targeted reloads.
	//
	// @since v0.20
	struct PluginReloadBatch {
		// The work that matured on this pump.
		PluginReloadAction Action = PluginReloadAction::None;

		// Stable ids to restart for a targeted action.
		std::vector<std::string> PluginIds;

		// Problems observed by a metadata poll performed on this call.
		std::vector<PluginReloadIssue> Issues;
	};

	// Periodically snapshots plugin roots and debounces their changes.
	//
	// The first Pump establishes a baseline and never requests a reload. A root
	// set change, root availability change or top-level `plugin.json` change
	// requests a full rescan. Other regular-file changes request a targeted
	// reload for that root.
	//
	// @since v0.20
	class PluginReloadTracker {
	  public:
		// Creates a tracker with caller-selected intervals.
		explicit PluginReloadTracker(PluginReloadConfig config = {});

		// Polls when due and returns any work whose debounce deadline has matured.
		//
		// `nowSeconds` is an absolute timestamp from one monotonic clock chosen by
		// the caller. Root ids must be non-empty and unique.
		PluginReloadBatch Pump(std::span<const PluginReloadRoot> roots, double nowSeconds);

		// Requests one debounced structural rescan without extending an already
		// pending deadline on every repeated observation.
		void RequestRescan(double nowSeconds);

		// Forgets snapshots and pending work while retaining the timing policy.
		void Reset();

	  private:
		struct FileMetadata {
			std::uintmax_t Size = 0;
			std::filesystem::file_time_type ModifiedAt;

			bool operator==(const FileMetadata &) const = default;
		};

		struct RootSnapshot {
			std::filesystem::path Path;
			bool Exists = false;
			std::error_code Error;
			std::map<std::string, FileMetadata> Files;
		};

		enum class ChangeKind {
			None,
			Targeted,
			FullRescan,
		};

		RootSnapshot Capture(const std::filesystem::path &root) const;
		ChangeKind Classify(const RootSnapshot &before, const RootSnapshot &after) const;
		void Poll(std::span<const PluginReloadRoot> roots, double nowSeconds, PluginReloadBatch &batch);
		void QueueTargeted(const std::string &pluginId, double nowSeconds);
		void QueueFullRescan(double nowSeconds);
		void EmitReady(double nowSeconds, PluginReloadBatch &batch);

		PluginReloadConfig Config;
		bool Initialized = false;
		double NextPollSeconds = 0.0;
		bool FullRescanPending = false;
		double FullRescanReadySeconds = 0.0;
		std::map<std::string, double> TargetedReadySeconds;
		std::map<std::string, RootSnapshot> Snapshots;
	};
}
