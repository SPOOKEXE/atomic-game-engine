#include <engine/delivery/Validation.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string_view>
#include <vector>

TEST_SUITE_ID("engine.delivery.validation")

namespace {
	bool Has(const engine::delivery::ValidationReport &report, std::string_view code) {
		return std::any_of(report.Findings.begin(), report.Findings.end(), [code](const auto &finding) {
			return finding.Code == code;
		});
	}

	engine::delivery::Source Http(std::string name = "public") {
		return engine::delivery::Source{
			.Name = std::move(name),
			.Kind = engine::delivery::SourceKind::Http,
			.Location = "127.0.0.1:9080",
			.Enabled = true,
			.Role = engine::delivery::SourceRole::Read,
		};
	}

	engine::delivery::ValidationOptions ValidOptions() {
		return engine::delivery::ValidationOptions{
			.HttpDeclared = true,
			.HttpAllowed = true,
			.CheckReachability = true,
			.Reachable = {true},
			.CataloguePresent = true,
			.CatalogueTrusted = true,
			.RequireComplete = true,
			.CatalogueComplete = true,
			.ValidateDeployment = true,
			.RelayRequested = true,
			.RelayAvailable = true,
		};
	}
}

TEST_CASE("a valid effective source list passes all validation levels", "[delivery][validation]") {
	const std::vector sources{Http()};
	const auto report = engine::delivery::ValidateSources(sources, ValidOptions());
	CHECK(report.Passed());
	CHECK(report.Findings.empty());
}

TEST_CASE(
	"source validation reports structure permission and reachability separately", "[delivery][validation]"
) {
	std::vector sources{Http("same"), Http("SAME")};
	auto options = ValidOptions();
	options.HttpDeclared = false;
	options.HttpAllowed = false;
	options.Reachable = {false, true};
	const auto report = engine::delivery::ValidateSources(sources, options);
	CHECK_FALSE(report.Passed());
	CHECK(Has(report, "delivery.source.duplicate"));
	CHECK(Has(report, "delivery.http.undeclared"));
	CHECK(Has(report, "delivery.reachability.failed"));
}

TEST_CASE("offline validation records skipped checks", "[delivery][validation]") {
	const std::vector sources{Http()};
	auto options = ValidOptions();
	options.CheckReachability = false;
	const auto report = engine::delivery::ValidateSources(sources, options);
	CHECK(report.Passed());
	CHECK(Has(report, "delivery.reachability.skipped"));
}

TEST_CASE("trust completeness and deployment viability fail closed", "[delivery][validation]") {
	const std::vector sources{Http()};
	auto options = ValidOptions();
	options.CatalogueTrusted = false;
	options.CatalogueComplete = false;
	options.RelayAvailable = false;
	const auto report = engine::delivery::ValidateSources(sources, options);
	CHECK_FALSE(report.Passed());
	CHECK(Has(report, "delivery.catalogue.untrusted"));
	CHECK(Has(report, "delivery.catalogue.incomplete"));
	CHECK(Has(report, "delivery.relay.unavailable"));
}

TEST_CASE("redirect requires an approved endpoint and server grants", "[delivery][validation]") {
	const std::vector sources{Http()};
	auto options = ValidOptions();
	options.RelayRequested = false;
	options.RedirectAvailable = false;
	auto report = engine::delivery::ValidateSources(sources, options);
	CHECK(Has(report, "delivery.redirect.unavailable"));

	options.RedirectAvailable = true;
	report = engine::delivery::ValidateSources(sources, options);
	CHECK(report.Passed());

	const std::vector localSources{engine::delivery::Source{
		.Name = "package",
		.Kind = engine::delivery::SourceKind::Directory,
		.Location = "assets",
		.Enabled = true,
		.Role = engine::delivery::SourceRole::Read,
	}};
	options.Reachable = {true};
	report = engine::delivery::ValidateSources(localSources, options);
	CHECK(report.Passed());
}
