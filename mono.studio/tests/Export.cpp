#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <studio/Export.hpp>

TEST_SUITE_ID("studio.export")

namespace {
	bool Has(const engine::game::ProjectValidationReport &report, std::string_view code) {
		return std::any_of(report.Findings.begin(), report.Findings.end(), [code](const auto &finding) {
			return finding.Code == code;
		});
	}
}

TEST_CASE("export request enforces product extension", "[studio][export]") {
	studio::ExportOptions options;
	options.Product = engine::game::ExportProduct::ProjectZip;
	engine::game::ProjectValidationReport report;
	auto request = studio::BuildExportRequest("game", options, report);
	REQUIRE(request.has_value());
	CHECK(request->Destination == std::filesystem::path("game.zip"));
	CHECK(request->IncludeProcessedAssets);
	CHECK(request->RequireCompleteCatalogue);
	CHECK(request->Reproducible);

	CHECK_FALSE(studio::BuildExportRequest("game.auniverse", options, report).has_value());
	CHECK(Has(report, "export.destination.extension"));
}

TEST_CASE("world request refuses universe and archive options", "[studio][export]") {
	studio::ExportOptions options;
	options.Product = engine::game::ExportProduct::WorldFile;
	options.IncludePublicCdns = true;
	engine::game::ProjectValidationReport report;
	CHECK_FALSE(studio::BuildExportRequest("world.aworld", options, report).has_value());
	CHECK(Has(report, "export.option.cdn"));

	options.IncludePublicCdns = false;
	options.Reproducible = true;
	CHECK_FALSE(studio::BuildExportRequest("world.aworld", options, report).has_value());
	CHECK(Has(report, "export.option.reproducible"));
}

TEST_CASE("universe request keeps every compatible public choice", "[studio][export]") {
	studio::ExportOptions options;
	options.Product = engine::game::ExportProduct::UniverseFolder;
	options.IncludeProcessedAssets = true;
	options.IncludeRawAuthoring = true;
	options.IncludePublicCdns = true;
	options.ValidateCdnConfiguration = true;
	options.CheckRemoteReachability = true;
	options.RequireCompleteCatalogue = true;
	options.ReplaceExisting = true;
	engine::game::ProjectValidationReport report;
	const auto request = studio::BuildExportRequest("universe.auniverse", options, report);
	REQUIRE(request.has_value());
	CHECK(request->IncludeProcessedAssets);
	CHECK(request->IncludeRawAuthoring);
	CHECK(request->IncludePublicCdns);
	CHECK(request->CheckRemoteReachability);
	CHECK(request->RequireCompleteCatalogue);
	CHECK(request->ReplaceExisting);
}

TEST_CASE("non-package requests reject package-only deployment choices", "[studio][export]") {
	studio::ExportOptions options;
	options.Product = engine::game::ExportProduct::UniverseFolder;
	options.RequireCompleteCatalogue = true;
	engine::game::ProjectValidationReport report;
	CHECK_FALSE(studio::BuildExportRequest("universe.auniverse", options, report).has_value());
	CHECK(Has(report, "export.option.completeness"));

	options.RequireCompleteCatalogue = false;
	options.Delivery = engine::game::ProjectDeliveryPreference::Redirect;
	CHECK_FALSE(studio::BuildExportRequest("universe.auniverse", options, report).has_value());
	CHECK(Has(report, "export.option.delivery"));
}

TEST_CASE("preflight counts worlds and separates errors warnings and skipped checks", "[studio][export]") {
	engine::world::Universe universe;
	engine::world::WorldSettings local;
	local.Name = engine::core::Name("Local");
	REQUIRE(universe.Create(local).IsValid());
	engine::world::WorldSettings remote;
	remote.Name = engine::core::Name("Remote");
	REQUIRE(universe.CreateRemote(remote, engine::core::Name("host")).IsValid());

	studio::ContentSources content;
	content.PublisherKey = "invalid";
	content.Sources.push_back(
		engine::delivery::Source{
			.Name = "public",
			.Kind = engine::delivery::SourceKind::Http,
			.Location = "127.0.0.1:9080",
			.Enabled = true,
			.Role = engine::delivery::SourceRole::Read,
		}
	);
	studio::ExportOptions options;
	options.Product = engine::game::ExportProduct::UniverseFolder;
	options.IncludeProcessedAssets = true;
	options.IncludeRawAuthoring = true;
	options.IncludePublicCdns = true;
	engine::game::ProjectValidationReport requestReport;
	const auto request = studio::BuildExportRequest("universe.auniverse", options, requestReport);
	REQUIRE(request.has_value());
	const studio::ExportPreflight preflight = studio::PreflightExport(*request, universe, content, nullptr);
	CHECK(preflight.LocalWorlds == 1);
	CHECK(preflight.RemoteWorlds == 1);
	CHECK(Has(preflight.Validation, "export.catalogue.unavailable"));
	CHECK(Has(preflight.Validation, "export.authoring.review"));
	CHECK(Has(preflight.Validation, "delivery.reachability.skipped"));
}

TEST_CASE("preflight protects document sidecar and recovery destinations", "[studio][export]") {
	const std::filesystem::path root =
		std::filesystem::temp_directory_path() / "atomic-studio-export-destinations";
	std::error_code ignored;
	std::filesystem::remove_all(root, ignored);
	std::filesystem::create_directories(root / "game.worlds");

	engine::world::Universe universe;
	engine::world::WorldSettings local;
	local.Name = engine::core::Name("Local");
	REQUIRE(universe.Create(local).IsValid());
	studio::ContentSources content;
	studio::ExportOptions options;
	options.Product = engine::game::ExportProduct::UniverseFolder;
	engine::game::ProjectValidationReport requestReport;
	auto request = studio::BuildExportRequest(root / "game.auniverse", options, requestReport);
	REQUIRE(request.has_value());
	auto preflight = studio::PreflightExport(*request, universe, content, nullptr);
	CHECK_FALSE(preflight.Validation.Passed());
	CHECK(Has(preflight.Validation, "export.destination.exists"));

	std::filesystem::remove_all(root / "game.worlds");
	std::filesystem::create_directories(root / "assets");
	preflight = studio::PreflightExport(*request, universe, content, nullptr);
	CHECK_FALSE(preflight.Validation.Passed());
	CHECK(Has(preflight.Validation, "export.destination.exists"));

	std::filesystem::remove_all(root / "assets");
	std::ofstream(root / "game.auniverse") << "old";
	std::ofstream(root / "game.auniverse.previous") << "older";
	options.ReplaceExisting = true;
	request = studio::BuildExportRequest(root / "game.auniverse", options, requestReport);
	REQUIRE(request.has_value());
	preflight = studio::PreflightExport(*request, universe, content, nullptr);
	CHECK_FALSE(preflight.Validation.Passed());
	CHECK(Has(preflight.Validation, "export.destination.backup"));
	std::filesystem::remove_all(root, ignored);
}
