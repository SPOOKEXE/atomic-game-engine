#include "GraphRasterHost.hpp"

#include <engine/core/Log.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/imagegraphexport/GraphDirectoryHost.hpp>

#include <algorithm>
#include <array>
namespace engine::imagegraphexport {
	namespace {
		using namespace engine::imagegraph;
		const Value *Input(const HostNodeInvocation &in, std::string_view port) {
			for (const auto &value : in.Inputs)
				if (value.Port == port) return &value.Data;
			return nullptr;
		}
		const std::string *Text(const HostNodeInvocation &in, std::string_view port) {
			const auto *v = Input(in, port);
			return v ? std::get_if<std::string>(v) : nullptr;
		}
		std::optional<int64_t> Mode(const HostNodeInvocation &in) {
			const auto *v = Input(in, "type");
			if (!v) return {};
			if (const auto *e = std::get_if<EnumValue>(v)) return e->Value;
			if (const auto *n = std::get_if<int64_t>(v)) return *n;
			return {};
		}
		std::filesystem::path TrimmedRoot(std::filesystem::path root) {
			while (root.filename().empty() && root != root.root_path())
				root = root.parent_path();
			return root;
		}
		bool Inside(const std::filesystem::path &root, const std::filesystem::path &file) {
			if (!root.is_absolute() || !file.is_absolute() || file.lexically_normal() != file) return false;
			auto r = root.begin(), f = file.begin();
			for (; r != root.end(); ++r, ++f)
				if (f == file.end() || *r != *f) return false;
			return f != file.end();
		}
		uint64_t LogicalBytes(const GraphDirectoryObservation &observation) {
			uint64_t bytes = sizeof(observation) + observation.NodeId.size() +
							 observation.Root.native().size() * sizeof(std::filesystem::path::value_type);
			for (const auto &listing : observation.Listings) {
				bytes += sizeof(listing) +
						 listing.Directory.native().size() * sizeof(std::filesystem::path::value_type);
				for (const auto &entry : listing.Entries)
					bytes += sizeof(entry) +
							 entry.File.native().size() * sizeof(std::filesystem::path::value_type);
			}
			return bytes;
		}
		struct EnumerationCounts {
			uint64_t Directories = 0, Entries = 0;
			~EnumerationCounts() {
				engine::core::Metrics::Count(
					"image composer directory enumeration operations", double(Directories)
				);
				engine::core::Metrics::Count("image composer directory entries observed", double(Entries));
			}
		};
		bool AdmitPath(const std::filesystem::path &path, uint64_t budget, uint64_t &retained) {
			const uint64_t bytes = path.native().size() * 8 + sizeof(GraphDirectoryEntry) * 2 + 64;
			if (retained > budget || bytes > budget - retained) return false;
			retained += bytes;
			return true;
		}
	}
	std::optional<uint64_t> GraphDirectoryObservationBytes(const GraphDirectoryObservation &observation) {
		constexpr uint64_t maximum = 8 * 1024 * 1024;
		if (observation.NodeId.size() > Limits::MaximumTextBytes || observation.Listings.size() > 64)
			return {};
		uint64_t retained = sizeof(GraphDirectoryObservation) + observation.NodeId.size() + 32;
		if (!AdmitPath(observation.Root, maximum, retained)) return {};
		size_t entries = 0;
		for (const auto &listing : observation.Listings) {
			if (!AdmitPath(listing.Directory, maximum, retained)) return {};
			for (const auto &entry : listing.Entries)
				if (++entries > 256 || !AdmitPath(entry.File, maximum, retained)) return {};
		}
		return retained;
	}
	bool ObserveGraphDirectory(
		const HostNodeInvocation &in,
		std::span<const GraphDirectoryGrant> grants,
		GraphDirectoryObservation &out,
		std::string &failure
	) try {
		ENGINE_PROFILE_CAT("image composer directory observation", engine::core::ProfileCategory::Engine);
		EnumerationCounts counts;
		const auto fail = [&](const char *message) {
			failure = message;
			return false;
		};
		const auto *path = Text(in, "path");
		const auto *rawRecursive = Input(in, "recursive");
		const auto *recursive = rawRecursive ? std::get_if<bool>(rawRecursive) : nullptr;
		if (in.Authored.Type != "pc.directory_search" || !path || path->size() > 4096 || !recursive ||
			in.Authored.Id.size() > Limits::MaximumTextBytes)
			return fail("directory observation requires resolved root and recursive controls");
		const GraphDirectoryGrant *grant = nullptr;
		for (const auto &candidate : grants)
			if (candidate.NodeId == in.Authored.Id) {
				if (grant) return fail("directory root grant is duplicated");
				grant = &candidate;
			}
		if (!grant || grant->Root.string() != *path || !grant->Root.is_absolute() ||
			grant->Root.lexically_normal() != grant->Root)
			return fail("directory observation requires its exact absolute root grant");
		std::error_code error;
		if (std::filesystem::weakly_canonical(grant->Root, error) != TrimmedRoot(grant->Root) || error ||
			std::filesystem::is_symlink(grant->Root, error) || error ||
			!std::filesystem::is_directory(grant->Root, error) || error)
			return fail("directory root must be a canonical real directory without symbolic links");
		const auto budget = std::min<uint64_t>(in.MaximumOperationBytes / 8, 8 * 1024 * 1024);
		uint64_t retained = sizeof(GraphDirectoryObservation) + in.Authored.Id.size() + 32;
		if (retained > budget) return fail("directory observation exceeds byte budget");
		if (!AdmitPath(grant->Root, budget, retained))
			return fail("directory root exceeds observation byte budget");
		GraphDirectoryObservation observation;
		observation.Root = TrimmedRoot(grant->Root);
		observation.NodeId = in.Authored.Id;
		observation.Recursive = *recursive;
		std::vector<std::filesystem::path> stack{observation.Root};
		size_t totalEntries = 0;
		while (!stack.empty()) {
			const auto directory = std::move(stack.back());
			stack.pop_back();
			if (observation.Listings.size() == 64)
				return fail("directory traversal exceeds 64 observed directories");
			if (!AdmitPath(directory, budget, retained))
				return fail("directory listing exceeds observation byte budget");
			GraphDirectoryListing listing;
			listing.Directory = directory;
			++counts.Directories;
			std::filesystem::directory_iterator iterator(directory, error), end;
			if (error) return fail("cannot observe the granted directory");
			for (; iterator != end; iterator.increment(error)) {
				if (error) return fail("directory observation failed during traversal");
				++counts.Entries;
				const auto &entry = *iterator;
				const auto &file = entry.path();
				if (++totalEntries > 256 || !Inside(observation.Root, file) ||
					!AdmitPath(file, budget, retained))
					return fail("directory entries exceed their root or observation budget");
				const auto status = entry.symlink_status(error);
				if (error || std::filesystem::is_symlink(status))
					return fail("directory symbolic links require an explicit native observation");
				const bool child = std::filesystem::is_directory(status);
				if (!child && !std::filesystem::is_regular_file(status))
					return fail("directory contains unsupported non-regular entries");
				listing.Entries.push_back({file, child});
			}
			if (error) return fail("directory observation failed during traversal");
			// The pinned non-recursive source still pushes immediate root child directories.
			if (*recursive || observation.Listings.empty())
				for (const auto &entry : listing.Entries)
					if (entry.Directory) {
						if (!AdmitPath(entry.File, budget, retained))
							return fail("directory traversal stack exceeds observation byte budget");
						stack.push_back(entry.File);
					}
			observation.Listings.push_back(std::move(listing));
		}
		engine::core::Metrics::Count(
			"image composer directory observed logical bytes", double(LogicalBytes(observation))
		);
		out = std::move(observation);
		return true;
	} catch (const std::filesystem::filesystem_error &) {
		failure = "directory observation filesystem operation failed";
		return false;
	} catch (const std::bad_alloc &) {
		failure = "directory observation allocation failed";
		return false;
	}
	bool CaptureGraphDirectory(
		const HostNodeInvocation &in,
		const GraphDirectoryObservation &observation,
		std::span<const GraphFileGrant> grants,
		const engine::assets::ContentPolicy &policy,
		HostNodeCapture &out,
		std::string &failure
	) try {
		ENGINE_PROFILE_CAT("image composer directory capture", engine::core::ProfileCategory::Engine);
		const auto fail = [&](const char *message) {
			failure = message;
			return false;
		};
		const auto *path = Text(in, "path"), *filter = Text(in, "extensions");
		const auto mode = Mode(in);
		if (in.Authored.Type != "pc.directory_search" || !path || !filter || !mode || *mode < 0 ||
			*mode > 1 || observation.Root != TrimmedRoot(std::filesystem::path(*path)) ||
			observation.Listings.size() > 64)
			return fail("directory capture requires its matching bounded observed root and controls");
		const auto authored = NodeClonePayloadBytes(in.Authored);
		if (!authored || *authored > in.MaximumOperationBytes / 8)
			return fail("directory authored capture exceeds byte budget");
		uint64_t retained = *authored;
		for (const auto &input : in.Inputs) {
			const auto bytes = ValueClonePayloadBytes(input.Data);
			const uint64_t overhead = sizeof(AuthoredValue) + input.Port.size() + 1;
			if (!bytes || overhead > in.MaximumOperationBytes / 8 - retained ||
				*bytes > in.MaximumOperationBytes / 8 - retained - overhead)
				return fail("directory resolved capture exceeds byte budget");
			retained += overhead + *bytes;
		}
		const auto *rawRecursive = Input(in, "recursive");
		const auto *recursive = rawRecursive ? std::get_if<bool>(rawRecursive) : nullptr;
		if (!recursive || *recursive != observation.Recursive || observation.NodeId != in.Authored.Id ||
			observation.Listings.empty())
			return fail("directory observation is stale for this node or recursion control");
		uint64_t observedBytes = sizeof(GraphDirectoryObservation) + observation.NodeId.size() + 32;
		const auto observationBudget = std::min<uint64_t>(in.MaximumOperationBytes / 8, 8 * 1024 * 1024);
		if (observation.NodeId.size() > Limits::MaximumTextBytes ||
			!AdmitPath(observation.Root, observationBudget, observedBytes) ||
			observation.Root != TrimmedRoot(std::filesystem::path(*path)))
			return fail("directory recorded root exceeds byte budget");
		std::vector<std::filesystem::path> traversal{observation.Root};
		std::vector<const std::filesystem::path *> unique;
		for (size_t index = 0; index < observation.Listings.size(); ++index) {
			const auto &listing = observation.Listings[index];
			if (traversal.empty() || listing.Directory != traversal.back() ||
				!AdmitPath(listing.Directory, observationBudget, observedBytes))
				return fail("directory recorded traversal disagrees with source stack order");
			traversal.pop_back();
			for (const auto &entry : listing.Entries) {
				if (unique.size() == 256 || !AdmitPath(entry.File, observationBudget, observedBytes) ||
					entry.File.parent_path() != listing.Directory || !Inside(observation.Root, entry.File) ||
					std::find_if(unique.begin(), unique.end(), [&](const auto *path) {
						return *path == entry.File;
					}) != unique.end())
					return fail("directory recorded entries are duplicated or exceed their bounded root");
				unique.push_back(&entry.File);
				if (entry.Directory && (*recursive || index == 0)) {
					if (!AdmitPath(entry.File, observationBudget, observedBytes))
						return fail("directory recorded stack exceeds byte budget");
					traversal.push_back(entry.File);
				}
			}
		}
		if (!traversal.empty()) return fail("directory recorded traversal is incomplete");
		std::vector<std::string_view> extensions;
		size_t start = 0;
		while (start <= filter->size()) {
			const auto at = filter->find(';', start);
			extensions.push_back(
				std::string_view(*filter).substr(start, at == std::string::npos ? at : at - start)
			);
			if (extensions.size() > 256) return fail("directory extension filters exceed bounded domain");
			if (at == std::string::npos) break;
			start = at + 1;
		}
		ArrayValue paths;
		paths.ElementType = ValueType::Text;
		size_t count = 0;
		for (const auto &listing : observation.Listings) {
			if (listing.Directory != observation.Root && !Inside(observation.Root, listing.Directory))
				return fail("directory listing escapes observed root");
			for (const auto &entry : listing.Entries) {
				if (++count > 256 || !Inside(observation.Root, entry.File))
					return fail("directory recorded entries escape bounded root");
				if (entry.Directory) continue;
				std::string ext = entry.File.extension().string();
				for (char &c : ext)
					if (c >= 'A' && c <= 'Z') c = char(c + ('a' - 'A'));
				if (std::find(extensions.begin(), extensions.end(), ext) == extensions.end()) continue;
				if (ext != ".png" && ext != ".jpg" && ext != ".jpeg" && ext != ".gif") continue;
				if (!*mode) paths.Elements.emplace_back(entry.File.string());
			}
		}
		HostNodeCapture candidate;
		if (*mode == 0) {
			uint64_t imageBytes = paths.Elements.size() * (sizeof(Image) + sizeof(ElementValue)) * 2 +
								  sizeof(HostCapturedImageArray);
			if (imageBytes > in.MaximumOperationBytes / 4)
				return fail("directory image and path array ledger exceeds byte budget");
			std::vector<Image> images;
			images.reserve(paths.Elements.size());
			ArrayValue validPaths;
			validPaths.ElementType = ValueType::Text;
			validPaths.Elements.reserve(paths.Elements.size());
			// One decoder runs at a time beside the admitted retained images.
			const uint64_t perImage = in.MaximumOperationBytes / 4;
			for (const auto &element : paths.Elements) {
				const auto &path = std::get<std::string>(element);
				const GraphFileGrant *selected = nullptr;
				for (const auto &grant : grants)
					if (grant.NodeId == in.Authored.Id && grant.Resource == path) {
						if (selected) return fail("directory image resource grant is duplicated");
						selected = &grant;
					}
				if (!selected || selected->Write || selected->File.string() != path)
					return fail("directory image requires its exact named read resource grant");
				Node node;
				node.Id = in.Authored.Id;
				node.Type = "pc.image";
				std::array<AuthoredValue, 2> controls{{{"path", path}, {"padding", Vector4{}}}};
				GraphFileGrant exact{node.Id, selected->File, false};
				HostNodeCapture decoded;
				RasterFailure classification = RasterFailure::Refused;
				if (!CaptureGraphRaster(
						{node, in.Request, controls, {}, perImage, in.Timeline, in.OutputFormat},
						std::span<const GraphFileGrant>(&exact, 1),
						policy,
						decoded,
						failure,
						&classification
					)) {
					if (classification != RasterFailure::SourceDecodeFailure) return false;
					engine::core::Metrics::Count("image composer directory invalid images skipped", 1);
					ENGINE_WARN(
						"Directory Search {} skipped invalid image {}: {}", in.Authored.Id, path, failure
					);
					continue;
				}
				auto image = std::move(decoded.Images.front().Data);
				const uint64_t charge =
					image.Pixels.capacity() + sizeof(Image) + path.size() + sizeof(ElementValue) + 64;
				if (charge > in.MaximumOperationBytes / 4 - imageBytes)
					return fail("directory decoded images exceed aggregate byte budget");
				imageBytes += charge;
				images.push_back(std::move(image));
				validPaths.Elements.emplace_back(path);
			}
			paths = std::move(validPaths);
			candidate.ImageArrays.push_back({"outputs", std::move(images)});
		}

		candidate.Authored = in.Authored;
		candidate.Tick = in.Request.Tick;
		candidate.Subframe = in.Request.Subframe;
		candidate.NegativeFrame = in.Request.NegativeFrame;
		candidate.Inputs.assign(in.Inputs.begin(), in.Inputs.end());
		candidate.Outputs = {{"paths", paths}};
		if (*mode == 1) {
			ArrayValue empty;
			empty.ElementType = ValueType::Text;
			candidate.Outputs.push_back({"outputs", std::move(empty)});
		}
		uint64_t emittedImages = 0, emittedBytes = 0;
		for (const auto &array : candidate.ImageArrays)
			for (const auto &image : array.Frames) {
				++emittedImages;
				emittedBytes += image.Pixels.size();
			}
		engine::core::Metrics::Count("image composer directory images emitted", double(emittedImages));
		engine::core::Metrics::Count(
			"image composer directory image payload bytes emitted", double(emittedBytes)
		);
		out = std::move(candidate);
		failure.clear();
		return true;
	} catch (const std::filesystem::filesystem_error &) {
		failure = "directory capture filesystem operation failed";
		return false;
	} catch (const std::bad_alloc &) {
		failure = "directory capture allocation failed";
		return false;
	}
	void GraphDirectoryHost::Refresh(std::string_view nodeId) {
		ENGINE_PROFILE_CAT("image composer directory refresh", engine::core::ProfileCategory::Engine);
		std::erase_if(Observations, [&](const auto &observation) { return observation.NodeId == nodeId; });
		RetainedBytes = 0;
		for (const auto &record : Observations)
			if (const auto bytes = GraphDirectoryObservationBytes(record)) RetainedBytes += *bytes;
		uint64_t logicalBytes = 0;
		for (const auto &record : Observations)
			logicalBytes += LogicalBytes(record);
		engine::core::Metrics::SetGauge(
			"image composer directory last session logical retained bytes", double(logicalBytes)
		);
	}
	bool GraphDirectoryHost::Capture(
		const HostNodeInvocation &in, HostNodeCapture &out, std::string &failure
	) try {
		ENGINE_PROFILE_CAT("image composer directory session", engine::core::ProfileCategory::Engine);
		engine::core::Metrics::Count("image composer directory session requests", 1);
		if (in.Authored.Type != "pc.directory_search") {
			failure = "directory provider does not own this node";
			return false;
		}
		const auto *path = Text(in, "path");
		const auto *rawRecursive = Input(in, "recursive");
		const auto *recursive = rawRecursive ? std::get_if<bool>(rawRecursive) : nullptr;
		const GraphDirectoryGrant *grant = nullptr;
		for (const auto &candidate : Directories) {
			if (candidate.NodeId != in.Authored.Id) continue;
			if (grant) {
				failure = "directory root grant is duplicated";
				return false;
			}
			grant = &candidate;
		}
		if (!path || !recursive || !grant || grant->Root.string() != *path || !grant->Root.is_absolute() ||
			grant->Root.lexically_normal() != grant->Root) {
			failure = "directory provider requires its exact absolute root grant and controls";
			return false;
		}
		const uint64_t ledgerLimit = std::min<uint64_t>(8 * 1024 * 1024, in.MaximumOperationBytes / 4);
		if (RetainedBytes >= ledgerLimit) {
			failure = "directory observations exceed the session byte budget";
			return false;
		}
		HostNodeInvocation bounded = in;
		bounded.MaximumOperationBytes -= RetainedBytes;
		const auto root = TrimmedRoot(grant->Root);
		for (const auto &record : Observations)
			if (record.NodeId == in.Authored.Id && record.Root == root && record.Recursive == *recursive) {
				engine::core::Metrics::Count("image composer directory order replays", 1);
				return CaptureGraphDirectory(bounded, record, Files, Policy, out, failure);
			}
		if (Observations.size() == 64) {
			failure = "directory observations exceed the 64 control-state limit";
			return false;
		}
		GraphDirectoryObservation observation;
		if (!ObserveGraphDirectory(bounded, Directories, observation, failure)) return false;
		const auto bytes = GraphDirectoryObservationBytes(observation);
		if (!bytes || *bytes > ledgerLimit - RetainedBytes) {
			failure = "directory recorded order exceeds its session byte budget";
			return false;
		}
		bounded.MaximumOperationBytes -= *bytes;
		HostNodeCapture candidate;
		if (!CaptureGraphDirectory(bounded, observation, Files, Policy, candidate, failure)) return false;
		Observations.push_back(std::move(observation));
		RetainedBytes += *bytes;
		uint64_t logicalBytes = 0;
		for (const auto &record : Observations)
			logicalBytes += LogicalBytes(record);
		engine::core::Metrics::SetGauge(
			"image composer directory last session logical retained bytes", double(logicalBytes)
		);
		out = std::move(candidate);
		return true;
	} catch (const std::filesystem::filesystem_error &) {
		failure = "directory host filesystem operation failed";
		return false;
	} catch (const std::bad_alloc &) {
		failure = "directory host allocation failed";
		return false;
	}
}
