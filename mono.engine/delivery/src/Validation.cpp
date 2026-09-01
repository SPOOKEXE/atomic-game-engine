#include <engine/delivery/Validation.hpp>

#include <algorithm>
#include <cctype>
#include <set>
#include <string>

namespace engine::delivery {

	bool ValidationReport::Passed() const {
		return std::none_of(Findings.begin(), Findings.end(), [](const ValidationFinding &finding) {
			return finding.Severity == ValidationSeverity::Error;
		});
	}

	void ValidationReport::Add(
		std::string code,
		ValidationSeverity severity,
		std::string source,
		std::string path,
		std::string explanation
	) {
		Findings.push_back(
			ValidationFinding{
				std::move(code), severity, std::move(source), std::move(path), std::move(explanation)
			}
		);
	}

	ValidationReport ValidateSources(std::span<const Source> sources, const ValidationOptions &options) {
		ValidationReport report;
		std::set<std::string> names;
		bool readable = false;

		for (size_t index = 0; index < sources.size(); index++) {
			const Source &source = sources[index];
			if (!source.Enabled) {
				continue;
			}
			if (source.Name.size() > 128 || source.Location.size() > 2048 || !source.IsValid()) {
				report.Add(
					"delivery.source.structure",
					ValidationSeverity::Error,
					source.Name,
					source.Location,
					"source name, location, or kind is invalid"
				);
				continue;
			}
			std::string folded = source.Name;
			std::transform(folded.begin(), folded.end(), folded.begin(), [](unsigned char character) {
				return static_cast<char>(std::tolower(character));
			});
			if (!names.insert(std::move(folded)).second) {
				report.Add(
					"delivery.source.duplicate",
					ValidationSeverity::Error,
					source.Name,
					source.Location,
					"source names must be unique without regard to case"
				);
			}
			if (!source.Readable()) {
				continue;
			}
			readable = true;
			if (source.Kind == SourceKind::Http) {
				if (!options.HttpDeclared) {
					report.Add(
						"delivery.http.undeclared",
						ValidationSeverity::Error,
						source.Name,
						source.Location,
						"HTTP source is not declared by the project"
					);
				} else if (!options.HttpAllowed) {
					report.Add(
						"delivery.http.denied",
						ValidationSeverity::Error,
						source.Name,
						source.Location,
						"hosting or export policy refuses outbound HTTP"
					);
				}
			}

			if (!options.CheckReachability) {
				report.Add(
					"delivery.reachability.skipped",
					ValidationSeverity::Skipped,
					source.Name,
					source.Location,
					"online source reachability was not requested"
				);
			} else if (index >= options.Reachable.size()) {
				report.Add(
					"delivery.reachability.missing",
					ValidationSeverity::Error,
					source.Name,
					source.Location,
					"online validation produced no result for this source"
				);
			} else if (!options.Reachable[index]) {
				report.Add(
					"delivery.reachability.failed",
					ValidationSeverity::Error,
					source.Name,
					source.Location,
					"catalogue and signature could not be reached"
				);
			}
		}

		if (!readable) {
			report.Add(
				"delivery.source.none",
				ValidationSeverity::Error,
				"delivery",
				{},
				"no enabled readable content source is configured"
			);
		}
		if (!options.CataloguePresent) {
			report.Add(
				"delivery.catalogue.missing",
				ValidationSeverity::Error,
				"catalogue",
				{},
				"no signed catalogue is available"
			);
		} else if (!options.CatalogueTrusted) {
			report.Add(
				"delivery.catalogue.untrusted",
				ValidationSeverity::Error,
				"catalogue",
				{},
				"catalogue signature does not match the publisher key"
			);
		}
		if (options.RequireComplete && !options.CatalogueComplete) {
			report.Add(
				"delivery.catalogue.incomplete",
				ValidationSeverity::Error,
				"catalogue",
				{},
				"not every catalogue asset is obtainable from the effective sources"
			);
		}

		if (!options.ValidateDeployment) {
			return report;
		}
		if (options.RelayRequested) {
			if (!options.RelayAvailable) {
				report.Add(
					"delivery.relay.unavailable",
					ValidationSeverity::Error,
					"deployment",
					{},
					"relay delivery was requested but the server cannot relay content"
				);
			}
		} else if (!options.RedirectAvailable) {
			report.Add(
				"delivery.redirect.unavailable",
				ValidationSeverity::Error,
				"deployment",
				{},
				"redirect delivery requires an approved public HTTP endpoint and server grants"
			);
		}
		return report;
	}
}
