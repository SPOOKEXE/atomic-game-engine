#pragma once

// Pure validation of ordered content-source and deployment policy.
//
// Network probes happen at an adapter boundary. This layer accepts their
// observations and turns them into one stable report shared by Studio and
// server startup.
//
// @tier L11 · shared

#include <engine/delivery/Source.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace engine::delivery {

	// How one validation finding affects a deployment.
	//
	// @since v0.21
	enum class ValidationSeverity : uint8_t { Error, Warning, Skipped };

	// One stable finding from source or deployment validation.
	//
	// @since v0.21
	struct ValidationFinding {
		// Stable machine-readable identifier for this validation rule.
		std::string Code;
		// Severity assigned to this validation finding.
		ValidationSeverity Severity = ValidationSeverity::Error;
		// Manifest source row that produced the finding.
		std::string Source;
		// Manifest-relative path of the affected file or asset.
		std::string Path;
		// Human-readable explanation of the failed or skipped check.
		std::string Explanation;
	};

	// Findings in source order.
	//
	// @since v0.21
	struct ValidationReport {
		// Findings kept in their declared order.
		std::vector<ValidationFinding> Findings;

		// True when the report contains no error-severity findings.
		bool Passed() const;
		void
		// Appends one source-order finding with its stable rule code and detail.
		Add(std::string code,
			ValidationSeverity severity,
			std::string source,
			std::string path,
			std::string explanation);
	};

	// Runtime observations and policy applied to an ordered source list.
	//
	// `Reachable` is aligned with the source span. An absent row means that
	// source was not checked rather than that it failed.
	//
	// @since v0.21
	struct ValidationOptions {
		// Whether http declared.
		bool HttpDeclared = false;
		// Whether http allowed.
		bool HttpAllowed = false;
		// Whether check reachability.
		bool CheckReachability = false;
		// Whether reachable.
		std::vector<bool> Reachable;
		// Whether catalogue present.
		bool CataloguePresent = false;
		// Whether catalogue trusted.
		bool CatalogueTrusted = false;
		// Whether require complete.
		bool RequireComplete = false;
		// Whether catalogue complete.
		bool CatalogueComplete = false;
		// Whether validate deployment.
		bool ValidateDeployment = false;
		// Whether relay requested.
		bool RelayRequested = true;
		// Whether relay available.
		bool RelayAvailable = false;
		// Whether redirect available.
		bool RedirectAvailable = false;
	};

	// Checks structure, permissions, observations, and deployment viability.
	ValidationReport ValidateSources(std::span<const Source> sources, const ValidationOptions &options);
}
