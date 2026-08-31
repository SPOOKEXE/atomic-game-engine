#include <cctype>
#include <studio/AssetGrounding.hpp>
#include <utility>

namespace studio {

	namespace {
		constexpr size_t RAW_ENTRIES_PER_PUMP = 8;

		void Fail(AssetGrounding &grounding, engine::delivery::AssetClient &client, std::string error) {
			for (const GroundedAssetRequest &request : grounding.Requests) {
				if (!request.Complete) {
					client.Cancel(request.Request);
				}
			}
			grounding.Error = std::move(error);
			grounding.State = AssetGroundingState::Failed;
		}

		std::string RawFolderName(const std::filesystem::path &source, size_t ordinal) {
			std::string name = source.filename().string();
			if (name.empty()) {
				name = "folder";
			}
			for (char &character : name) {
				if (!std::isalnum(static_cast<unsigned char>(character)) && character != '-' &&
					character != '_') {
					character = '_';
				}
			}
			return std::to_string(ordinal + 1) + "-" + name;
		}

		bool IsWithin(const std::filesystem::path &child, const std::filesystem::path &parent) {
			auto childPart = child.begin();
			for (auto parentPart = parent.begin(); parentPart != parent.end(); ++parentPart, ++childPart) {
				if (childPart == child.end() || *childPart != *parentPart) {
					return false;
				}
			}
			return true;
		}

		bool StartRawFolder(AssetGrounding &grounding, engine::delivery::AssetClient &client) {
			while (grounding.RawSource < grounding.RawSources.size()) {
				const size_t ordinal = grounding.RawSource++;
				std::error_code error;
				const std::filesystem::path source =
					std::filesystem::weakly_canonical(grounding.RawSources[ordinal], error);
				if (error || !std::filesystem::is_directory(source, error)) {
					Fail(
						grounding,
						client,
						"raw asset folder is not readable: " + grounding.RawSources[ordinal].string()
					);
					return false;
				}
				const std::filesystem::path destination =
					std::filesystem::weakly_canonical(grounding.Destination, error) / "raw" /
					RawFolderName(source, ordinal);
				if (error || IsWithin(destination, source)) {
					Fail(grounding, client, "raw asset export cannot be placed inside its source folder");
					return false;
				}
				std::filesystem::create_directories(destination, error);
				if (error) {
					Fail(grounding, client, "could not create the raw asset export folder");
					return false;
				}
				grounding.RawRoot = source;
				grounding.RawDestination = destination;
				grounding.RawIterator = std::filesystem::recursive_directory_iterator(
					source, std::filesystem::directory_options::skip_permission_denied, error
				);
				if (error) {
					Fail(grounding, client, "could not enumerate raw asset folder: " + source.string());
					return false;
				}
				if (grounding.RawIterator == std::filesystem::recursive_directory_iterator{}) {
					continue;
				}
				return true;
			}
			grounding.State = AssetGroundingState::Complete;
			return false;
		}

		void FinishProcessed(AssetGrounding &grounding, engine::delivery::AssetClient &client) {
			if (!grounding.Store->WriteManifest(*grounding.Catalogue, grounding.Signature)) {
				Fail(grounding, client, "could not write the grounded manifest");
				return;
			}
			if (grounding.RawSources.empty()) {
				grounding.State = AssetGroundingState::Complete;
				return;
			}
			grounding.State = AssetGroundingState::CopyingRaw;
			StartRawFolder(grounding, client);
		}
	}

	bool BeginAssetGrounding(
		AssetGrounding &grounding,
		const std::filesystem::path &destination,
		std::span<const std::filesystem::path> rawSources
	) {
		if (destination.empty() || grounding.State == AssetGroundingState::WaitingForCatalogue ||
			grounding.State == AssetGroundingState::Fetching ||
			grounding.State == AssetGroundingState::CopyingRaw) {
			return false;
		}

		grounding = AssetGrounding{};
		grounding.Destination = destination;
		grounding.RawSources.assign(rawSources.begin(), rawSources.end());
		grounding.State = AssetGroundingState::WaitingForCatalogue;
		return true;
	}

	void PumpAssetGrounding(AssetGrounding &grounding, engine::delivery::AssetClient &client) {
		if (grounding.State == AssetGroundingState::WaitingForCatalogue) {
			if (!client.Ready()) {
				return;
			}
			const engine::assets::Manifest *catalogue = client.Catalogue();
			const engine::assets::SignatureBytes *signature = client.CatalogueSignature();
			if (catalogue == nullptr || signature == nullptr) {
				Fail(grounding, client, "delivery reported ready without a signed catalogue");
				return;
			}

			grounding.Store = engine::assets::ChunkStore::Open(grounding.Destination, true);
			if (!grounding.Store) {
				Fail(grounding, client, "could not create the grounded asset store");
				return;
			}
			grounding.Catalogue = *catalogue;
			grounding.Signature = *signature;
			grounding.Requests.reserve(catalogue->Assets().size());
			for (const engine::assets::AssetEntry &entry : catalogue->Assets()) {
				const engine::delivery::RequestId request = client.Request(entry.Name);
				if (!request.IsValid()) {
					Fail(grounding, client, "could not request '" + entry.Name + "'");
					return;
				}
				grounding.Requests.push_back(
					GroundedAssetRequest{.Entry = entry, .Request = request, .Complete = false}
				);
			}
			if (grounding.Requests.empty()) {
				FinishProcessed(grounding, client);
				return;
			}
			grounding.State = AssetGroundingState::Fetching;
		}

		if (grounding.State == AssetGroundingState::CopyingRaw) {
			for (size_t copied = 0; copied < RAW_ENTRIES_PER_PUMP; copied++) {
				if (grounding.RawIterator == std::filesystem::recursive_directory_iterator{}) {
					if (!StartRawFolder(grounding, client)) {
						return;
					}
				}

				const std::filesystem::directory_entry entry = *grounding.RawIterator;
				std::error_code error;
				const std::filesystem::file_status status = entry.symlink_status(error);
				if (error) {
					Fail(grounding, client, "could not inspect raw asset: " + entry.path().string());
					return;
				}
				if (std::filesystem::is_symlink(status)) {
					grounding.RawIterator.increment(error);
					if (error) {
						Fail(grounding, client, "could not continue raw asset folder");
						return;
					}
					continue;
				}

				const std::filesystem::path relative = entry.path().lexically_relative(grounding.RawRoot);
				const std::filesystem::path destination = grounding.RawDestination / relative;
				if (std::filesystem::is_directory(status)) {
					std::filesystem::create_directories(destination, error);
				} else if (std::filesystem::is_regular_file(status)) {
					std::filesystem::create_directories(destination.parent_path(), error);
					if (!error) {
						std::filesystem::copy_file(
							entry.path(),
							destination,
							std::filesystem::copy_options::overwrite_existing,
							error
						);
					}
				}
				if (error) {
					Fail(grounding, client, "could not copy raw asset: " + entry.path().string());
					return;
				}
				grounding.RawIterator.increment(error);
				if (error) {
					Fail(grounding, client, "could not continue raw asset folder");
					return;
				}
			}
			return;
		}

		if (grounding.State != AssetGroundingState::Fetching) {
			return;
		}

		for (GroundedAssetRequest &pending : grounding.Requests) {
			if (pending.Complete) {
				continue;
			}
			const engine::delivery::RequestState state = client.StateOf(pending.Request);
			if (state == engine::delivery::RequestState::Pending) {
				continue;
			}
			if (state != engine::delivery::RequestState::Ready) {
				Fail(grounding, client, "no source supplied '" + pending.Entry.Name + "'");
				return;
			}

			std::optional<engine::delivery::Asset> asset = client.Take(pending.Request);
			if (!asset || asset->Root != pending.Entry.Root ||
				!grounding.Store->WriteAsset(pending.Entry, asset->Bytes)) {
				Fail(grounding, client, "delivered asset disagrees with '" + pending.Entry.Name + "'");
				return;
			}
			pending.Complete = true;
			grounding.Completed++;
		}

		if (grounding.Completed != grounding.Requests.size()) {
			return;
		}
		FinishProcessed(grounding, client);
	}

	void CancelAssetGrounding(AssetGrounding &grounding, engine::delivery::AssetClient &client) {
		for (const GroundedAssetRequest &request : grounding.Requests) {
			if (!request.Complete) {
				client.Cancel(request.Request);
			}
		}
		grounding = AssetGrounding{};
	}
}
