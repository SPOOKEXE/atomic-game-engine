#include <engine/assets/Signature.hpp>
#include <engine/delivery/Validation.hpp>

#include <algorithm>
#include <filesystem>
#include <limits>
#include <string>
#include <studio/Export.hpp>
#include <utility>

namespace studio {
	namespace {
		using engine::game::ExportProduct;
		using engine::game::ProjectFindingSeverity;

		void Error(
			engine::game::ProjectValidationReport &report,
			std::string code,
			std::string path,
			std::string explanation
		) {
			report.Add(
				std::move(code),
				ProjectFindingSeverity::Error,
				"export",
				std::move(path),
				std::move(explanation)
			);
		}

		void
		Convert(engine::game::ProjectValidationReport &into, const engine::delivery::ValidationReport &from) {
			for (const engine::delivery::ValidationFinding &finding : from.Findings) {
				ProjectFindingSeverity severity = ProjectFindingSeverity::Error;
				if (finding.Severity == engine::delivery::ValidationSeverity::Warning) {
					severity = ProjectFindingSeverity::Warning;
				} else if (finding.Severity == engine::delivery::ValidationSeverity::Skipped) {
					severity = ProjectFindingSeverity::Skipped;
				}
				into.Add(finding.Code, severity, finding.Source, finding.Path, finding.Explanation);
			}
		}

		bool AddChecked(uint64_t &total, uint64_t amount) {
			if (amount > std::numeric_limits<uint64_t>::max() - total) {
				return false;
			}
			total += amount;
			return true;
		}
	}

	const char *Describe(ExportPhase phase) {
		switch (phase) {
		case ExportPhase::Idle:
			return "idle";
		case ExportPhase::SerializeWorlds:
			return "serialize worlds";
		case ExportPhase::ValidateCatalogue:
			return "validate catalogue";
		case ExportPhase::FetchAssets:
			return "fetch assets";
		case ExportPhase::CopyRawFiles:
			return "copy raw files";
		case ExportPhase::ValidateCdnSources:
			return "validate CDN sources";
		case ExportPhase::BuildArchive:
			return "build archive";
		case ExportPhase::VerifyArchive:
			return "verify archive";
		case ExportPhase::PublishResult:
			return "publish result";
		case ExportPhase::Complete:
			return "complete";
		case ExportPhase::Failed:
			return "failed";
		case ExportPhase::Cancelled:
			return "cancelled";
		}
		return "unknown";
	}

	std::optional<ExportRequest> BuildExportRequest(
		const std::filesystem::path &destination,
		const ExportOptions &options,
		engine::game::ProjectValidationReport &report
	) {
		report = {};
		if (destination.empty()) {
			Error(report, "export.destination.empty", {}, "choose an export destination");
			return std::nullopt;
		}
		const std::filesystem::path expected(engine::game::ExtensionOf(options.Product));
		std::filesystem::path normalized = destination;
		if (normalized.extension().empty()) {
			normalized += expected.string();
		} else if (normalized.extension() != expected) {
			Error(
				report,
				"export.destination.extension",
				destination.string(),
				"destination extension does not match the selected export product"
			);
			return std::nullopt;
		}

		if (options.Product == ExportProduct::WorldFile && options.IncludePublicCdns) {
			Error(report, "export.option.cdn", {}, "a world file does not carry universe CDN declarations");
		}
		if (options.Product != ExportProduct::ProjectZip && options.RequireCompleteCatalogue &&
			!options.IncludeProcessedAssets) {
			Error(
				report, "export.option.completeness", {}, "catalogue completeness requires processed assets"
			);
		}
		if (options.Product != ExportProduct::ProjectZip &&
			options.Delivery != engine::game::ProjectDeliveryPreference::Relay) {
			Error(
				report,
				"export.option.delivery",
				{},
				"relay or redirect is package deployment metadata and applies only to Project ZIP"
			);
		}
		if (options.Product != ExportProduct::ProjectZip && options.Reproducible) {
			Error(
				report,
				"export.option.reproducible",
				{},
				"reproducible archive output applies only to Project ZIP"
			);
		}
		if (!report.Passed()) {
			return std::nullopt;
		}

		ExportRequest request;
		request.Product = options.Product;
		request.Destination = std::move(normalized);
		request.IncludeProcessedAssets =
			options.Product == ExportProduct::ProjectZip ? true : options.IncludeProcessedAssets;
		request.IncludeRawAuthoring = options.IncludeRawAuthoring;
		request.IncludePublicCdns =
			options.Product == ExportProduct::WorldFile ? false : options.IncludePublicCdns;
		request.ValidateCdnConfiguration = options.ValidateCdnConfiguration;
		request.CheckRemoteReachability = options.CheckRemoteReachability;
		request.RequireCompleteCatalogue =
			options.Product == ExportProduct::ProjectZip ? true : options.RequireCompleteCatalogue;
		request.Delivery = options.Delivery;
		request.Reproducible = options.Product == ExportProduct::ProjectZip ? true : false;
		request.ReplaceExisting = options.ReplaceExisting;
		return request;
	}

	ExportPreflight PreflightExport(
		const ExportRequest &request,
		const engine::world::Universe &universe,
		const ContentSources &content,
		const engine::delivery::AssetClient *client
	) {
		ExportPreflight preflight;
		preflight.Request = request;
		for (const engine::world::WorldId world : universe.Worlds()) {
			if (universe.IsRemote(world)) {
				preflight.RemoteWorlds++;
			} else {
				preflight.LocalWorlds++;
			}
		}

		std::error_code failure;
		auto checkDestination = [&](const std::filesystem::path &path) {
			if (std::filesystem::exists(path, failure) && !request.ReplaceExisting) {
				Error(
					preflight.Validation,
					"export.destination.exists",
					path.string(),
					"destination or export sidecar exists and replacement was not requested"
				);
			} else if (request.ReplaceExisting && std::filesystem::exists(path, failure) &&
					   std::filesystem::exists(path.string() + ".previous", failure)) {
				Error(
					preflight.Validation,
					"export.destination.backup",
					path.string() + ".previous",
					"recoverable replacement backup already exists"
				);
			}
			failure.clear();
		};
		checkDestination(request.Destination);
		if (request.Product == ExportProduct::UniverseFolder) {
			checkDestination(
				request.Destination.parent_path() / (request.Destination.stem().string() + ".worlds")
			);
		}
		if (request.Product != ExportProduct::ProjectZip &&
			(request.Product == ExportProduct::UniverseFolder || request.IncludeProcessedAssets ||
			 request.IncludeRawAuthoring)) {
			checkDestination(request.Destination.parent_path() / "assets");
		}

		const engine::assets::Manifest *catalogue = client != nullptr ? client->Catalogue() : nullptr;
		const bool catalogueReady = client != nullptr && client->Ready() && catalogue != nullptr &&
									client->CatalogueSignature() != nullptr;
		if (catalogue != nullptr) {
			preflight.ProcessedAssets = catalogue->Assets().size();
			for (const engine::assets::AssetEntry &asset : catalogue->Assets()) {
				if (!AddChecked(preflight.ProcessedBytes, asset.TotalBytes)) {
					Error(
						preflight.Validation,
						"export.size.overflow",
						asset.Name,
						"processed asset size overflowed"
					);
					break;
				}
			}
		}
		preflight.EstimatedUncompressedBytes = preflight.ProcessedBytes;
		preflight.EstimatedArchiveBytes =
			request.Product == ExportProduct::ProjectZip ? preflight.EstimatedUncompressedBytes : 0;
		const engine::delivery::DeliverySettings contentSettings = content.ToSettings();
		preflight.PublisherKeyValid = !contentSettings.Publisher.IsZero();
		preflight.PublicHttpIncluded = request.IncludePublicCdns;

		if ((request.IncludeProcessedAssets || request.RequireCompleteCatalogue) && client == nullptr) {
			Error(
				preflight.Validation,
				"export.catalogue.unavailable",
				{},
				"a verified catalogue must be ready before this export can begin"
			);
		} else if ((request.IncludeProcessedAssets || request.RequireCompleteCatalogue) && !catalogueReady) {
			preflight.Validation.Add(
				"export.catalogue.pending",
				ProjectFindingSeverity::Warning,
				"catalogue",
				{},
				"catalogue counts and trust will be proven before asset staging begins"
			);
		}
		if (request.IncludeRawAuthoring) {
			preflight.Validation.Add(
				"export.authoring.review",
				ProjectFindingSeverity::Warning,
				"authoring",
				{},
				"raw authoring files are unnecessary for hosting and known secret filenames are excluded"
			);
		}

		if (request.IncludePublicCdns) {
			for (const engine::delivery::Source &source : contentSettings.Sources) {
				if (source.Enabled && source.Readable() &&
					source.Kind == engine::delivery::SourceKind::Http) {
					preflight.EffectiveSources.push_back(source);
				}
			}
		}
		if (request.IncludeProcessedAssets) {
			preflight.EffectiveSources.insert(
				preflight.EffectiveSources.begin(),
				engine::delivery::Source{
					.Name = "exported-project",
					.Kind = engine::delivery::SourceKind::Directory,
					.Location = request.Product == ExportProduct::ProjectZip
									? "assets"
									: (request.Destination.parent_path() / "assets").string(),
					.Enabled = true,
					.Role = engine::delivery::SourceRole::Read,
				}
			);
		}

		if (request.IncludePublicCdns || request.IncludeProcessedAssets) {
			const bool publisherValid =
				preflight.PublisherKeyValid || (client != nullptr && !client->Settings().Publisher.IsZero());
			engine::delivery::ValidationOptions validation;
			validation.HttpDeclared = request.IncludePublicCdns;
			validation.HttpAllowed = request.IncludePublicCdns;
			validation.CheckReachability = request.CheckRemoteReachability;
			if (validation.CheckReachability && catalogueReady) {
				validation.Reachable.assign(preflight.EffectiveSources.size(), true);
			}
			validation.CataloguePresent = request.IncludeProcessedAssets ? client != nullptr : publisherValid;
			validation.CatalogueTrusted =
				request.IncludeProcessedAssets ? client != nullptr && publisherValid : publisherValid;
			validation.RequireComplete = request.RequireCompleteCatalogue;
			validation.CatalogueComplete = !request.IncludeProcessedAssets || client != nullptr;
			validation.ValidateDeployment = request.Product == ExportProduct::ProjectZip;
			validation.RelayRequested = request.Delivery == engine::game::ProjectDeliveryPreference::Relay;
			validation.RelayAvailable = true;
			validation.RedirectAvailable = request.IncludePublicCdns;
			Convert(
				preflight.Validation,
				engine::delivery::ValidateSources(preflight.EffectiveSources, validation)
			);
		}
		return preflight;
	}
}
