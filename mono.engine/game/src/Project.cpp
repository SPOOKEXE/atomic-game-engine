#include <engine/assets/ChunkStore.hpp>
#include <engine/assets/ContentHash.hpp>
#include <engine/assets/Signature.hpp>
#include <engine/core/Version.hpp>
#include <engine/game/Project.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <miniz.h>
#include <optional>
#include <random>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

namespace engine::game {
	namespace {
		namespace fs = std::filesystem;

		constexpr uint32_t PROJECT_PACKAGE_VERSION = 1;
		constexpr std::string_view PROJECT_DOCUMENT = "project.xml";
		constexpr std::string_view PROJECT_UNIVERSE = "game.auniverse";
		constexpr size_t MAXIMUM_PACKAGE_CDNS = 64;
		constexpr size_t MAXIMUM_PACKAGE_TEXT = 2048;
		constexpr size_t COPY_BUFFER_BYTES = 64u * 1024u;

		struct ArchiveEntry {
			mz_uint Index = 0;
			std::string Path;
			uint64_t CompressedBytes = 0;
			uint64_t UncompressedBytes = 0;
			bool Directory = false;
		};

		struct PayloadFile {
			fs::path Source;
			std::string Path;
			uint64_t Bytes = 0;
		};

		struct PayloadSummary {
			uint64_t Files = 0;
			uint64_t Bytes = 0;
			assets::ContentHash Digest;
		};

		struct SemanticVersion {
			uint32_t Major = 0;
			uint32_t Minor = 0;
			uint32_t Patch = 0;

			auto operator<=>(const SemanticVersion &) const = default;
		};

		struct ArchiveReader {
			std::ifstream File;
			mz_zip_archive Zip{};
			bool Open = false;

			~ArchiveReader() {
				if (Open) {
					mz_zip_reader_end(&Zip);
				}
			}
		};

		struct ArchiveWriter {
			std::fstream File;
			mz_zip_archive Zip{};
			bool Open = false;

			~ArchiveWriter() {
				if (Open) {
					mz_zip_writer_end(&Zip);
				}
			}
		};

		struct SourceFile {
			std::ifstream File;
		};

		std::span<const std::byte> BytesOf(std::string_view text) {
			return std::span(reinterpret_cast<const std::byte *>(text.data()), text.size());
		}

		void AddError(
			ProjectValidationReport &report,
			std::string code,
			std::string source,
			std::string path,
			std::string explanation
		) {
			report.Add(
				std::move(code),
				ProjectFindingSeverity::Error,
				std::move(source),
				std::move(path),
				std::move(explanation)
			);
		}

		std::string LowerAscii(std::string_view text) {
			std::string lowered;
			lowered.reserve(text.size());
			for (const unsigned char character : text) {
				lowered.push_back(static_cast<char>(std::tolower(character)));
			}
			return lowered;
		}

		bool ReservedWindowsName(std::string_view component) {
			const size_t dot = component.find('.');
			const std::string base = LowerAscii(component.substr(0, dot));
			if (base == "con" || base == "prn" || base == "aux" || base == "nul") {
				return true;
			}
			if (base.size() == 4 && (base.starts_with("com") || base.starts_with("lpt")) && base[3] >= '1' &&
				base[3] <= '9') {
				return true;
			}
			return false;
		}

		bool PortableArchivePath(
			std::string_view path, bool directory, const ProjectPackageLimits &limits, std::string &reason
		) {
			if (path.empty() || path.size() > limits.MaximumPathBytes) {
				reason = "entry path is empty or exceeds the path limit";
				return false;
			}
			if (path.front() == '/' || path.front() == '\\' || path.find('\\') != std::string_view::npos) {
				reason = "entry path is absolute or uses a platform separator";
				return false;
			}
			if (directory != path.ends_with('/')) {
				reason = "directory marker disagrees with entry metadata";
				return false;
			}

			uint32_t components = 0;
			size_t start = 0;
			while (start < path.size()) {
				const size_t slash = path.find('/', start);
				const size_t end = slash == std::string_view::npos ? path.size() : slash;
				const std::string_view component = path.substr(start, end - start);
				if (component.empty() || component == "." || component == "..") {
					reason = "entry path contains an empty, current, or parent component";
					return false;
				}
				if (component.back() == '.' || component.back() == ' ' || ReservedWindowsName(component)) {
					reason = "entry path is not portable across supported filesystems";
					return false;
				}
				for (const unsigned char character : component) {
					if (character < 0x20 || character >= 0x7f || character == ':' || character == '<' ||
						character == '>' || character == '"' || character == '|' || character == '?' ||
						character == '*') {
						reason = "entry path contains a non-portable character";
						return false;
					}
				}
				components++;
				if (components > limits.MaximumNesting) {
					reason = "entry path exceeds the nesting limit";
					return false;
				}
				if (slash == std::string_view::npos || slash + 1 == path.size()) {
					break;
				}
				start = slash + 1;
			}
			return true;
		}

		bool SensitiveAuthoringPath(std::string_view path) {
			if (!path.starts_with("authoring/")) {
				return false;
			}
			const size_t slash = path.find_last_of('/');
			const std::string leaf = LowerAscii(path.substr(slash == std::string_view::npos ? 0 : slash + 1));
			const size_t dot = leaf.find_last_of('.');
			const std::string_view extension =
				dot == std::string::npos ? std::string_view{} : std::string_view(leaf).substr(dot);
			return leaf == ".env" || leaf == "id_rsa" || leaf == "id_ed25519" || leaf == "credentials" ||
				   leaf == "credentials.json" || leaf == "secrets.toml" || extension == ".pem" ||
				   extension == ".key" || extension == ".p12" || extension == ".pfx";
		}

		bool AllowedStagingPath(std::string_view path) {
			if (path == PROJECT_UNIVERSE) {
				return true;
			}
			return path.starts_with("game.worlds/") || path.starts_with("assets/") ||
				   path.starts_with("authoring/");
		}

		bool AddChecked(uint64_t &total, uint64_t amount) {
			if (amount > std::numeric_limits<uint64_t>::max() - total) {
				return false;
			}
			total += amount;
			return true;
		}

		void HashUnsigned(assets::Hasher &hasher, uint64_t value) {
			std::array<std::byte, sizeof(value)> bytes{};
			for (size_t index = 0; index < bytes.size(); index++) {
				bytes[index] = static_cast<std::byte>((value >> (index * 8u)) & 0xffu);
			}
			hasher.Update(bytes);
		}

		bool HashFile(assets::Hasher &hasher, const PayloadFile &file, ProjectValidationReport &report) {
			HashUnsigned(hasher, file.Path.size());
			hasher.Update(BytesOf(file.Path));
			HashUnsigned(hasher, file.Bytes);

			std::ifstream input(file.Source, std::ios::binary);
			if (!input) {
				AddError(report, "package.file.read", "package", file.Path, "could not read staging file");
				return false;
			}
			std::array<char, COPY_BUFFER_BYTES> buffer{};
			uint64_t readBytes = 0;
			while (input) {
				input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
				const std::streamsize taken = input.gcount();
				if (taken > 0) {
					hasher.Update(
						std::span(
							reinterpret_cast<const std::byte *>(buffer.data()), static_cast<size_t>(taken)
						)
					);
					readBytes += static_cast<uint64_t>(taken);
				}
			}
			if (!input.eof() || readBytes != file.Bytes) {
				AddError(
					report, "package.file.changed", "package", file.Path, "staging file changed while read"
				);
				return false;
			}
			return true;
		}

		std::optional<PayloadSummary>
		SummarizePayload(const std::vector<PayloadFile> &files, ProjectValidationReport &report) {
			PayloadSummary summary;
			assets::Hasher hasher;
			for (const PayloadFile &file : files) {
				if (!AddChecked(summary.Bytes, file.Bytes) || !HashFile(hasher, file, report)) {
					return std::nullopt;
				}
				summary.Files++;
			}
			summary.Digest = hasher.Finish();
			return summary;
		}

		std::optional<uint64_t> ParseUnsigned(std::string_view text) {
			uint64_t value = 0;
			const auto [end, status] = std::from_chars(text.data(), text.data() + text.size(), value);
			if (status != std::errc{} || end != text.data() + text.size()) {
				return std::nullopt;
			}
			return value;
		}

		std::optional<SemanticVersion> ParseSemanticVersion(std::string_view text) {
			SemanticVersion version;
			std::array<uint32_t *, 3> parts{&version.Major, &version.Minor, &version.Patch};
			size_t start = 0;
			for (size_t index = 0; index < parts.size(); index++) {
				const size_t dot = text.find('.', start);
				const size_t end = dot == std::string_view::npos ? text.size() : dot;
				uint32_t value = 0;
				const auto [parsed, status] = std::from_chars(text.data() + start, text.data() + end, value);
				if (status != std::errc{} || parsed != text.data() + end || start == end) {
					return std::nullopt;
				}
				*parts[index] = value;
				if (index + 1 < parts.size()) {
					if (dot == std::string_view::npos) {
						return std::nullopt;
					}
					start = dot + 1;
				} else if (dot != std::string_view::npos) {
					return std::nullopt;
				}
			}
			return version;
		}

		bool PublicText(std::string_view text, size_t maximum = MAXIMUM_PACKAGE_TEXT) {
			if (text.empty() || text.size() > maximum) {
				return false;
			}
			return std::all_of(text.begin(), text.end(), [](unsigned char character) {
				return character >= 0x20 && character < 0x7f;
			});
		}

		std::string WriteProjectDocument(const ProjectPackageInfo &info) {
			XmlWriter writer;
			writer.Open("ProjectPackage");
			writer.Attribute("format", std::to_string(info.FormatVersion));
			writer.Attribute("universe", info.UniverseEntrypoint.generic_string());
			writer.Attribute("minEngine", info.MinimumEngine);
			writer.Attribute("maxEngine", info.MaximumEngine);
			writer.Attribute("profile", info.CreationProfile);
			writer.Attribute("publisher", info.PublisherKey);
			writer.Attribute("delivery", Describe(info.Delivery));
			writer.Attribute("payloadFiles", std::to_string(info.FileCount));
			writer.Attribute("payloadBytes", std::to_string(info.UncompressedBytes));
			writer.Attribute("payloadDigest", info.ContentDigest);
			for (const UniverseCdn &cdn : info.Cdns) {
				writer.Open("Cdn");
				writer.Attribute("name", cdn.Name);
				writer.Attribute("location", cdn.Location);
				writer.Close();
			}
			writer.Close();
			return writer.Finish();
		}

		std::optional<ProjectPackageInfo>
		ReadProjectDocument(std::string_view text, ProjectValidationReport &report) {
			XmlDocument document;
			XmlLimits limits;
			limits.MaximumBytes = 64u * 1024u;
			limits.MaximumDepth = 3;
			limits.MaximumElements = static_cast<uint32_t>(MAXIMUM_PACKAGE_CDNS + 1);
			limits.MaximumAttributes = 16;
			const XmlStatus status = ParseXml(text, document, limits);
			const XmlElement *root = document.Root();
			if (status != XmlStatus::Ok || root == nullptr || root->Name != "ProjectPackage") {
				AddError(
					report,
					"package.project.invalid",
					"package",
					std::string(PROJECT_DOCUMENT),
					"project.xml is not a supported project package document"
				);
				return std::nullopt;
			}

			static const std::set<std::string_view> allowedAttributes{
				"format",
				"universe",
				"minEngine",
				"maxEngine",
				"profile",
				"publisher",
				"delivery",
				"payloadFiles",
				"payloadBytes",
				"payloadDigest"
			};
			for (const std::string &attribute : root->AttributeNames) {
				if (!allowedAttributes.contains(attribute)) {
					AddError(
						report,
						"package.project.field",
						"package",
						std::string(PROJECT_DOCUMENT),
						"project.xml contains an unsupported field"
					);
					return std::nullopt;
				}
			}

			ProjectPackageInfo info;
			const auto format = ParseUnsigned(root->Attribute("format"));
			const auto files = ParseUnsigned(root->Attribute("payloadFiles"));
			const auto bytes = ParseUnsigned(root->Attribute("payloadBytes"));
			const auto delivery = ProjectDeliveryPreferenceOf(root->Attribute("delivery"));
			const auto digest = assets::ContentHash::FromHex(root->Attribute("payloadDigest"));
			if (!format || *format != PROJECT_PACKAGE_VERSION || !files || !bytes || !delivery || !digest) {
				AddError(
					report,
					"package.project.contract",
					"package",
					std::string(PROJECT_DOCUMENT),
					"project.xml has an unsupported version or malformed package metadata"
				);
				return std::nullopt;
			}
			info.FormatVersion = static_cast<uint32_t>(*format);
			info.UniverseEntrypoint = fs::path(root->Attribute("universe"));
			info.MinimumEngine = root->Attribute("minEngine");
			info.MaximumEngine = root->Attribute("maxEngine");
			info.CreationProfile = root->Attribute("profile");
			info.PublisherKey = root->Attribute("publisher");
			info.Delivery = *delivery;
			info.FileCount = *files;
			info.UncompressedBytes = *bytes;
			info.ContentDigest = digest->ToHex();

			std::string pathReason;
			ProjectPackageLimits pathLimits;
			if (!PortableArchivePath(
					info.UniverseEntrypoint.generic_string(), false, pathLimits, pathReason
				) ||
				info.UniverseEntrypoint.extension() != UNIVERSE_EXTENSION ||
				!PublicText(info.MinimumEngine, 32) || !PublicText(info.MaximumEngine, 32) ||
				!PublicText(info.CreationProfile, 64) || !assets::PublicKey::FromHex(info.PublisherKey)) {
				AddError(
					report,
					"package.project.value",
					"package",
					std::string(PROJECT_DOCUMENT),
					"project.xml contains an invalid entrypoint, version, profile, or publisher key"
				);
				return std::nullopt;
			}

			const auto minimum = ParseSemanticVersion(info.MinimumEngine);
			const auto maximum = ParseSemanticVersion(info.MaximumEngine);
			const auto current = ParseSemanticVersion(core::Version());
			if (!minimum || !maximum || !current || *minimum > *maximum || *current < *minimum ||
				*current > *maximum) {
				AddError(
					report,
					"package.engine.incompatible",
					"package",
					std::string(PROJECT_DOCUMENT),
					"this engine version is outside the package compatibility range"
				);
				return std::nullopt;
			}

			std::set<std::string> sourceNames;
			for (const uint32_t childIndex : root->Children) {
				const XmlElement *child = document.At(childIndex);
				if (child == nullptr || child->Name != "Cdn" || !child->Children.empty() ||
					child->AttributeNames.size() != 2 || !child->HasAttribute("name") ||
					!child->HasAttribute("location")) {
					AddError(
						report,
						"package.cdn.contract",
						"package",
						std::string(PROJECT_DOCUMENT),
						"project.xml contains an unsupported content source"
					);
					return std::nullopt;
				}
				UniverseCdn cdn{
					std::string(child->Attribute("name")), std::string(child->Attribute("location"))
				};
				if (!PublicText(cdn.Name, 128) || !PublicText(cdn.Location) ||
					!sourceNames.insert(LowerAscii(cdn.Name)).second) {
					AddError(
						report,
						"package.cdn.invalid",
						cdn.Name,
						std::string(PROJECT_DOCUMENT),
						"content source names must be unique and public source values must be bounded"
					);
					return std::nullopt;
				}
				info.Cdns.push_back(std::move(cdn));
			}
			return info;
		}

		size_t ArchiveRead(void *opaque, mz_uint64 offset, void *destination, size_t bytes) {
			auto &file = *static_cast<std::ifstream *>(opaque);
			file.clear();
			file.seekg(static_cast<std::streamoff>(offset));
			if (!file) {
				return 0;
			}
			file.read(static_cast<char *>(destination), static_cast<std::streamsize>(bytes));
			return static_cast<size_t>(file.gcount());
		}

		size_t ArchiveWrite(void *opaque, mz_uint64 offset, const void *source, size_t bytes) {
			auto &file = *static_cast<std::fstream *>(opaque);
			file.clear();
			file.seekp(static_cast<std::streamoff>(offset));
			if (!file) {
				return 0;
			}
			file.write(static_cast<const char *>(source), static_cast<std::streamsize>(bytes));
			return file ? bytes : 0;
		}

		size_t SourceRead(void *opaque, mz_uint64 offset, void *destination, size_t bytes) {
			auto &file = static_cast<SourceFile *>(opaque)->File;
			file.clear();
			file.seekg(static_cast<std::streamoff>(offset));
			if (!file) {
				return 0;
			}
			file.read(static_cast<char *>(destination), static_cast<std::streamsize>(bytes));
			return static_cast<size_t>(file.gcount());
		}

		std::string ZipError(const mz_zip_archive &zip) {
			return mz_zip_get_error_string(mz_zip_get_last_error(const_cast<mz_zip_archive *>(&zip)));
		}

		bool ExactArchiveBoundary(std::ifstream &file, uint64_t bytes) {
			if (bytes < 22) {
				return false;
			}
			std::array<unsigned char, 22> end{};
			file.clear();
			file.seekg(static_cast<std::streamoff>(bytes - end.size()));
			file.read(reinterpret_cast<char *>(end.data()), static_cast<std::streamsize>(end.size()));
			if (!file) {
				return false;
			}
			const uint32_t signature = static_cast<uint32_t>(end[0]) | (static_cast<uint32_t>(end[1]) << 8u) |
									   (static_cast<uint32_t>(end[2]) << 16u) |
									   (static_cast<uint32_t>(end[3]) << 24u);
			const uint16_t commentBytes =
				static_cast<uint16_t>(end[20]) | (static_cast<uint16_t>(end[21]) << 8u);
			return signature == 0x06054b50u && commentBytes == 0;
		}

		bool OpenArchiveReader(ArchiveReader &reader, const fs::path &path, ProjectValidationReport &report) {
			std::error_code failure;
			const uint64_t fileBytes = fs::file_size(path, failure);
			if (failure || fileBytes < 22) {
				AddError(report, "archive.open", "archive", {}, "project ZIP is missing or too small");
				return false;
			}
			reader.File.open(path, std::ios::binary);
			if (!reader.File || !ExactArchiveBoundary(reader.File, fileBytes)) {
				AddError(
					report,
					"archive.trailing",
					"archive",
					{},
					"project ZIP has a preamble, archive comment, trailing data, or no exact end record"
				);
				return false;
			}
			std::array<unsigned char, 4> first{};
			reader.File.clear();
			reader.File.seekg(0);
			reader.File.read(
				reinterpret_cast<char *>(first.data()), static_cast<std::streamsize>(first.size())
			);
			const uint32_t firstSignature =
				static_cast<uint32_t>(first[0]) | (static_cast<uint32_t>(first[1]) << 8u) |
				(static_cast<uint32_t>(first[2]) << 16u) | (static_cast<uint32_t>(first[3]) << 24u);
			if (!reader.File || firstSignature != 0x04034b50u) {
				AddError(
					report, "archive.preamble", "archive", {}, "project ZIP has data before its first entry"
				);
				return false;
			}

			reader.Zip.m_pRead = ArchiveRead;
			reader.Zip.m_pIO_opaque = &reader.File;
			if (!mz_zip_reader_init(&reader.Zip, fileBytes, 0)) {
				AddError(
					report, "archive.parse", "archive", {}, "could not parse project ZIP central directory"
				);
				return false;
			}
			reader.Open = true;
			return true;
		}

		bool UnixSpecialEntry(const mz_zip_archive_file_stat &stat) {
			const uint8_t creator = static_cast<uint8_t>(stat.m_version_made_by >> 8u);
			if (creator == 3) {
				const uint32_t kind = (stat.m_external_attr >> 16u) & 0170000u;
				return kind != 0 && kind != 0040000u && kind != 0100000u;
			}
			return (stat.m_external_attr & 0x40u) != 0;
		}

		std::optional<std::vector<ArchiveEntry>> InspectArchive(
			ArchiveReader &reader, const ProjectPackageLimits &limits, ProjectValidationReport &report
		) {
			if (reader.Zip.m_total_files == 0 || reader.Zip.m_total_files > limits.MaximumEntries) {
				AddError(
					report, "archive.entry.count", "archive", {}, "project ZIP entry count is out of bounds"
				);
				return std::nullopt;
			}

			std::set<std::string> exactPaths;
			std::set<std::string> foldedPaths;
			std::vector<ArchiveEntry> entries;
			entries.reserve(reader.Zip.m_total_files);
			uint64_t totalBytes = 0;
			for (mz_uint index = 0; index < reader.Zip.m_total_files; index++) {
				const mz_uint filenameBytes = mz_zip_reader_get_filename(&reader.Zip, index, nullptr, 0);
				if (filenameBytes <= 1 || filenameBytes - 1 > limits.MaximumPathBytes) {
					AddError(
						report,
						"archive.path.length",
						"archive",
						{},
						"project ZIP entry path is out of bounds"
					);
					return std::nullopt;
				}
				std::vector<char> filename(filenameBytes);
				if (mz_zip_reader_get_filename(&reader.Zip, index, filename.data(), filenameBytes) !=
					filenameBytes) {
					AddError(
						report, "archive.path.read", "archive", {}, "could not read project ZIP entry path"
					);
					return std::nullopt;
				}
				mz_zip_archive_file_stat stat{};
				if (!mz_zip_reader_file_stat(&reader.Zip, index, &stat)) {
					AddError(
						report, "archive.entry.read", "archive", {}, "could not inspect project ZIP entry"
					);
					return std::nullopt;
				}
				ArchiveEntry entry;
				entry.Index = index;
				entry.Path.assign(filename.data(), filenameBytes - 1);
				entry.CompressedBytes = stat.m_comp_size;
				entry.UncompressedBytes = stat.m_uncomp_size;
				entry.Directory = stat.m_is_directory != 0;

				std::string pathReason;
				if (!PortableArchivePath(entry.Path, entry.Directory, limits, pathReason)) {
					AddError(report, "archive.path.invalid", "archive", entry.Path, std::move(pathReason));
					return std::nullopt;
				}
				if (!exactPaths.insert(entry.Path).second) {
					AddError(report, "archive.path.duplicate", "archive", entry.Path, "duplicate entry path");
					return std::nullopt;
				}
				if (!foldedPaths.insert(LowerAscii(entry.Path)).second) {
					AddError(
						report,
						"archive.path.case-collision",
						"archive",
						entry.Path,
						"entry path collides on a case-insensitive filesystem"
					);
					return std::nullopt;
				}
				if (stat.m_is_encrypted || !stat.m_is_supported ||
					(stat.m_method != 0 && stat.m_method != 8)) {
					AddError(
						report,
						stat.m_is_encrypted ? "archive.entry.encrypted" : "archive.entry.unsupported",
						"archive",
						entry.Path,
						"encrypted, patched, or unsupported compression entries are refused"
					);
					return std::nullopt;
				}
				if (UnixSpecialEntry(stat)) {
					AddError(
						report,
						"archive.entry.special",
						"archive",
						entry.Path,
						"links, devices, sockets, and other special entries are refused"
					);
					return std::nullopt;
				}
				if (entry.UncompressedBytes > limits.MaximumFileBytes ||
					!AddChecked(totalBytes, entry.UncompressedBytes) ||
					totalBytes > limits.MaximumTotalBytes) {
					AddError(
						report,
						"archive.entry.size",
						"archive",
						entry.Path,
						"entry or archive size exceeds its limit"
					);
					return std::nullopt;
				}
				if (entry.UncompressedBytes > 0 &&
					(entry.CompressedBytes == 0 ||
					 entry.UncompressedBytes / entry.CompressedBytes > limits.MaximumCompressionRatio)) {
					AddError(
						report,
						"archive.entry.ratio",
						"archive",
						entry.Path,
						"entry compression ratio exceeds its limit"
					);
					return std::nullopt;
				}
				entries.push_back(std::move(entry));
			}
			return entries;
		}

		const ArchiveEntry *FindEntry(const std::vector<ArchiveEntry> &entries, std::string_view path) {
			const auto found =
				std::find_if(entries.begin(), entries.end(), [path](const ArchiveEntry &entry) {
					return entry.Path == path;
				});
			return found == entries.end() ? nullptr : &*found;
		}

		std::optional<std::vector<std::byte>> ExtractMemory(
			ArchiveReader &reader,
			const ArchiveEntry &entry,
			size_t maximumBytes,
			ProjectValidationReport &report
		) {
			if (entry.Directory || entry.UncompressedBytes > maximumBytes ||
				entry.UncompressedBytes > std::numeric_limits<size_t>::max()) {
				AddError(
					report,
					"archive.document.size",
					"archive",
					entry.Path,
					"project document exceeds its limit"
				);
				return std::nullopt;
			}
			std::vector<std::byte> bytes(static_cast<size_t>(entry.UncompressedBytes));
			if (!mz_zip_reader_extract_to_mem(&reader.Zip, entry.Index, bytes.data(), bytes.size(), 0)) {
				AddError(
					report,
					"archive.document.extract",
					"archive",
					entry.Path,
					"could not verify project document"
				);
				return std::nullopt;
			}
			return bytes;
		}

		fs::path UniquePath(const fs::path &directory, std::string_view prefix, std::string_view suffix) {
			std::random_device random;
			for (size_t attempt = 0; attempt < 32; attempt++) {
				const uint64_t nonce =
					(static_cast<uint64_t>(random()) << 32u) ^ random() ^
					static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
				const fs::path candidate =
					directory / (std::string(prefix) + std::to_string(nonce) + std::string(suffix));
				std::error_code failure;
				if (!fs::exists(candidate, failure)) {
					return candidate;
				}
			}
			return {};
		}

		std::optional<fs::path> CreateExtractionRoot(ProjectValidationReport &report) {
			std::error_code failure;
			const fs::path temporary = fs::temp_directory_path(failure);
			if (failure) {
				AddError(report, "archive.extract.root", "archive", {}, "temporary directory is unavailable");
				return std::nullopt;
			}
			for (size_t attempt = 0; attempt < 32; attempt++) {
				const fs::path candidate = UniquePath(temporary, "atomic-project-", {});
				if (!candidate.empty() && fs::create_directory(candidate, failure)) {
					fs::permissions(candidate, fs::perms::owner_all, fs::perm_options::replace, failure);
					if (!failure) {
						return candidate;
					}
					std::error_code ignored;
					fs::remove_all(candidate, ignored);
				}
				failure.clear();
			}
			AddError(
				report,
				"archive.extract.root",
				"archive",
				{},
				"could not create a private extraction directory"
			);
			return std::nullopt;
		}

		bool ExtractFile(
			ArchiveReader &reader,
			const ArchiveEntry &entry,
			const fs::path &root,
			assets::Hasher *payloadHasher,
			ProjectValidationReport &report
		) {
			const fs::path destination = root / fs::path(entry.Path);
			std::error_code failure;
			if (entry.Directory) {
				fs::create_directories(destination, failure);
				if (failure) {
					AddError(
						report,
						"archive.extract.directory",
						"archive",
						entry.Path,
						"could not create entry directory"
					);
					return false;
				}
				return true;
			}

			fs::create_directories(destination.parent_path(), failure);
			if (failure || fs::is_symlink(fs::symlink_status(destination.parent_path(), failure))) {
				AddError(
					report,
					"archive.extract.parent",
					"archive",
					entry.Path,
					"could not create a safe entry parent"
				);
				return false;
			}
			std::ofstream output(destination, std::ios::binary | std::ios::trunc);
			if (!output) {
				AddError(
					report, "archive.extract.open", "archive", entry.Path, "could not create extracted entry"
				);
				return false;
			}

			if (payloadHasher != nullptr) {
				HashUnsigned(*payloadHasher, entry.Path.size());
				payloadHasher->Update(BytesOf(entry.Path));
				HashUnsigned(*payloadHasher, entry.UncompressedBytes);
			}
			mz_zip_reader_extract_iter_state *iterator =
				mz_zip_reader_extract_iter_new(&reader.Zip, entry.Index, 0);
			if (iterator == nullptr) {
				AddError(
					report, "archive.extract.begin", "archive", entry.Path, "could not begin entry extraction"
				);
				return false;
			}
			std::array<std::byte, COPY_BUFFER_BYTES> buffer{};
			uint64_t writtenBytes = 0;
			while (writtenBytes < entry.UncompressedBytes) {
				const size_t requested = static_cast<size_t>(
					std::min<uint64_t>(buffer.size(), entry.UncompressedBytes - writtenBytes)
				);
				const size_t taken = mz_zip_reader_extract_iter_read(iterator, buffer.data(), requested);
				if (taken == 0) {
					break;
				}
				output.write(
					reinterpret_cast<const char *>(buffer.data()), static_cast<std::streamsize>(taken)
				);
				if (!output) {
					break;
				}
				if (payloadHasher != nullptr) {
					payloadHasher->Update(std::span(buffer.data(), taken));
				}
				writtenBytes += taken;
			}
			const bool verified = mz_zip_reader_extract_iter_free(iterator) != 0;
			output.close();
			if (!verified || writtenBytes != entry.UncompressedBytes || !output) {
				fs::remove(destination, failure);
				AddError(
					report,
					"archive.extract.verify",
					"archive",
					entry.Path,
					"entry failed length or CRC verification"
				);
				return false;
			}
			return true;
		}

		std::optional<std::vector<PayloadFile>>
		CollectStagingFiles(const fs::path &root, ProjectValidationReport &report) {
			std::error_code failure;
			if (!fs::is_directory(root, failure)) {
				AddError(
					report,
					"package.staging.missing",
					"package",
					root.string(),
					"staging root is not a directory"
				);
				return std::nullopt;
			}
			std::vector<PayloadFile> files;
			fs::recursive_directory_iterator iterator(root, fs::directory_options::none, failure);
			const fs::recursive_directory_iterator end;
			while (!failure && iterator != end) {
				const fs::directory_entry &entry = *iterator;
				const fs::file_status status = entry.symlink_status(failure);
				if (failure) {
					break;
				}
				const std::string relative = entry.path().lexically_relative(root).generic_string();
				if (fs::is_symlink(status) || (!fs::is_directory(status) && !fs::is_regular_file(status))) {
					AddError(
						report,
						"package.staging.special",
						"package",
						relative,
						"staging links and special files are refused"
					);
					return std::nullopt;
				}
				if (fs::is_regular_file(status)) {
					std::string reason;
					if (!PortableArchivePath(relative, false, ProjectPackageLimits{}, reason) ||
						!AllowedStagingPath(relative)) {
						AddError(
							report,
							"package.staging.path",
							"package",
							relative,
							"staging path is not part of the package contract"
						);
						return std::nullopt;
					}
					if (SensitiveAuthoringPath(relative)) {
						AddError(
							report,
							"package.authoring.secret",
							"package",
							relative,
							"sensitive authoring file is excluded from packages"
						);
						return std::nullopt;
					}
					const uint64_t bytes = entry.file_size(failure);
					if (failure) {
						break;
					}
					files.push_back(PayloadFile{entry.path(), relative, bytes});
				}
				iterator.increment(failure);
			}
			if (failure) {
				AddError(
					report,
					"package.staging.walk",
					"package",
					root.string(),
					"could not enumerate staging tree"
				);
				return std::nullopt;
			}
			std::sort(files.begin(), files.end(), [](const PayloadFile &left, const PayloadFile &right) {
				return left.Path < right.Path;
			});
			if (std::none_of(files.begin(), files.end(), [](const PayloadFile &file) {
					return file.Path == PROJECT_UNIVERSE;
				})) {
				AddError(
					report,
					"package.universe.missing",
					"package",
					std::string(PROJECT_UNIVERSE),
					"staging universe entrypoint is missing"
				);
				return std::nullopt;
			}
			return files;
		}

		bool OpenArchiveWriter(ArchiveWriter &writer, const fs::path &path, ProjectValidationReport &report) {
			writer.File.open(path, std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
			if (!writer.File) {
				AddError(
					report,
					"package.write.open",
					"package",
					path.string(),
					"could not create temporary archive"
				);
				return false;
			}
			writer.Zip.m_pWrite = ArchiveWrite;
			writer.Zip.m_pIO_opaque = &writer.File;
			if (!mz_zip_writer_init(&writer.Zip, 0)) {
				AddError(
					report, "package.write.begin", "package", path.string(), "could not begin ZIP writer"
				);
				return false;
			}
			writer.Open = true;
			return true;
		}

		bool AddMemoryEntry(
			ArchiveWriter &writer,
			std::string_view path,
			std::string_view content,
			ProjectValidationReport &report
		) {
			if (!mz_zip_writer_add_mem_ex_v2(
					&writer.Zip,
					std::string(path).c_str(),
					content.data(),
					content.size(),
					nullptr,
					0,
					MZ_NO_COMPRESSION,
					0,
					0,
					nullptr,
					nullptr,
					0,
					nullptr,
					0
				)) {
				AddError(report, "package.write.entry", "package", std::string(path), ZipError(writer.Zip));
				return false;
			}
			return true;
		}

		bool AddFileEntry(ArchiveWriter &writer, const PayloadFile &file, ProjectValidationReport &report) {
			SourceFile source;
			source.File.open(file.Source, std::ios::binary);
			if (!source.File || !mz_zip_writer_add_read_buf_callback(
									&writer.Zip,
									file.Path.c_str(),
									SourceRead,
									&source,
									file.Bytes,
									nullptr,
									nullptr,
									0,
									MZ_NO_COMPRESSION,
									nullptr,
									0,
									nullptr,
									0
								)) {
				AddError(report, "package.write.entry", "package", file.Path, ZipError(writer.Zip));
				return false;
			}
			return true;
		}

		bool ValidPackageOptions(const ProjectPackageOptions &options, ProjectValidationReport &report) {
			if (!assets::PublicKey::FromHex(options.PublisherKey)) {
				AddError(
					report,
					"package.publisher.invalid",
					"package",
					{},
					"publisher key must be 64 lowercase hex characters"
				);
			}
			const std::string minimum(
				options.MinimumEngine.empty() ? core::Version() : std::string_view(options.MinimumEngine)
			);
			const std::string maximum(
				options.MaximumEngine.empty() ? core::Version() : std::string_view(options.MaximumEngine)
			);
			const auto minimumVersion = ParseSemanticVersion(minimum);
			const auto maximumVersion = ParseSemanticVersion(maximum);
			if (!minimumVersion || !maximumVersion || *minimumVersion > *maximumVersion) {
				AddError(
					report, "package.engine.range", "package", {}, "engine compatibility range is invalid"
				);
			}
			if (!PublicText(options.CreationProfile, 64)) {
				AddError(
					report,
					"package.profile.invalid",
					"package",
					{},
					"creation profile is empty or not portable public text"
				);
			}
			if (options.Cdns.size() > MAXIMUM_PACKAGE_CDNS) {
				AddError(report, "package.cdn.count", "package", {}, "too many public content sources");
			}
			std::set<std::string> names;
			for (const UniverseCdn &cdn : options.Cdns) {
				if (!PublicText(cdn.Name, 128) || !PublicText(cdn.Location) ||
					!names.insert(LowerAscii(cdn.Name)).second) {
					AddError(
						report,
						"package.cdn.invalid",
						cdn.Name,
						{},
						"public content source is malformed or duplicated"
					);
				}
			}
			return report.Passed();
		}
	}

	const char *ExtensionOf(ExportProduct product) {
		switch (product) {
		case ExportProduct::WorldFile:
			return WORLD_EXTENSION.data();
		case ExportProduct::UniverseFolder:
			return UNIVERSE_EXTENSION.data();
		case ExportProduct::ProjectZip:
			return ".zip";
		}
		return "";
	}

	ProjectKind ClassifyProject(const fs::path &path) {
		if (path.empty()) {
			return ProjectKind::Unknown;
		}
		const fs::path extension = path.extension();
		if (extension == GAME_EXTENSION) {
			return ProjectKind::GameFile;
		}
		if (extension == UNIVERSE_EXTENSION) {
			return ProjectKind::UniverseFolder;
		}
		if (extension == ".zip") {
			return ProjectKind::ProjectZip;
		}
		return ProjectKind::SceneScript;
	}

	const char *Describe(ProjectKind kind) {
		switch (kind) {
		case ProjectKind::Unknown:
			return "unknown";
		case ProjectKind::SceneScript:
			return "scene script";
		case ProjectKind::GameFile:
			return "game file";
		case ProjectKind::UniverseFolder:
			return "universe folder";
		case ProjectKind::ProjectZip:
			return "project ZIP";
		}
		return "unknown";
	}

	const char *Describe(ProjectDeliveryPreference preference) {
		return preference == ProjectDeliveryPreference::Redirect ? "redirect" : "relay";
	}

	std::optional<ProjectDeliveryPreference> ProjectDeliveryPreferenceOf(std::string_view text) {
		if (text == "relay") {
			return ProjectDeliveryPreference::Relay;
		}
		if (text == "redirect") {
			return ProjectDeliveryPreference::Redirect;
		}
		return std::nullopt;
	}

	bool ProjectValidationReport::Passed() const {
		return std::none_of(Findings.begin(), Findings.end(), [](const ProjectValidationFinding &finding) {
			return finding.Severity == ProjectFindingSeverity::Error;
		});
	}

	void ProjectValidationReport::Add(
		std::string code,
		ProjectFindingSeverity severity,
		std::string source,
		std::string path,
		std::string explanation
	) {
		Findings.push_back(
			ProjectValidationFinding{
				std::move(code), severity, std::move(source), std::move(path), std::move(explanation)
			}
		);
	}

	void ProjectValidationReport::Append(ProjectValidationReport other) {
		Findings.insert(
			Findings.end(),
			std::make_move_iterator(other.Findings.begin()),
			std::make_move_iterator(other.Findings.end())
		);
	}

	OpenedProject::~OpenedProject() {
		Cleanup();
	}

	OpenedProject::OpenedProject(OpenedProject &&other) noexcept
		: RootPath(std::move(other.RootPath)), EntrypointPath(std::move(other.EntrypointPath)),
		  AssetsPath(std::move(other.AssetsPath)), PackageMetadata(std::move(other.PackageMetadata)),
		  OwnsRoot(std::exchange(other.OwnsRoot, false)) {}

	OpenedProject &OpenedProject::operator=(OpenedProject &&other) noexcept {
		if (this == &other) {
			return *this;
		}
		Cleanup();
		RootPath = std::move(other.RootPath);
		EntrypointPath = std::move(other.EntrypointPath);
		AssetsPath = std::move(other.AssetsPath);
		PackageMetadata = std::move(other.PackageMetadata);
		OwnsRoot = std::exchange(other.OwnsRoot, false);
		return *this;
	}

	const fs::path &OpenedProject::Entrypoint() const {
		return EntrypointPath;
	}

	const fs::path &OpenedProject::Assets() const {
		return AssetsPath;
	}

	const ProjectPackageInfo &OpenedProject::Package() const {
		return PackageMetadata;
	}

	bool OpenedProject::Temporary() const {
		return OwnsRoot;
	}

	void OpenedProject::Cleanup() {
		if (!OwnsRoot || RootPath.empty()) {
			return;
		}
		std::error_code ignored;
		fs::remove_all(RootPath, ignored);
		OwnsRoot = false;
	}

	ProjectValidationReport
	ValidateProcessedAssetStore(const fs::path &directory, std::string_view publisherKey) {
		ProjectValidationReport report;
		const auto publisher = assets::PublicKey::FromHex(publisherKey);
		if (!publisher) {
			AddError(
				report, "content.publisher.invalid", "content", {}, "publisher key is missing or invalid"
			);
			return report;
		}
		const auto store = assets::ChunkStore::Open(directory, false);
		if (!store) {
			AddError(
				report,
				"content.store.missing",
				"content",
				directory.string(),
				"processed asset store is missing"
			);
			return report;
		}
		assets::SignatureBytes signature;
		const auto manifest = store->ReadManifest(signature);
		if (!manifest) {
			AddError(
				report,
				"content.manifest.missing",
				"content",
				assets::ChunkStore::MANIFEST_FILE,
				"signed asset manifest is missing or malformed"
			);
			return report;
		}
		if (!assets::VerifyManifestRoot(manifest->Root(), signature, *publisher)) {
			AddError(
				report,
				"content.manifest.signature",
				"content",
				assets::ChunkStore::MANIFEST_FILE,
				"asset manifest signature does not match the public publisher key"
			);
			return report;
		}
		for (const assets::AssetEntry &asset : manifest->Assets()) {
			if (manifest->BundleFor(asset.Root) == nullptr) {
				AddError(
					report,
					"content.asset.unbundled",
					"content",
					asset.Name,
					"catalogue asset belongs to no delivery bundle"
				);
				continue;
			}
			const bool complete =
				assets::VerifyAssetShape(asset) &&
				std::all_of(asset.Chunks.begin(), asset.Chunks.end(), [&](const assets::ChunkEntry &chunk) {
					const std::optional<std::vector<std::byte>> bytes = store->Read(chunk.Hash);
					return bytes && bytes->size() == chunk.Bytes;
				});
			if (!complete) {
				AddError(
					report,
					"content.asset.missing",
					"content",
					asset.Name,
					"catalogue asset is missing or corrupt"
				);
			}
		}
		return report;
	}

	bool WriteProjectPackage(
		const fs::path &stagingRoot,
		const fs::path &destination,
		const ProjectPackageOptions &options,
		ProjectPackageInfo &written,
		ProjectValidationReport &report
	) {
		report = ProjectValidationReport{};
		written = ProjectPackageInfo{};
		if (destination.empty() || destination.extension() != ".zip") {
			AddError(
				report,
				"package.destination.extension",
				"package",
				destination.string(),
				"Project ZIP destination must end in .zip"
			);
			return false;
		}
		if (!ValidPackageOptions(options, report)) {
			return false;
		}
		std::error_code failure;
		if (fs::exists(destination, failure) && !options.ReplaceExisting) {
			AddError(
				report,
				"package.destination.exists",
				"package",
				destination.string(),
				"destination exists and replacement was not requested"
			);
			return false;
		}
		const fs::path backup = destination.string() + ".previous";
		if (options.ReplaceExisting && fs::exists(destination, failure) && fs::exists(backup, failure)) {
			AddError(
				report,
				"package.destination.backup",
				"package",
				backup.string(),
				"recoverable replacement backup already exists"
			);
			return false;
		}

		const auto files = CollectStagingFiles(stagingRoot, report);
		if (!files) {
			return false;
		}
		report.Append(ValidateProcessedAssetStore(stagingRoot / "assets", options.PublisherKey));
		if (!report.Passed()) {
			return false;
		}
		const auto summary = SummarizePayload(*files, report);
		if (!summary) {
			return false;
		}

		written.FormatVersion = PROJECT_PACKAGE_VERSION;
		written.UniverseEntrypoint = PROJECT_UNIVERSE;
		written.MinimumEngine = options.MinimumEngine.empty() ? core::Version() : options.MinimumEngine;
		written.MaximumEngine = options.MaximumEngine.empty() ? core::Version() : options.MaximumEngine;
		written.CreationProfile = options.CreationProfile;
		written.PublisherKey = options.PublisherKey;
		written.Delivery = options.Delivery;
		written.Cdns = options.Cdns;
		written.FileCount = summary->Files;
		written.UncompressedBytes = summary->Bytes;
		written.ContentDigest = summary->Digest.ToHex();
		const std::string projectDocument = WriteProjectDocument(written);

		const fs::path parent = destination.parent_path().empty() ? fs::path(".") : destination.parent_path();
		fs::create_directories(parent, failure);
		if (failure) {
			AddError(
				report,
				"package.destination.parent",
				"package",
				parent.string(),
				"could not create destination directory"
			);
			return false;
		}
		const fs::path partial = UniquePath(parent, ".atomic-project-partial-", ".zip");
		if (partial.empty()) {
			AddError(
				report,
				"package.write.temporary",
				"package",
				parent.string(),
				"could not choose a temporary archive path"
			);
			return false;
		}

		bool archiveWritten = false;
		{
			ArchiveWriter writer;
			if (OpenArchiveWriter(writer, partial, report) &&
				AddMemoryEntry(writer, PROJECT_DOCUMENT, projectDocument, report)) {
				archiveWritten = std::all_of(files->begin(), files->end(), [&](const PayloadFile &file) {
					return AddFileEntry(writer, file, report);
				});
				if (archiveWritten && !mz_zip_writer_finalize_archive(&writer.Zip)) {
					AddError(
						report, "package.write.finalize", "package", partial.string(), ZipError(writer.Zip)
					);
					archiveWritten = false;
				}
			}
		}
		if (!archiveWritten) {
			fs::remove(partial, failure);
			return false;
		}

		ProjectValidationReport verification;
		auto reopened = OpenProject(partial, ProjectPackageLimits{}, verification);
		if (!reopened) {
			report.Append(std::move(verification));
			fs::remove(partial, failure);
			return false;
		}
		reopened.reset();

		const bool replacing = fs::exists(destination, failure);
		if (replacing) {
			fs::rename(destination, backup, failure);
			if (failure) {
				AddError(
					report,
					"package.publish.backup",
					"package",
					destination.string(),
					"could not preserve existing destination"
				);
				fs::remove(partial, failure);
				return false;
			}
		}
		fs::rename(partial, destination, failure);
		if (failure) {
			if (replacing) {
				std::error_code restoreFailure;
				fs::rename(backup, destination, restoreFailure);
			}
			fs::remove(partial, failure);
			AddError(
				report,
				"package.publish.rename",
				"package",
				destination.string(),
				"could not atomically publish project ZIP"
			);
			return false;
		}
		return true;
	}

	std::optional<OpenedProject>
	OpenProject(const fs::path &path, const ProjectPackageLimits &limits, ProjectValidationReport &report) {
		report = ProjectValidationReport{};
		const ProjectKind kind = ClassifyProject(path);
		if (kind == ProjectKind::GameFile || kind == ProjectKind::UniverseFolder) {
			std::error_code failure;
			if (!fs::is_regular_file(path, failure)) {
				AddError(
					report,
					"project.entrypoint.missing",
					"project",
					path.string(),
					"project entrypoint is missing"
				);
				return std::nullopt;
			}
			OpenedProject opened;
			opened.RootPath = path.parent_path();
			opened.EntrypointPath = path;
			if (kind == ProjectKind::UniverseFolder) {
				const fs::path assets = path.parent_path() / "assets";
				if (fs::is_directory(assets, failure)) {
					opened.AssetsPath = assets;
				}
			}
			return opened;
		}
		if (kind != ProjectKind::ProjectZip) {
			AddError(
				report,
				"project.kind.unsupported",
				"project",
				path.string(),
				"path is not .agame, .auniverse, or .zip"
			);
			return std::nullopt;
		}

		ArchiveReader reader;
		if (!OpenArchiveReader(reader, path, report)) {
			return std::nullopt;
		}
		const auto entries = InspectArchive(reader, limits, report);
		if (!entries) {
			return std::nullopt;
		}
		if (!mz_zip_validate_archive(&reader.Zip, MZ_ZIP_FLAG_VALIDATE_HEADERS_ONLY)) {
			AddError(
				report,
				"archive.headers",
				"archive",
				{},
				"project ZIP local and central headers disagree: " + ZipError(reader.Zip)
			);
			return std::nullopt;
		}
		const ArchiveEntry *projectEntry = FindEntry(*entries, PROJECT_DOCUMENT);
		if (projectEntry == nullptr) {
			AddError(
				report,
				"package.project.missing",
				"package",
				std::string(PROJECT_DOCUMENT),
				"project ZIP entrypoint is missing"
			);
			return std::nullopt;
		}
		const auto projectBytes =
			ExtractMemory(reader, *projectEntry, limits.MaximumProjectDocumentBytes, report);
		if (!projectBytes) {
			return std::nullopt;
		}
		const std::string_view projectText(
			reinterpret_cast<const char *>(projectBytes->data()), projectBytes->size()
		);
		const auto package = ReadProjectDocument(projectText, report);
		if (!package) {
			return std::nullopt;
		}
		if (FindEntry(*entries, package->UniverseEntrypoint.generic_string()) == nullptr) {
			AddError(
				report,
				"package.universe.missing",
				"package",
				package->UniverseEntrypoint.generic_string(),
				"declared universe entrypoint is missing"
			);
			return std::nullopt;
		}

		const auto root = CreateExtractionRoot(report);
		if (!root) {
			return std::nullopt;
		}
		OpenedProject opened;
		opened.RootPath = *root;
		opened.OwnsRoot = true;
		opened.PackageMetadata = *package;

		std::vector<ArchiveEntry> ordered = *entries;
		std::sort(ordered.begin(), ordered.end(), [](const ArchiveEntry &left, const ArchiveEntry &right) {
			return left.Path < right.Path;
		});
		assets::Hasher payloadHasher;
		uint64_t payloadFiles = 0;
		uint64_t payloadBytes = 0;
		for (const ArchiveEntry &entry : ordered) {
			if (entry.Path == PROJECT_DOCUMENT) {
				continue;
			}
			if (!ExtractFile(reader, entry, *root, entry.Directory ? nullptr : &payloadHasher, report)) {
				return std::nullopt;
			}
			if (!entry.Directory) {
				payloadFiles++;
				if (!AddChecked(payloadBytes, entry.UncompressedBytes)) {
					AddError(
						report,
						"package.payload.overflow",
						"package",
						entry.Path,
						"payload byte total overflowed"
					);
					return std::nullopt;
				}
			}
		}
		{
			std::ofstream projectFile(*root / fs::path(PROJECT_DOCUMENT), std::ios::binary | std::ios::trunc);
			projectFile.write(projectText.data(), static_cast<std::streamsize>(projectText.size()));
			if (!projectFile) {
				AddError(
					report,
					"archive.project.write",
					"archive",
					std::string(PROJECT_DOCUMENT),
					"could not materialize project document"
				);
				return std::nullopt;
			}
		}
		if (payloadFiles != package->FileCount || payloadBytes != package->UncompressedBytes ||
			payloadHasher.Finish().ToHex() != package->ContentDigest) {
			AddError(
				report,
				"package.payload.digest",
				"package",
				{},
				"payload count, size, or digest does not match project.xml"
			);
			return std::nullopt;
		}

		opened.EntrypointPath = *root / package->UniverseEntrypoint;
		opened.AssetsPath = *root / "assets";
		world::Universe validatedUniverse;
		GameInfo universeInfo;
		std::string universeError;
		if (!LoadGame(validatedUniverse, opened.EntrypointPath, universeInfo, universeError)) {
			AddError(
				report,
				"package.universe.invalid",
				"package",
				package->UniverseEntrypoint.generic_string(),
				"packaged universe could not be loaded: " + universeError
			);
			return std::nullopt;
		}
		const bool sameCdns = universeInfo.Cdns.size() == package->Cdns.size() &&
							  std::equal(
								  universeInfo.Cdns.begin(),
								  universeInfo.Cdns.end(),
								  package->Cdns.begin(),
								  [](const UniverseCdn &left, const UniverseCdn &right) {
									  return left.Name == right.Name && left.Location == right.Location;
								  }
							  );
		if (universeInfo.PublisherKey != package->PublisherKey || !sameCdns) {
			AddError(
				report,
				"package.universe.metadata",
				"package",
				package->UniverseEntrypoint.generic_string(),
				"project.xml and the universe disagree about public content metadata"
			);
			return std::nullopt;
		}
		report.Append(ValidateProcessedAssetStore(opened.AssetsPath, package->PublisherKey));
		if (!report.Passed()) {
			return std::nullopt;
		}
		return opened;
	}
}
