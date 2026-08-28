// Automatic plugin reload change tracking without an editor frame or runtime.
//
// The clock is explicit in every case. Files are real because metadata capture
// is the boundary under test, but no case sleeps or depends on wall time.

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <studio/PluginReload.hpp>
#include <system_error>
#include <vector>

TEST_SUITE_ID("studio.pluginreload")

using studio::PluginReloadAction;
using studio::PluginReloadBatch;
using studio::PluginReloadConfig;
using studio::PluginReloadRoot;
using studio::PluginReloadTracker;

namespace {
	struct PluginFolders {
		std::filesystem::path Root;

		PluginFolders() {
			std::error_code error;
			Root = std::filesystem::temp_directory_path(error) / "atomic-studio-plugin-reload";
			REQUIRE_FALSE(error);
			std::filesystem::remove_all(Root, error);
			REQUIRE_FALSE(error);
			std::filesystem::create_directories(Root, error);
			REQUIRE_FALSE(error);
		}

		~PluginFolders() {
			std::error_code error;
			std::filesystem::remove_all(Root, error);
		}

		std::filesystem::path Add(const std::string &id) {
			const std::filesystem::path plugin = Root / id;
			Write(plugin / "plugin.json", R"({"name":"Test"})");
			Write(plugin / "main.luau", "return 1");
			return plugin;
		}

		void Write(const std::filesystem::path &path, const std::string &text) {
			std::error_code error;
			std::filesystem::create_directories(path.parent_path(), error);
			REQUIRE_FALSE(error);
			std::ofstream output(path, std::ios::binary | std::ios::trunc);
			REQUIRE(output.is_open());
			output << text;
			output.close();
			REQUIRE(output.good());
		}
	};

	PluginReloadBatch
	Pump(PluginReloadTracker &tracker, const std::vector<PluginReloadRoot> &roots, double nowSeconds) {
		return tracker.Pump(std::span<const PluginReloadRoot>(roots), nowSeconds);
	}
}

TEST_CASE("unchanged plugin roots produce no work at or between polls", "[studio][pluginreload]") {
	PluginFolders folders;
	const std::vector roots{PluginReloadRoot{.Id = "alpha", .Path = folders.Add("alpha")}};
	PluginReloadTracker tracker;

	CHECK(Pump(tracker, roots, 10.0).Action == PluginReloadAction::None);
	CHECK(Pump(tracker, roots, 10.1).Action == PluginReloadAction::None);
	CHECK(Pump(tracker, roots, 10.25).Action == PluginReloadAction::None);
	CHECK(Pump(tracker, roots, 11.0).Action == PluginReloadAction::None);
}

TEST_CASE("a modified source requests one targeted reload after quiet time", "[studio][pluginreload]") {
	PluginFolders folders;
	const std::filesystem::path alpha = folders.Add("alpha");
	const std::vector roots{PluginReloadRoot{.Id = "alpha", .Path = alpha}};
	PluginReloadTracker tracker;
	REQUIRE(Pump(tracker, roots, 0.0).Action == PluginReloadAction::None);

	std::error_code error;
	const std::filesystem::file_time_type before =
		std::filesystem::last_write_time(alpha / "main.luau", error);
	REQUIRE_FALSE(error);
	folders.Write(alpha / "main.luau", "return 2");
	std::filesystem::last_write_time(alpha / "main.luau", before + std::chrono::seconds(1), error);
	REQUIRE_FALSE(error);
	CHECK(Pump(tracker, roots, 0.25).Action == PluginReloadAction::None);
	CHECK(Pump(tracker, roots, 0.49).Action == PluginReloadAction::None);
	const PluginReloadBatch ready = Pump(tracker, roots, 0.50);
	CHECK(ready.Action == PluginReloadAction::ReloadPlugins);
	CHECK(ready.PluginIds == std::vector<std::string>{"alpha"});
	CHECK(Pump(tracker, roots, 0.75).Action == PluginReloadAction::None);
}

TEST_CASE("source change bursts extend one deterministic debounce window", "[studio][pluginreload]") {
	PluginFolders folders;
	const std::filesystem::path alpha = folders.Add("alpha");
	const std::vector roots{PluginReloadRoot{.Id = "alpha", .Path = alpha}};
	PluginReloadTracker tracker;
	REQUIRE(Pump(tracker, roots, 0.0).Action == PluginReloadAction::None);

	folders.Write(alpha / "main.luau", "return 200");
	CHECK(Pump(tracker, roots, 0.25).Action == PluginReloadAction::None);
	folders.Write(alpha / "src" / "helper.luau", "return 3");
	CHECK(Pump(tracker, roots, 0.50).Action == PluginReloadAction::None);
	CHECK(Pump(tracker, roots, 0.74).Action == PluginReloadAction::None);

	const PluginReloadBatch ready = Pump(tracker, roots, 0.75);
	CHECK(ready.Action == PluginReloadAction::ReloadPlugins);
	CHECK(ready.PluginIds == std::vector<std::string>{"alpha"});
}

TEST_CASE("ordinary source creation and deletion are targeted", "[studio][pluginreload]") {
	SECTION("creation") {
		PluginFolders folders;
		const std::filesystem::path alpha = folders.Add("alpha");
		const std::vector roots{PluginReloadRoot{.Id = "alpha", .Path = alpha}};
		PluginReloadTracker tracker;
		REQUIRE(Pump(tracker, roots, 0.0).Action == PluginReloadAction::None);

		folders.Write(alpha / "nested" / "new.luau", "return true");
		CHECK(Pump(tracker, roots, 0.25).Action == PluginReloadAction::None);
		const PluginReloadBatch ready = Pump(tracker, roots, 0.50);
		CHECK(ready.Action == PluginReloadAction::ReloadPlugins);
		CHECK(ready.PluginIds == std::vector<std::string>{"alpha"});
	}

	SECTION("deletion") {
		PluginFolders folders;
		const std::filesystem::path alpha = folders.Add("alpha");
		folders.Write(alpha / "nested" / "old.luau", "return false");
		const std::vector roots{PluginReloadRoot{.Id = "alpha", .Path = alpha}};
		PluginReloadTracker tracker;
		REQUIRE(Pump(tracker, roots, 0.0).Action == PluginReloadAction::None);

		std::error_code error;
		REQUIRE(std::filesystem::remove(alpha / "nested" / "old.luau", error));
		REQUIRE_FALSE(error);
		CHECK(Pump(tracker, roots, 0.25).Action == PluginReloadAction::None);
		const PluginReloadBatch ready = Pump(tracker, roots, 0.50);
		CHECK(ready.Action == PluginReloadAction::ReloadPlugins);
		CHECK(ready.PluginIds == std::vector<std::string>{"alpha"});
	}
}

TEST_CASE("manifest and plugin root structure changes request a full rescan", "[studio][pluginreload]") {
	SECTION("manifest") {
		PluginFolders folders;
		const std::filesystem::path alpha = folders.Add("alpha");
		const std::vector roots{PluginReloadRoot{.Id = "alpha", .Path = alpha}};
		PluginReloadTracker tracker;
		REQUIRE(Pump(tracker, roots, 0.0).Action == PluginReloadAction::None);

		folders.Write(alpha / "plugin.json", R"({"name":"Renamed","version":"2"})");
		CHECK(Pump(tracker, roots, 0.25).Action == PluginReloadAction::None);
		CHECK(Pump(tracker, roots, 0.50).Action == PluginReloadAction::RescanPlugins);
	}

	SECTION("root set") {
		PluginFolders folders;
		const std::filesystem::path alpha = folders.Add("alpha");
		const std::filesystem::path beta = folders.Add("beta");
		std::vector roots{PluginReloadRoot{.Id = "alpha", .Path = alpha}};
		PluginReloadTracker tracker;
		REQUIRE(Pump(tracker, roots, 0.0).Action == PluginReloadAction::None);

		roots.push_back(PluginReloadRoot{.Id = "beta", .Path = beta});
		CHECK(Pump(tracker, roots, 0.25).Action == PluginReloadAction::None);
		CHECK(Pump(tracker, roots, 0.50).Action == PluginReloadAction::RescanPlugins);
	}

	SECTION("root disappearance") {
		PluginFolders folders;
		const std::filesystem::path alpha = folders.Add("alpha");
		const std::vector roots{PluginReloadRoot{.Id = "alpha", .Path = alpha}};
		PluginReloadTracker tracker;
		REQUIRE(Pump(tracker, roots, 0.0).Action == PluginReloadAction::None);

		std::error_code error;
		std::filesystem::remove_all(alpha, error);
		REQUIRE_FALSE(error);
		CHECK(Pump(tracker, roots, 0.25).Action == PluginReloadAction::None);
		CHECK(Pump(tracker, roots, 0.50).Action == PluginReloadAction::RescanPlugins);
	}
}

TEST_CASE(
	"an external structural rescan is debounced without deadline starvation", "[studio][pluginreload]"
) {
	PluginReloadTracker tracker;
	const std::vector<PluginReloadRoot> roots;
	REQUIRE(Pump(tracker, roots, 0.0).Action == PluginReloadAction::None);

	tracker.RequestRescan(0.10);
	tracker.RequestRescan(0.20);
	CHECK(Pump(tracker, roots, 0.34).Action == PluginReloadAction::None);
	tracker.RequestRescan(0.34);
	CHECK(Pump(tracker, roots, 0.35).Action == PluginReloadAction::RescanPlugins);
}

TEST_CASE("targeted noise does not postpone a pending full rescan", "[studio][pluginreload]") {
	PluginFolders folders;
	const std::filesystem::path alpha = folders.Add("alpha");
	const std::filesystem::path beta = folders.Add("beta");
	const std::vector roots{
		PluginReloadRoot{.Id = "alpha", .Path = alpha},
		PluginReloadRoot{.Id = "beta", .Path = beta},
	};
	PluginReloadTracker tracker(
		PluginReloadConfig{.PollIntervalSeconds = 0.10, .DebounceIntervalSeconds = 0.25}
	);
	REQUIRE(Pump(tracker, roots, 0.0).Action == PluginReloadAction::None);

	folders.Write(alpha / "plugin.json", R"({"name":"Alpha 2"})");
	CHECK(Pump(tracker, roots, 0.10).Action == PluginReloadAction::None);
	folders.Write(beta / "main.luau", "return 2");
	CHECK(Pump(tracker, roots, 0.20).Action == PluginReloadAction::None);
	folders.Write(beta / "main.luau", "return 3");
	CHECK(Pump(tracker, roots, 0.30).Action == PluginReloadAction::None);
	CHECK(Pump(tracker, roots, 0.35).Action == PluginReloadAction::RescanPlugins);
}

TEST_CASE("plugin roots debounce independently and ready ids are sorted", "[studio][pluginreload]") {
	PluginFolders folders;
	const std::filesystem::path alpha = folders.Add("alpha");
	const std::filesystem::path zeta = folders.Add("zeta");
	const std::vector roots{
		PluginReloadRoot{.Id = "zeta", .Path = zeta},
		PluginReloadRoot{.Id = "alpha", .Path = alpha},
	};
	PluginReloadTracker tracker(
		PluginReloadConfig{
			.PollIntervalSeconds = 0.10,
			.DebounceIntervalSeconds = 0.25,
		}
	);
	REQUIRE(Pump(tracker, roots, 0.0).Action == PluginReloadAction::None);

	folders.Write(alpha / "main.luau", "return 200");
	CHECK(Pump(tracker, roots, 0.10).Action == PluginReloadAction::None);
	folders.Write(zeta / "main.luau", "return 3000");
	CHECK(Pump(tracker, roots, 0.20).Action == PluginReloadAction::None);

	const PluginReloadBatch alphaReady = Pump(tracker, roots, 0.36);
	CHECK(alphaReady.Action == PluginReloadAction::ReloadPlugins);
	CHECK(alphaReady.PluginIds == std::vector<std::string>{"alpha"});
	const PluginReloadBatch zetaReady = Pump(tracker, roots, 0.46);
	CHECK(zetaReady.Action == PluginReloadAction::ReloadPlugins);
	CHECK(zetaReady.PluginIds == std::vector<std::string>{"zeta"});

	folders.Write(alpha / "extra.luau", "alpha");
	folders.Write(zeta / "extra.luau", "zeta");
	CHECK(Pump(tracker, roots, 0.60).Action == PluginReloadAction::None);
	const PluginReloadBatch bothReady = Pump(tracker, roots, 0.86);
	CHECK(bothReady.Action == PluginReloadAction::ReloadPlugins);
	CHECK(bothReady.PluginIds == std::vector<std::string>{"alpha", "zeta"});
}
