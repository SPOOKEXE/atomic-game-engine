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
		std::string Code;
		ValidationSeverity Severity = ValidationSeverity::Error;
		std::string Source;
		std::string Path;
		std::string Explanation;
	};

	// Findings in source order.
	//
	// @since v0.21
	struct ValidationReport {
		std::vector<ValidationFinding> Findings;

		bool Passed() const;
		void
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
		bool HttpDeclared = false;
		bool HttpAllowed = false;
		bool CheckReachability = false;
		std::vector<bool> Reachable;
		bool CataloguePresent = false;
		bool CatalogueTrusted = false;
		bool RequireComplete = false;
		bool CatalogueComplete = false;
		bool ValidateDeployment = false;
		bool RelayRequested = true;
		bool RelayAvailable = false;
		bool RedirectAvailable = false;
	};

	// Checks structure, permissions, observations, and deployment viability.
	ValidationReport ValidateSources(std::span<const Source> sources, const ValidationOptions &options);
}
