#include "ImportBudget.hpp"

#include <engine/core/Profiling.hpp>
#include <engine/imagegraph/Catalogue.hpp>
#include <engine/imagegraphio/PxcxAppend.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <type_traits>

namespace engine::imagegraphio {
	namespace {
		using namespace imagegraph;
		thread_local detail::ImportBudget *jsonBudget = nullptr;
		struct AppendWorkExceeded {};
		struct AppendWork {
			uint64_t Remaining = 64ull * 1024 * 1024;
			void Charge(uint64_t count, uint64_t bytes) {
				if (count && bytes > Remaining / count) throw AppendWorkExceeded{};
				Remaining -= count * bytes;
			}
		};
		template <class T> struct JsonAllocator {
			using value_type = T;
			detail::ImportBudget *Budget = jsonBudget;
			JsonAllocator() = default;
			template <class U> JsonAllocator(const JsonAllocator<U> &other) : Budget(other.Budget) {}
			T *allocate(size_t count) {
				if (!Budget || count > UINT64_MAX / sizeof(T) || !Budget->Hold(count * sizeof(T)))
					throw std::bad_alloc{};
				try {
					return std::allocator<T>{}.allocate(count);
				} catch (...) {
					Budget->Release(count * sizeof(T));
					throw;
				}
			}
			void deallocate(T *pointer, size_t count) {
				std::allocator<T>{}.deallocate(pointer, count);
				Budget->Release(count * sizeof(T));
			}
			template <class U> bool operator==(const JsonAllocator<U> &other) const {
				return Budget == other.Budget;
			}
		};
		using JsonString = std::basic_string<char, std::char_traits<char>, JsonAllocator<char>>;
		using AppendJson = nlohmann::basic_json<
			nlohmann::ordered_map,
			std::vector,
			JsonString,
			bool,
			int64_t,
			uint64_t,
			double,
			JsonAllocator>;
		struct JsonScope {
			detail::ImportBudget *Previous = jsonBudget;
			explicit JsonScope(detail::ImportBudget &budget) {
				jsonBudget = &budget;
			}
			~JsonScope() {
				jsonBudget = Previous;
			}
		};
		bool ArchiveBounds(const bake::PxcxArchive &archive) {
			return archive.Nodes.size() <= bake::PxcxLimits::MaximumNodes &&
				   archive.Nodes.capacity() <= bake::PxcxLimits::MaximumNodes &&
				   archive.Links.size() <= bake::PxcxLimits::MaximumLinks &&
				   archive.Links.capacity() <= bake::PxcxLimits::MaximumLinks &&
				   archive.OriginalBytes.capacity() <= bake::PxcxLimits::MaximumArchiveBytes &&
				   archive.GraphJson.capacity() <= 2 * bake::PxcxLimits::MaximumGraphJsonBytes &&
				   archive.MetadataPayload.capacity() <= bake::PxcxLimits::MaximumMetadataBytes &&
				   archive.ThumbnailRgba.capacity() <= bake::PxcxLimits::ThumbnailRgbaBytes &&
				   archive.MetadataText.capacity() <= bake::PxcxLimits::MaximumMetadataBytes;
		}
		uint64_t ArchiveBytes(const bake::PxcxArchive &archive) {
			uint64_t bytes = sizeof(archive) + archive.OriginalBytes.capacity() +
							 archive.ThumbnailRgba.capacity() + archive.MetadataPayload.capacity() +
							 archive.MetadataText.capacity() + archive.GraphJson.capacity() +
							 archive.Nodes.capacity() * sizeof(bake::PxcxNodeFact) +
							 archive.Links.capacity() * sizeof(bake::PxcxLinkFact);
			for (const auto &node : archive.Nodes)
				bytes += node.Id.capacity() + node.Type.capacity();
			for (const auto &link : archive.Links)
				bytes += link.FromNode.capacity() + link.ToNode.capacity();
			return bytes;
		}
		std::optional<uint64_t> ImportBytes(const PxcxImport &project) {
			if (!ArchiveBounds(project.Source) ||
				project.Diagnostics.capacity() > Limits::MaximumEvaluationBytes / sizeof(Diagnostic) ||
				project.GroupBootstrap.capacity() > Limits::MaximumNodes ||
				project.GroupBindings.capacity() >
					Limits::MaximumEvaluationBytes / sizeof(GroupSubtypeBinding))
				return std::nullopt;
			const auto document = DocumentRetainedPayloadBytes(project.Graph);
			const auto prebinding = project.GroupPrebinding
										? DocumentRetainedPayloadBytes(*project.GroupPrebinding)
										: std::optional<uint64_t>{0};
			if (!document || !prebinding) return std::nullopt;
			uint64_t bytes = ArchiveBytes(project.Source) + *document + *prebinding +
							 project.Diagnostics.capacity() * sizeof(Diagnostic) +
							 project.GroupBootstrap.capacity() * sizeof(PxcxGroupBootstrapRecord) +
							 project.GroupBindings.capacity() * sizeof(GroupSubtypeBinding);
			for (const auto &error : project.Diagnostics)
				bytes += error.NodeId.capacity() + error.Port.capacity() + error.Message.capacity();
			for (const auto &record : project.GroupBootstrap)
				bytes += record.NodeId.capacity();
			for (const auto &record : project.GroupBindings)
				bytes += record.NodeId.capacity() + record.OwnerId.capacity() + record.Port.capacity() +
						 record.AnimatorPort.capacity() + record.Axes.OwnerId.capacity() +
						 record.Axes.Port.capacity() + record.Axes.InstanceBase.capacity();
			return bytes;
		}
		std::optional<AppendJson> ParseText(std::string_view text, AppendWork &work) {
			bool ambiguous = false;
			std::vector<AppendJson, JsonAllocator<AppendJson>> keys;
			std::vector<size_t, JsonAllocator<size_t>> nameLengths;
			auto parsed = AppendJson::parse(
				text.begin(), text.end(), [&](int depth, AppendJson::parse_event_t event, AppendJson &item) {
					if (depth > int(bake::PxcxLimits::MaximumJsonDepth))
						throw std::length_error("append JSON depth");
					if (event == AppendJson::parse_event_t::object_start) {
						keys.push_back(AppendJson::object());
						nameLengths.push_back(0);
					} else if (event == AppendJson::parse_event_t::object_end) {
						keys.pop_back();
						nameLengths.pop_back();
					} else if (event == AppendJson::parse_event_t::key) {
						const auto &key = item.get_ref<const JsonString &>();
						nameLengths.back() = std::max(nameLengths.back(), key.size());
						work.Charge(keys.back().size() * 3 + 1, nameLengths.back() + 1);
						if (keys.back().contains(key)) ambiguous = true;
						keys.back()[key] = true;
					}
					return true;
				}
			);
			if (ambiguous || !parsed.is_object()) return std::nullopt;
			return parsed;
		}
		std::optional<AppendJson> Parse(const bake::PxcxArchive &archive, AppendWork &work) {
			if (archive.GraphJson.empty() || archive.GraphJson.back() != '\0') return std::nullopt;
			auto root =
				ParseText(std::string_view(archive.GraphJson).substr(0, archive.GraphJson.size() - 1), work);
			if (!root || !root->contains("nodes") || !root->at("nodes").is_array()) return std::nullopt;
			return root;
		}
		void RecordWork(const AppendJson &value, AppendWork &work) {
			work.Charge(1, 1);
			if (value.is_string()) work.Charge(1, value.get_ref<const JsonString &>().size());
			if (!value.is_structured()) return;
			if (value.is_object()) {
				size_t longest = 0;
				for (const auto &entry : value.items())
					longest = std::max(longest, entry.key().size());
				work.Charge(64 * value.size(), longest + 1);
			}
			for (const auto &child : value)
				RecordWork(child, work);
		}
		uint64_t CodecDomBytes(const AppendJson &value) {
			// grug count values before bake uses its ordinary libstdc++ JSON allocator.
			// 256 covers map nodes, object/array headers and doubled vector slots per value.
			// text allowance covers keys, token buffers and string growth; this counts payload, not RSS.
			uint64_t bytes = 256;
			if (value.is_string()) bytes += 8 * value.get_ref<const JsonString &>().size();
			if (value.is_object())
				for (const auto &entry : value.items())
					bytes += 256 + 8 * entry.key().size();
			if (value.is_structured())
				for (const auto &child : value)
					bytes += CodecDomBytes(child);
			return bytes;
		}
		std::string Text(const AppendJson &value) {
			const auto &text = value.get_ref<const JsonString &>();
			return {text.data(), text.size()};
		}
		bool Namespace(std::string_view text) {
			return !text.empty() && text.size() <= 255 &&
				   std::all_of(text.begin(), text.end(), [](unsigned char c) {
					   return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
							  c == '_' || c == '-';
				   });
		}
		bool GroupType(const AppendJson &node) {
			if (!node.contains("type") || !node["type"].is_string()) return false;
			const auto &type = node["type"].get_ref<const JsonString &>();
			// grug include pinned collection subclasses whose inherited callbacks remap custom sockets.
			return type == "Node_Group" || type == "Node_Collection" || type == "Node_Canvas_Group" ||
				   type == "Node_DynaSurf" || type == "Node_Feedback" || type == "Node_Iterate" ||
				   type == "Node_Iterate_Each" || type == "Node_Iterate_Filter" ||
				   type == "Node_Iterate_Sort" || type == "Node_Iterator" || type == "Node_Pixel_Builder" ||
				   type == "Node_Smoke_Group" || type == "Node_Strand_Group" || type == "Node_VFX_Group";
		}
		AppendJson CollectionDefaults() {
			return {
				{"description", ""},
				{"author", ""},
				{"contact", ""},
				{"alias", ""},
				{"file_id", 0},
				{"tags", AppendJson::array()},
				{"version", 121092},
				{"isDefault", false},
				{"preview_frames", 1},
				{"deprecated", false},
				{"aut_id", 0}
			};
		}
		void Remap(AppendJson &value, const AppendJson &ids, AppendWork &work, size_t maximumIdBytes) {
			if (!value.is_string()) return;
			work.Charge(
				ids.size() + 1, std::max(maximumIdBytes, value.get_ref<const JsonString &>().size()) + 1
			);
			const auto found = ids.find(value.get_ref<const JsonString &>());
			if (found != ids.end()) value = *found;
		}
		void Field(
			AppendJson &record,
			const char *name,
			const AppendJson &ids,
			AppendWork &work,
			size_t maximumIdBytes
		) {
			const auto found = record.find(name);
			if (found != record.end()) Remap(*found, ids, work, maximumIdBytes);
		}
		void Connections(
			AppendJson &node,
			const char *field,
			const AppendJson &ids,
			AppendWork &work,
			size_t maximumIdBytes
		) {
			const auto values = node.find(field);
			if (values == node.end() || !values->is_array()) return;
			for (size_t index = 0; index < values->size(); ++index) {
				// grug source never connects updatedOutTrigger or surplus inspector records.
				if (std::string_view(field) == "inspectInputs" && (index == 3 || index >= 5)) continue;
				auto &input = (*values)[index];
				if (!input.is_object() || !input.contains("from_node")) continue;
				const auto &from = input["from_node"];
				work.Charge(
					ids.size() + 1,
					from.is_string() ? std::max(maximumIdBytes, from.get_ref<const JsonString &>().size()) + 1
									 : 1
				);
				if (from.is_string() && ids.contains(from.get_ref<const JsonString &>()))
					Remap(input["from_node"], ids, work, maximumIdBytes);
				else {
					// grug default APPEND connects only inside the loaded node set.
					input.erase("from_node");
					input.erase("from_index");
					input.erase("from_tag");
				}
			}
		}
	}

	bool AppendPxcxProject(
		const bake::PxcxArchive &destination,
		const bake::PxcxArchive &incoming,
		const PxcxAppendOptions &options,
		PxcxAppendResult &result,
		Diagnostic &diagnostic
	) try {
		ENGINE_PROFILE("imagegraphio.source_append");
		diagnostic = {};
		const auto fail = [&](Status code, const char *message) {
			diagnostic = {code, {}, {}, message};
			return false;
		};
		if (!Namespace(options.Namespace) || !std::isfinite(options.Offset.X) ||
			!std::isfinite(options.Offset.Y) ||
			options.Context.size() > bake::PxcxLimits::MaximumNodeTextBytes)
			return fail(Status::InvalidValue, "PXC append namespace, context or offset is invalid");
		if (!options.MaximumOperationBytes ||
			options.MaximumOperationBytes > Limits::MaximumEvaluationBytes ||
			destination.GraphJson.size() > bake::PxcxLimits::MaximumGraphJsonBytes ||
			incoming.GraphJson.size() > bake::PxcxLimits::MaximumGraphJsonBytes ||
			destination.OriginalBytes.size() > bake::PxcxLimits::MaximumArchiveBytes ||
			incoming.OriginalBytes.size() > bake::PxcxLimits::MaximumArchiveBytes)
			return fail(Status::LimitExceeded, "PXC append operation bounds are invalid");
		if (destination.MetadataNumber != 121092 || incoming.MetadataNumber != 121092)
			return fail(Status::UnsupportedExecution, "PXC append requires the pinned source save version");
		if (!ArchiveBounds(destination) || !ArchiveBounds(incoming) ||
			result.Nodes.capacity() > Limits::MaximumNodes ||
			result.MetadataJson.capacity() > bake::PxcxLimits::MaximumMetadataBytes)
			return fail(Status::LimitExceeded, "PXC append retained tables exceed bounds");
		detail::ImportBudget budget(options.MaximumOperationBytes);
		if (!budget.Hold(
				sizeof(options) + options.Namespace.capacity() + options.Context.capacity() +
				result.MetadataJson.capacity()
			))
			return fail(Status::LimitExceeded, "PXC append options and metadata exceed operation bounds");
		const auto prior = ImportBytes(result.Project);
		if (!prior || !budget.Hold(*prior) ||
			!budget.Hold(result.Nodes.capacity() * sizeof(PxcxAppendedNode)))
			return fail(Status::LimitExceeded, "retained PXC append result exceeds operation bounds");
		for (const auto &node : result.Nodes)
			if (!budget.Hold(node.SourceId.capacity() + node.NodeId.capacity()))
				return fail(Status::LimitExceeded, "retained PXC append identities exceed operation bounds");
		if (!budget.Hold(ArchiveBytes(destination)) ||
			(&destination != &incoming && !budget.Hold(ArchiveBytes(incoming))))
			return fail(Status::LimitExceeded, "borrowed PXC append archives exceed operation bounds");
		JsonScope scope(budget);
		AppendWork work;
		auto root = Parse(destination, work), append = Parse(incoming, work);
		if (!root || !append)
			return fail(Status::InvalidValue, "PXC append source JSON is invalid or ambiguous");
		if (root->at("nodes").size() > Limits::MaximumNodes ||
			append->at("nodes").size() > Limits::MaximumNodes - root->at("nodes").size())
			return fail(Status::LimitExceeded, "PXC appended node count exceeds bounds");
		RecordWork(*root, work);
		RecordWork(*append, work);
		const auto destinationDomBytes = CodecDomBytes(*root), incomingDomBytes = CodecDomBytes(*append);
		constexpr uint64_t codecFixedBytes = 2ull * 1024 * 1024;
		// grug writer readback validates pinned bytes after parser work and scratch admission.
		for (const auto *archive : {&destination, &incoming}) {
			const uint64_t workspace = ArchiveBytes(*archive) * 4 + codecFixedBytes +
									   (archive == &destination ? destinationDomBytes : incomingDomBytes);
			if (!budget.Hold(workspace))
				return fail(Status::LimitExceeded, "PXC append source validation exceeds operation bounds");
			{
				std::vector<std::byte> checked;
				std::string failure;
				if (!bake::WritePxcx(*archive, checked, failure) || checked.empty() ||
					checked != archive->OriginalBytes)
					return fail(
						Status::InvalidValue,
						"PXC append archive has untracked changes or invalid source bytes"
					);
			}
			budget.Release(workspace);
		}
		AppendJson ids = AppendJson::object();
		size_t maximumIdBytes = 0;
		for (const auto &node : incoming.Nodes)
			maximumIdBytes = std::max(maximumIdBytes, node.Id.size());
		maximumIdBytes += options.Namespace.size() + 1;
		for (const auto &node : append->at("nodes")) {
			if (!node.is_object() || !node.contains("id") || !node["id"].is_string())
				return fail(Status::InvalidValue, "PXC append node identity is invalid");
			const auto &id = node["id"].get_ref<const JsonString &>();
			work.Charge(ids.size() * 2 + 1, maximumIdBytes + 1);
			if (id.empty() || ids.contains(id) ||
				id.size() > bake::PxcxLimits::MaximumNodeTextBytes - options.Namespace.size() - 1)
				return fail(Status::InvalidValue, "PXC append node identities are duplicated or too long");
			JsonString fresh(options.Namespace.data(), options.Namespace.size());
			fresh += '/';
			fresh += id;
			if (std::any_of(root->at("nodes").begin(), root->at("nodes").end(), [&](const auto &existing) {
					work.Charge(
						1,
						std::max(existing["id"].template get_ref<const JsonString &>().size(), fresh.size()) +
							1
					);
					return existing["id"] == fresh;
				}))
				return fail(Status::InvalidValue, "PXC append namespace is already in use");
			ids[id] = std::move(fresh);
		}
		if (!options.Context.empty() &&
			std::none_of(root->at("nodes").begin(), root->at("nodes").end(), [&](const auto &node) {
				work.Charge(
					1,
					std::max(
						node["id"].template get_ref<const JsonString &>().size(), options.Context.size()
					) + 1
				);
				return node["id"] == JsonString(options.Context.data(), options.Context.size()) &&
					   GroupType(node);
			}))
			return fail(Status::InvalidValue, "PXC append context is not a destination group");
		PxcxAppendResult candidate;
		if (!budget.Hold(append->at("nodes").size() * sizeof(PxcxAppendedNode)))
			return fail(Status::LimitExceeded, "PXC append ordering exceeds operation bounds");
		candidate.Nodes.reserve(append->at("nodes").size());
		if (!budget.Hold(
				(candidate.Nodes.capacity() - append->at("nodes").size()) * sizeof(PxcxAppendedNode)
			))
			return fail(Status::LimitExceeded, "PXC append ordering capacity exceeds bounds");
		if (append->contains("metadata")) {
			if (!append->at("metadata").is_object())
				return fail(Status::InvalidValue, "PXC append collection metadata is malformed");
			const auto metadata = append->at("metadata").dump();
			if (metadata.size() > bake::PxcxLimits::MaximumMetadataBytes ||
				!budget.Hold(std::max(metadata.size(), std::string{}.capacity()) + 1))
				return fail(Status::LimitExceeded, "PXC append collection metadata exceeds bounds");
			candidate.MetadataJson.assign(metadata.data(), metadata.size());
		}
		for (auto &node : append->at("nodes")) {
			work.Charge(ids.size() + 1, maximumIdBytes + 1);
			const auto &mapped = ids[node["id"].get_ref<const JsonString &>()];
			if (!budget.Hold(
					std::max(
						node["id"].template get_ref<const JsonString &>().size(), std::string{}.capacity()
					) +
					1 + std::max(mapped.get_ref<const JsonString &>().size(), std::string{}.capacity()) + 1
				))
				return fail(Status::LimitExceeded, "PXC append ordering names exceed bounds");
			auto oldId = Text(node["id"]);
			auto newId = Text(mapped);
			const auto parent = node.find("group");
			const bool topLevel =
				parent == node.end() || parent->is_null() ||
				(parent->is_number_integer() &&
				 (parent->template get<int64_t>() == -1 || parent->template get<int64_t>() == -4)) ||
				(parent->is_string() && parent->template get_ref<const JsonString &>().empty());
			candidate.Nodes.push_back({std::move(oldId), std::move(newId), topLevel});
			Field(node, "id", ids, work, maximumIdBytes);
			Field(node, "group", ids, work, maximumIdBytes);
			// grug source remaps instance bases only while loading a saved parent group.
			if (!topLevel) Field(node, "instanceBase", ids, work, maximumIdBytes);
			Field(node, "ictx", ids, work, maximumIdBytes);
			if (GroupType(node)) Field(node, "tool", ids, work, maximumIdBytes);
			if (topLevel) {
				if (!options.Context.empty())
					node["group"] = JsonString(options.Context.data(), options.Context.size());
				const double x = node.value("x", 0.0) + options.Offset.X,
							 y = node.value("y", 0.0) + options.Offset.Y;
				if (!std::isfinite(x) || !std::isfinite(y))
					return fail(Status::InvalidValue, "PXC appended position exceeds finite bounds");
				node["x"] = x;
				node["y"] = y;
			}
			for (const auto *field : {"inputs", "inspectInputs"})
				Connections(node, field, ids, work, maximumIdBytes);
			if (node.contains("attri") && node["attri"].is_object()) {
				if (GroupType(node)) {
					for (const auto *field : {"custom_input_list", "custom_output_list"}) {
						const auto values = node["attri"].find(field);
						if (values != node["attri"].end() && values->is_array())
							for (auto &id : *values)
								Remap(id, ids, work, maximumIdBytes);
					}
					const auto controls = node["attri"].find("input_display_list");
					if (controls != node["attri"].end() && controls->is_array())
						for (auto &control : *controls)
							if (control.is_object() &&
								control.value("type", JsonString{}) == "Inspector_Sprite")
								Field(control, "node_id", ids, work, maximumIdBytes);
				}
				const auto &type = node.at("type").get_ref<const JsonString &>();
				const bool inlineMembers =
					type == "Node_Collection_Inline" || type == "Node_Collection_Managed" ||
					type == "Node_Iterate_Inline" || type == "Node_Iterate_Each_Inline" ||
					type == "Node_Iterate_Each_File_Inline" || type == "Node_Iterate_Filter_Inline" ||
					type == "Node_Iterate_Sort_Inline" || type == "Node_Iterator_Reduce_Inline" ||
					type == "Node_Smoke_Group_Inline" || type == "Node_VFX_Group_Inline" ||
					type == "Node_VerletSim_Inline" || type == "Node_FLIP_Group_Inline" ||
					type == "Node_Strand_Group_Inline" || type == "Node_Rigid_Group_Inline" ||
					type == "Node_pSystem_Inline" || type == "Node_pSystem_3D_Inline" ||
					type == "Node_MK_Blast_Inline" || type == "Node_MK_Tree_Inline";
				if (inlineMembers) {
					const auto members = node["attri"].find("members");
					if (members != node["attri"].end() && members->is_array())
						for (auto &id : *members)
							Remap(id, ids, work, maximumIdBytes);
				}
			}
			root->at("nodes").push_back(std::move(node));
		}
		// grug APPEND reads timelines, not aRegion, globals or incoming project attributes.
		if (append->contains("timelines")) {
			const auto &timeline = append->at("timelines");
			if (!timeline.is_object() || !timeline.contains("contents") || !timeline["contents"].is_array())
				return fail(Status::InvalidValue, "PXC appended timeline contents are invalid");
			if (!root->contains("timelines"))
				(*root)["timelines"] = AppendJson{{"type", "Folder"}, {"contents", AppendJson::array()}};
			auto &target = (*root)["timelines"];
			if (!target.is_object() || !target.contains("contents") || !target["contents"].is_array())
				return fail(Status::InvalidValue, "PXC destination timeline contents are invalid");
			for (const auto &item : timeline["contents"])
				target["contents"].push_back(item);
		}
		const auto serialized = root->dump();
		if (serialized.size() >= bake::PxcxLimits::MaximumGraphJsonBytes)
			return fail(Status::LimitExceeded, "PXC appended JSON exceeds file bounds");
		const uint64_t publication = ArchiveBytes(destination) * 4 + ArchiveBytes(incoming) * 4 +
									 serialized.size() * 8 +
									 std::max(destinationDomBytes, CodecDomBytes(*root)) + codecFixedBytes;
		if (!budget.Hold(publication))
			return fail(Status::LimitExceeded, "PXC append publication exceeds operation bounds");
		bake::PxcxArchive merged = destination;
		merged.GraphJson.assign(serialized.data(), serialized.size());
		merged.GraphJson.push_back('\0');
		merged.Nodes.clear();
		merged.Links.clear();
		std::vector<std::byte> bytes;
		std::string failure;
		if (!bake::WritePxcx(merged, bytes, failure))
			return fail(Status::InvalidValue, "PXC appended source is invalid");
		bake::PxcxArchive checked;
		if (!bake::ReadPxcx(bytes, checked, failure))
			return fail(Status::InvalidValue, "PXC appended archive failed readback");
		PxcxImportOptions importOptions;
		importOptions.SavePreviewSettings = options.SavePreviewSettings;
		importOptions.MaximumOperationBytes = budget.Available();
		if (!ImportPxcxImageGraph(checked, candidate.Project, failure, importOptions)) {
			diagnostic = {Status::UnsupportedExecution, {}, {}, std::move(failure)};
			return false;
		}
		result = std::move(candidate);
		return true;
	} catch (const AppendWorkExceeded &) {
		diagnostic = {Status::LimitExceeded, {}, {}, "PXC append comparison work exceeds bounds"};
		return false;
	} catch (const std::bad_alloc &) {
		diagnostic = {Status::LimitExceeded, {}, {}, "PXC append allocation exceeds operation bounds"};
		return false;
	} catch (const std::length_error &) {
		diagnostic = {Status::LimitExceeded, {}, {}, "PXC append string or JSON depth exceeds bounds"};
		return false;
	} catch (const AppendJson::exception &) {
		diagnostic = {Status::InvalidValue, {}, {}, "PXC append source record is malformed"};
		return false;
	}

	bool PreparePxcxCollectionLoad(
		const PxcxCollectionSave &files,
		const TimelineSettings &timeline,
		bake::PxcxArchive &result,
		Diagnostic &diagnostic,
		uint64_t maximumBytes
	) try {
		ENGINE_PROFILE("imagegraphio.collection_load");
		diagnostic = {};
		const auto fail = [&](Status code, const char *message) {
			diagnostic = {code, {}, {}, message};
			return false;
		};
		if (!maximumBytes || maximumBytes > Limits::MaximumEvaluationBytes || !ArchiveBounds(result) ||
			files.GraphJson.empty() || files.GraphJson.size() > bake::PxcxLimits::MaximumGraphJsonBytes ||
			(files.MetadataJson && files.MetadataJson->size() > bake::PxcxLimits::MaximumMetadataBytes))
			return fail(Status::LimitExceeded, "PXC collection load counts or bytes exceed bounds");
		if (!timeline.Frames || timeline.Frames > Limits::MaximumTick ||
			!std::isfinite(timeline.FramesPerSecond) || timeline.FramesPerSecond <= 0 ||
			(timeline.Playback != "loop" && timeline.Playback != "stop" && timeline.Playback != "pingpong"))
			return fail(Status::InvalidValue, "PXC collection needs a valid destination timeline");
		detail::ImportBudget budget(maximumBytes);
		if (!budget.Hold(ArchiveBytes(result)) || !budget.Hold(files.GraphJson.capacity() + 1) ||
			(files.MetadataJson && !budget.Hold(files.MetadataJson->capacity() + 1)) ||
			!budget.Hold(timeline.Playback.capacity() + 1))
			return fail(Status::LimitExceeded, "PXC collection load owners exceed allowance");
		JsonScope scope(budget);
		AppendWork work;
		auto graph = ParseText(files.GraphJson, work);
		if (!graph || !graph->contains("nodes") || !graph->at("nodes").is_array() ||
			!graph->contains("version") || graph->at("version") != 121092 || !graph->contains("versionStr") ||
			!graph->at("versionStr").is_string())
			return fail(Status::InvalidValue, "PXC collection JSON or pinned save version is invalid");
		if (graph->at("nodes").size() > Limits::MaximumNodes)
			return fail(Status::LimitExceeded, "PXC collection node count exceeds bounds");
		RecordWork(*graph, work);
		std::optional<AppendJson> metadata;
		if (files.MetadataJson) {
			metadata = ParseText(*files.MetadataJson, work);
			if (!metadata || !metadata->is_object())
				return fail(Status::InvalidValue, "PXC collection metadata is invalid");
			RecordWork(*metadata, work);
			(*graph)["metadata"] = std::move(*metadata);
		}
		size_t keys = 0;
		const auto scaleAnimator = [&](AppendJson &data) {
			if (data.is_object()) {
				if (!data.contains("d"))
					return fail(Status::InvalidValue, "PXC collection compact animator has no value");
				if (++keys > Limits::MaximumKeyframes)
					return fail(Status::LimitExceeded, "PXC collection aggregate keys exceed bounds");
				return true;
			}
			if (!data.is_array())
				return fail(Status::InvalidValue, "PXC collection animator storage is invalid");
			if (data.size() > Limits::MaximumKeyframes - keys)
				return fail(Status::LimitExceeded, "PXC collection aggregate keys exceed bounds");
			keys += data.size();
			for (auto &key : data) {
				work.Charge(1, 1);
				if (!key.is_array() || key.size() < 2)
					return fail(Status::InvalidValue, "PXC collection key record is invalid");
				auto &marker = key[0];
				AppendJson *time = &marker;
				if (marker.is_array()) {
					if (marker.size() < 2 || (marker[0] != 0 && marker[0] != 1))
						return fail(Status::InvalidValue, "PXC collection key marker is unsupported");
					time = &marker[1];
				}
				if (!time->is_number())
					return fail(Status::InvalidValue, "PXC collection normalized key time is invalid");
				const double raw = time->get<double>();
				const double scaled = raw * double(timeline.Frames - 1);
				if (!std::isfinite(raw) || !std::isfinite(scaled) || std::abs(scaled) > Limits::MaximumTick)
					return fail(Status::InvalidValue, "PXC collection scaled key time exceeds bounds");
				const double lower = std::floor(scaled), fraction = scaled - lower;
				// grug GML round chooses the even neighbour on exact halves, including negatives.
				*time = fraction < .5	? lower
						: fraction > .5 ? lower + 1
										: (std::fmod(lower, 2.) == 0 ? lower : lower + 1);
			}
			return true;
		};
		for (auto &node : graph->at("nodes")) {
			if (!node.is_object()) return fail(Status::InvalidValue, "PXC collection node is invalid");
			for (const char *field : {"inputs", "inspectInputs"}) {
				const auto sockets = node.find(field);
				if (sockets == node.end()) continue;
				if (!sockets->is_array())
					return fail(Status::InvalidValue, "PXC collection sockets are invalid");
				for (size_t index = 0; index < sockets->size(); ++index) {
					work.Charge(1, 1);
					if (std::string_view(field) == "inspectInputs" && (index == 3 || index >= 5)) continue;
					auto &socket = (*sockets)[index];
					if (!socket.is_object()) continue;
					for (const char *stored : {"raw_value", "r"}) {
						const auto data = socket.find(stored);
						if (data != socket.end() && !scaleAnimator(*data)) return false;
					}
					const auto axes = socket.find("animators");
					if (axes == socket.end() || !socket.value("sep_axis", false)) continue;
					if (!axes->is_array())
						return fail(Status::InvalidValue, "PXC collection axes are invalid");
					for (auto &axis : *axes)
						if (!scaleAnimator(axis)) return false;
				}
			}
		}

		auto &animator = (*graph)["animator"];
		if (animator.is_null()) animator = AppendJson::object();
		if (!animator.is_object())
			return fail(Status::InvalidValue, "PXC collection animator context is invalid");
		animator["frames_total"] = timeline.Frames;
		animator["framerate"] = timeline.FramesPerSecond;
		animator["playback"] = timeline.Playback == "loop" ? 0 : timeline.Playback == "stop" ? 1 : 2;
		const std::string_view version = {
			graph->at("versionStr").get_ref<const JsonString &>().data(),
			graph->at("versionStr").get_ref<const JsonString &>().size()
		};
		if (version.size() > bake::PxcxLimits::MaximumMetadataBytes)
			return fail(Status::LimitExceeded, "PXC collection version text exceeds bounds");
		const auto text = graph->dump();
		const uint64_t encodedBound = uint64_t(bake::PxcxLimits::MaximumGraphCompressedBytes) +
									  bake::PxcxLimits::MaximumMetadataBytes + 24;
		if (text.size() >= bake::PxcxLimits::MaximumGraphJsonBytes ||
			!budget.Hold(
				text.size() * 4 + version.size() * 4 + encodedBound * 2 + CodecDomBytes(*graph) +
				2 * 1024 * 1024
			))
			return fail(Status::LimitExceeded, "PXC collection checked codec staging exceeds allowance");
		bake::PxcxArchive candidate;
		candidate.MetadataNumber = 121092;
		candidate.MetadataText.assign(version.data(), version.size());
		candidate.GraphJson.assign(text.data(), text.size());
		candidate.GraphJson.push_back('\0');
		std::vector<std::byte> written;
		std::string failure;
		if (!bake::WritePxcx(candidate, written, failure) || !bake::ReadPxcx(written, candidate, failure)) {
			diagnostic = {Status::InvalidValue, {}, {}, std::move(failure)};
			return false;
		}
		result = std::move(candidate);
		return true;
	} catch (const AppendWorkExceeded &) {
		diagnostic = {Status::LimitExceeded, {}, {}, "PXC collection load work exceeds bounds"};
		return false;
	} catch (const std::bad_alloc &) {
		diagnostic = {Status::LimitExceeded, {}, {}, "PXC collection load allocation exceeds allowance"};
		return false;
	} catch (const std::length_error &) {
		diagnostic = {Status::LimitExceeded, {}, {}, "PXC collection load JSON depth exceeds bounds"};
		return false;
	} catch (const AppendJson::exception &) {
		diagnostic = {Status::InvalidValue, {}, {}, "PXC collection load source record is malformed"};
		return false;
	}

	bool PreparePxcxCollectionSave(
		const bake::PxcxArchive &archive,
		std::string_view collectionId,
		std::optional<std::string_view> metadataJson,
		PxcxCollectionSave &result,
		Diagnostic &diagnostic,
		uint64_t maximumBytes
	) try {
		ENGINE_PROFILE("imagegraphio.collection_save");
		const auto fail = [&](Status code, const char *message) {
			diagnostic = {code, {}, {}, message};
			return false;
		};
		if (!maximumBytes || maximumBytes > Limits::MaximumEvaluationBytes || !ArchiveBounds(archive) ||
			archive.Nodes.size() > Limits::MaximumNodes || collectionId.empty() ||
			collectionId.size() > bake::PxcxLimits::MaximumNodeTextBytes ||
			(metadataJson && metadataJson->size() > bake::PxcxLimits::MaximumMetadataBytes) ||
			result.GraphJson.capacity() > bake::PxcxLimits::MaximumGraphJsonBytes ||
			(result.MetadataJson && result.MetadataJson->capacity() > bake::PxcxLimits::MaximumMetadataBytes))
			return fail(Status::LimitExceeded, "PXC collection save counts or bytes exceed bounds");
		if (archive.MetadataNumber != 121092)
			return fail(Status::InvalidValue, "PXC collection save version is invalid");
		detail::ImportBudget budget(maximumBytes);
		if (!budget.Hold(ArchiveBytes(archive)) || !budget.Hold(result.GraphJson.capacity() + 1) ||
			(result.MetadataJson && !budget.Hold(result.MetadataJson->capacity() + 1)) ||
			!budget.Hold(collectionId.size() + (metadataJson ? metadataJson->size() : 0)))
			return fail(Status::LimitExceeded, "PXC collection save owners exceed live bounds");
		JsonScope scope(budget);
		AppendWork work;
		auto graph = Parse(archive, work);
		if (!graph) return fail(Status::InvalidValue, "PXC collection source JSON is invalid");
		RecordWork(*graph, work);
		const uint64_t validationBytes =
			ArchiveBytes(archive) * 4 + CodecDomBytes(*graph) + 2ull * 1024 * 1024;
		if (!budget.Hold(validationBytes))
			return fail(Status::LimitExceeded, "PXC collection archive validation exceeds live bounds");
		{
			std::vector<std::byte> checked;
			std::string failure;
			if (!bake::WritePxcx(archive, checked, failure) || checked != archive.OriginalBytes)
				return fail(Status::InvalidValue, "PXC collection archive has untracked changes");
		}
		budget.Release(validationBytes);
		const auto &nodes = graph->at("nodes");
		const size_t count = nodes.size();
		if (count > Limits::MaximumNodes)
			return fail(Status::LimitExceeded, "PXC collection node count exceeds bounds");
		struct NodeIndex {
			std::string_view Id;
			size_t Index = 0;
		};
		struct Visit {
			size_t Index = 0, Next = 0;
		};
		struct Traversal {
			std::array<NodeIndex, Limits::MaximumNodes> Index;
			std::array<size_t, Limits::MaximumNodes> First, Last, Next;
			std::array<bool, Limits::MaximumNodes> Visited{};
			std::array<Visit, Limits::MaximumNodes> Stack;
		};
		if (!budget.Hold(sizeof(Traversal)))
			return fail(Status::LimitExceeded, "PXC collection traversal exceeds live bounds");
		auto traversalStorage = std::make_unique<Traversal>();
		auto &traversal = *traversalStorage;
		traversal.First.fill(count);
		traversal.Last.fill(count);
		traversal.Next.fill(count);
		for (size_t index = 0; index < count; ++index) {
			const auto &node = nodes[index];
			if (!node.is_object() || !node.contains("id") || !node["id"].is_string())
				return fail(Status::InvalidValue, "PXC collection node identity is invalid");
			const auto &id = node["id"].get_ref<const JsonString &>();
			if (id.empty() || id.size() > bake::PxcxLimits::MaximumNodeTextBytes)
				return fail(Status::InvalidValue, "PXC collection node identity is outside bounds");
			traversal.Index[index] = {{id.data(), id.size()}, index};
		}
		const auto compare = [&](const NodeIndex &a, const NodeIndex &b) {
			work.Charge(1, std::min(a.Id.size(), b.Id.size()) + 1);
			return a.Id < b.Id;
		};
		std::sort(traversal.Index.begin(), traversal.Index.begin() + count, compare);
		for (size_t index = 1; index < count; ++index)
			if (!compare(traversal.Index[index - 1], traversal.Index[index]))
				return fail(Status::InvalidValue, "PXC collection node identity is duplicated");
		const auto find = [&](std::string_view id) {
			const auto found = std::lower_bound(
				traversal.Index.begin(), traversal.Index.begin() + count, NodeIndex{id, 0}, compare
			);
			work.Charge(1, id.size() + 1);
			return found != traversal.Index.begin() + count && found->Id == id ? found->Index : count;
		};
		const size_t selected = find(collectionId);
		if (selected == count || !GroupType(nodes[selected]))
			return fail(Status::InvalidValue, "select a source Collection to save");
		const auto position = [&](const AppendJson &node, const char *name) -> std::optional<double> {
			const auto value = node.find(name);
			if (value == node.end() || !value->is_number()) return {};
			const double number = value->get<double>();
			return std::isfinite(number) ? std::optional<double>{number} : std::nullopt;
		};
		const auto originX = position(nodes[selected], "x"), originY = position(nodes[selected], "y");
		if (!originX || !originY)
			return fail(Status::InvalidValue, "PXC collection has no finite saved position");
		for (size_t index = 0; index < count; ++index) {
			const auto parent = nodes[index].find("group");
			if (parent == nodes[index].end() || !parent->is_string()) continue;
			const auto &parentName = parent->get_ref<const JsonString &>();
			const size_t owner = find({parentName.data(), parentName.size()});
			if (owner == count || !GroupType(nodes[owner])) continue;
			if (traversal.First[owner] == count)
				traversal.First[owner] = index;
			else
				traversal.Next[traversal.Last[owner]] = index;
			traversal.Last[owner] = index;
		}
		std::optional<double> frameCount;
		const auto animator = graph->find("animator");
		if (animator != graph->end() && animator->is_object()) {
			const auto total = animator->find("frames_total");
			if (total != animator->end() && total->is_number()) {
				const double value = total->get<double>();
				if (std::isfinite(value) && value > 1 && value <= Limits::MaximumTick &&
					std::trunc(value) == value)
					frameCount = value;
			}
		}
		size_t keyCount = 0;
		const auto normalizeAnimator = [&](AppendJson &data, bool canCompact) {
			if (data.is_object()) {
				if (data.contains("d") && ++keyCount > Limits::MaximumKeyframes)
					return fail(Status::LimitExceeded, "PXC collection aggregate key count exceeds bounds");
				return true;
			}
			if (!data.is_array())
				return fail(Status::InvalidValue, "PXC collection animator storage is invalid");
			if (data.size() > Limits::MaximumKeyframes - keyCount)
				return fail(Status::LimitExceeded, "PXC collection aggregate key count exceeds bounds");
			keyCount += data.size();
			if (canCompact && data.size() == 1) {
				const auto &key = data.front();
				if (key.is_array() && key.size() >= 2 && key.size() <= 9 && key[0].is_array() &&
					key[0].size() >= 2 && key[0][0].is_number() && key[0][1].is_number() &&
					std::isfinite(key[0][1].get<double>()) &&
					((key[0][0] == 0 && key[0].size() == 2) ||
					 (key[0][0] == 1 && (key[0].size() == 2 || (key[0].size() == 3 && key[0][2] == 0)))) &&
					(key.size() <= 7 || !key[7].is_object())) {
					// grug source drops a lone nontrigger key's time and known ease/color fields.
					// opaque tails remain expanded; their meaning cannot be inferred from value type.
					RecordWork(key[1], work);
					AppendJson compact = {{"d", key[1]}};
					data = std::move(compact);
					return true;
				}
			}
			for (auto &key : data) {
				work.Charge(1, 1);
				if (!key.is_array() || key.size() < 2 || !key[0].is_array() || key[0].size() < 2 ||
					!key[0][0].is_number() || !key[0][1].is_number() || (key[0][0] != 0 && key[0][0] != 1))
					return fail(Status::InvalidValue, "PXC collection key marker is invalid");
				if (!frameCount)
					return fail(
						Status::InvalidValue,
						"PXC collection keyed save needs a checked project frame count greater than one"
					);
				const double time = key[0][1].get<double>() / (*frameCount - 1);
				if (!std::isfinite(time))
					return fail(Status::InvalidValue, "PXC collection scaled key time is invalid");
				key[0][1] = time;
			}
			return true;
		};
		const auto normalizeInputs = [&](AppendJson &record) {
			const CatalogueEntry *entry = nullptr;
			const auto type = record.find("type");
			if (type != record.end() && type->is_string()) {
				const auto &name = type->get_ref<const JsonString &>();
				work.Charge(1, name.size() + 1);
				entry = FindCatalogueSource({name.data(), name.size()});
			}
			for (const char *field : {"inputs", "inspectInputs"}) {
				const auto inputs = record.find(field);
				if (inputs == record.end()) continue;
				if (!inputs->is_array())
					return fail(Status::InvalidValue, "PXC collection socket list is invalid");
				for (size_t index = 0; index < inputs->size(); ++index) {
					work.Charge(1, 1);
					// grug source output trigger and surplus inspector fields have no saved animator.
					if (std::string_view(field) == "inspectInputs" && (index == 3 || index >= 5)) continue;
					bool canCompact = std::string_view(field) == "inspectInputs" && index != 2;
					if (std::string_view(field) == "inputs" && entry && index <= INT32_MAX) {
						work.Charge(1, entry->Inputs.size() + 1);
						const auto *declared = FindCatalogueInputIndex(*entry, static_cast<int32_t>(index));
						// grug Trigger is boolean in native schemas. constructor kind owns this rule.
						canCompact =
							declared && declared->SourceKind != "Trigger" && declared->Type != ValueType::Any;
					}
					auto &input = (*inputs)[index];
					if (!input.is_object())
						return fail(Status::InvalidValue, "PXC collection socket record is invalid");
					const auto data = input.find("r");
					if (data != input.end() && !normalizeAnimator(*data, canCompact)) return false;
					const auto axes = input.find("animators");
					if (axes == input.end()) continue;
					if (!axes->is_array())
						return fail(Status::InvalidValue, "PXC collection axis storage is invalid");
					for (auto &axis : *axes)
						if (!normalizeAnimator(axis, canCompact)) return false;
				}
			}
			return true;
		};
		constexpr std::string_view versionText = "1.21.10.203";
		AppendJson content = {
			{"version", 121092}, {"versionStr", versionText}, {"nodes", AppendJson::array()}
		};
		size_t depth = 1;
		traversal.Visited[selected] = true;
		traversal.Stack[0] = {selected, traversal.First[selected]};
		while (depth) {
			work.Charge(1, 1);
			auto &frame = traversal.Stack[depth - 1];
			if (frame.Next != count) {
				const size_t child = frame.Next;
				frame.Next = traversal.Next[child];
				if (traversal.Visited[child] || depth == traversal.Stack.size())
					return fail(Status::InvalidValue, "PXC collection contains cyclic source groups");
				traversal.Visited[child] = true;
				traversal.Stack[depth++] = {child, GroupType(nodes[child]) ? traversal.First[child] : count};
				continue;
			}
			const auto x = position(nodes[frame.Index], "x"), y = position(nodes[frame.Index], "y");
			if (!x || !y || !std::isfinite(*x - *originX) || !std::isfinite(*y - *originY))
				return fail(Status::InvalidValue, "PXC collection saved position exceeds finite bounds");
			RecordWork(nodes[frame.Index], work);
			AppendJson record = nodes[frame.Index];
			record["version"] = 121092;
			record["x"] = *x - *originX;
			record["y"] = *y - *originY;
			// grug SAVE_COLLECTION passes selected parent as context; GameMaker noone is -4.
			if (frame.Index == selected && record.contains("group")) {
				const auto &parent = record["group"];
				if (parent.is_string() && !parent.get_ref<const JsonString &>().empty())
					record["group"] = -4;
				else if (parent.is_null() || parent.is_string() ||
						 (parent.is_number_integer() &&
						  (parent.get<int64_t>() == -1 || parent.get<int64_t>() == -4)))
					record.erase("group");
				else
					return fail(Status::InvalidValue, "PXC collection saved parent is invalid");
			}
			if (!normalizeInputs(record)) return false;
			content["nodes"].push_back(std::move(record));
			--depth;
		}
		std::optional<AppendJson> manager;
		if (metadataJson) {
			auto supplied = ParseText(*metadataJson, work);
			if (!supplied) return fail(Status::InvalidValue, "PXC collection manager JSON is invalid");
			RecordWork(*supplied, work);
			manager = CollectionDefaults();
			for (auto &field : manager->items()) {
				const auto value = supplied->find(field.key());
				if (value != supplied->end() && !value->is_null()) field.value() = *value;
			}
			(*manager)["version"] = 121092;
			(*manager)["versionStr"] = versionText;
		}
		const auto serialized = content.dump();
		if (serialized.size() > bake::PxcxLimits::MaximumGraphJsonBytes ||
			!budget.Hold(2 * std::max(serialized.size(), std::string{}.capacity()) + 1))
			return fail(Status::LimitExceeded, "PXC collection text exceeds live bounds");
		PxcxCollectionSave candidate;
		candidate.GraphJson.assign(serialized.data(), serialized.size());
		if (manager) {
			const auto metadata = manager->dump();
			if (metadata.size() > bake::PxcxLimits::MaximumMetadataBytes ||
				!budget.Hold(2 * std::max(metadata.size(), std::string{}.capacity()) + 1))
				return fail(Status::LimitExceeded, "PXC collection sidecar exceeds live bounds");
			candidate.MetadataJson.emplace(metadata.data(), metadata.size());
		}
		static_assert(std::is_nothrow_move_assignable_v<PxcxCollectionSave>);
		result = std::move(candidate);
		diagnostic = {};
		return true;
	} catch (const AppendWorkExceeded &) {
		diagnostic = {Status::LimitExceeded, {}, {}, "PXC collection comparison work exceeds bounds"};
		return false;
	} catch (const std::bad_alloc &) {
		diagnostic = {Status::LimitExceeded, {}, {}, "PXC collection allocation exceeds operation bounds"};
		return false;
	} catch (const std::length_error &) {
		diagnostic = {Status::LimitExceeded, {}, {}, "PXC collection string or JSON depth exceeds bounds"};
		return false;
	} catch (const AppendJson::exception &) {
		diagnostic = {Status::InvalidValue, {}, {}, "PXC collection source record is malformed"};
		return false;
	}

	bool PreparePxcxCollectionMetadata(
		const bake::PxcxArchive &archive,
		std::vector<PxcxCollectionMetadata> &result,
		Diagnostic &diagnostic,
		uint64_t maximumBytes,
		bool applyRootMetadata
	) try {
		ENGINE_PROFILE("imagegraphio.collection_metadata_defaults");
		const auto fail = [&](Status code, const char *message) {
			diagnostic = {code, {}, {}, message};
			return false;
		};
		if (!maximumBytes || maximumBytes > Limits::MaximumEvaluationBytes || !ArchiveBounds(archive) ||
			archive.Nodes.size() > Limits::MaximumNodes || result.size() > Limits::MaximumNodes ||
			result.capacity() > Limits::MaximumNodes)
			return fail(Status::LimitExceeded, "PXC collection metadata counts or bytes exceed bounds");
		if (archive.MetadataNumber != 121092)
			return fail(Status::InvalidValue, "PXC collection metadata save version is invalid");
		detail::ImportBudget budget(maximumBytes);
		if (!budget.Hold(ArchiveBytes(archive)) ||
			!budget.Hold(result.capacity() * sizeof(PxcxCollectionMetadata)))
			return fail(Status::LimitExceeded, "PXC collection metadata owners exceed live bounds");
		for (const auto &manager : result) {
			if (manager.NodeId.size() > bake::PxcxLimits::MaximumNodeTextBytes ||
				manager.MetadataJson.size() > bake::PxcxLimits::MaximumMetadataBytes ||
				!budget.Hold(manager.NodeId.capacity() + manager.MetadataJson.capacity()))
				return fail(Status::LimitExceeded, "PXC prior collection metadata exceeds live bounds");
		}
		JsonScope scope(budget);
		AppendWork work;
		auto graph = Parse(archive, work);
		if (!graph) return fail(Status::InvalidValue, "PXC collection metadata source JSON is invalid");
		RecordWork(*graph, work);
		const uint64_t domBytes = CodecDomBytes(*graph);
		constexpr uint64_t codecFixedBytes = 2ull * 1024 * 1024;
		const uint64_t validationBytes = ArchiveBytes(archive) * 4 + domBytes + codecFixedBytes;
		if (!budget.Hold(validationBytes))
			return fail(
				Status::LimitExceeded, "PXC collection metadata archive validation exceeds live bounds"
			);
		{
			std::vector<std::byte> checked;
			std::string failure;
			if (!bake::WritePxcx(archive, checked, failure) || checked != archive.OriginalBytes)
				return fail(Status::InvalidValue, "PXC collection metadata archive has untracked changes");
		}
		budget.Release(validationBytes);
		AppendJson defaults = CollectionDefaults();
		AppendJson rootDefaults = defaults;
		if (applyRootMetadata && graph->contains("metadata")) {
			const auto &metadata = graph->at("metadata");
			if (!metadata.is_object())
				return fail(Status::InvalidValue, "PXC collection root metadata is invalid");
			for (auto &field : rootDefaults.items()) {
				const auto value = metadata.find(field.key());
				if (value != metadata.end() && !value->is_null()) field.value() = *value;
			}
		}
		const auto defaultMetadataJson = defaults.dump();
		const auto rootMetadataJson = rootDefaults.dump();
		const auto &metadataJson = defaultMetadataJson;
		if (metadataJson.size() > bake::PxcxLimits::MaximumMetadataBytes ||
			rootMetadataJson.size() > bake::PxcxLimits::MaximumMetadataBytes)
			return fail(Status::LimitExceeded, "PXC collection metadata exceeds field bounds");
		size_t collectionCount = 0;
		for (const auto &node : graph->at("nodes"))
			if (GroupType(node) && ++collectionCount > Limits::MaximumNodes)
				throw std::length_error("collection count");
		std::vector<PxcxCollectionMetadata> candidate;
		if (!budget.Hold(collectionCount * sizeof(PxcxCollectionMetadata)))
			return fail(Status::LimitExceeded, "PXC collection metadata table exceeds live bounds");
		candidate.reserve(collectionCount);
		if (!budget.Hold((candidate.capacity() - collectionCount) * sizeof(PxcxCollectionMetadata)))
			return fail(Status::LimitExceeded, "PXC collection metadata table capacity exceeds live bounds");
		for (const auto &node : graph->at("nodes")) {
			if (GroupType(node)) {
				const auto parent = node.find("group");
				const bool topLevel =
					parent == node.end() || parent->is_null() ||
					(parent->is_number_integer() && (*parent == -1 || *parent == -4)) ||
					(parent->is_string() && parent->template get_ref<const JsonString &>().empty());
				const auto &metadataJson =
					applyRootMetadata && topLevel ? rootMetadataJson : defaultMetadataJson;
				const auto id = node.find("id");
				if (id == node.end() || !id->is_string()) throw std::invalid_argument("collection identity");
				const auto &nodeId = id->template get_ref<const JsonString &>();
				if (nodeId.empty() || nodeId.size() > bake::PxcxLimits::MaximumNodeTextBytes ||
					candidate.size() == collectionCount)
					throw std::length_error("collection identity bounds");
				work.Charge(1, nodeId.size() + metadataJson.size() + 2);
				const uint64_t copyBytes = std::max(nodeId.size(), std::string{}.capacity()) + 1 +
										   std::max(metadataJson.size(), std::string{}.capacity()) + 1;
				if (!budget.Hold(copyBytes)) throw std::bad_alloc{};
				candidate.push_back(
					{{nodeId.data(), nodeId.size()}, {metadataJson.data(), metadataJson.size()}}
				);
			}
		}
		static_assert(std::is_nothrow_move_assignable_v<std::vector<PxcxCollectionMetadata>>);
		result = std::move(candidate);
		diagnostic = {};
		return true;
	} catch (const AppendWorkExceeded &) {
		diagnostic = {
			Status::LimitExceeded, {}, {}, "PXC collection metadata comparison work exceeds bounds"
		};
		return false;
	} catch (const std::bad_alloc &) {
		diagnostic = {
			Status::LimitExceeded, {}, {}, "PXC collection metadata allocation exceeds live bounds"
		};
		return false;
	} catch (const std::length_error &) {
		diagnostic = {Status::LimitExceeded, {}, {}, "PXC collection metadata counts exceed bounds"};
		return false;
	} catch (const std::invalid_argument &) {
		diagnostic = {Status::InvalidValue, {}, {}, "PXC collection metadata identity is invalid"};
		return false;
	} catch (const AppendJson::exception &) {
		diagnostic = {Status::InvalidValue, {}, {}, "PXC collection metadata source record is malformed"};
		return false;
	}

	bool PreparePxcxAppendPostLoad(
		const PxcxAppendResult &append,
		std::string_view sourcePath,
		PxcxAppendPostLoad &result,
		Diagnostic &diagnostic,
		uint64_t maximumBytes
	) try {
		ENGINE_PROFILE("imagegraphio.source_append_post_load");
		const auto fail = [&](Status code, const char *message) {
			diagnostic = {code, {}, {}, message};
			return false;
		};
		if (!maximumBytes || maximumBytes > Limits::MaximumEvaluationBytes ||
			append.Nodes.size() > Limits::MaximumNodes || result.Collections.size() > Limits::MaximumNodes ||
			append.MetadataJson.size() > bake::PxcxLimits::MaximumMetadataBytes ||
			sourcePath.size() > bake::PxcxLimits::MaximumNodeTextBytes)
			return fail(Status::LimitExceeded, "PXC append post-load counts or bytes exceed bounds");
		if (sourcePath.find('\0') != std::string_view::npos || append.Project.Source.MetadataNumber != 121092)
			return fail(Status::InvalidValue, "PXC append post-load path or save version is invalid");
		detail::ImportBudget budget(maximumBytes);
		const auto sourceBytes = ImportBytes(append.Project);
		if (!sourceBytes || !budget.Hold(*sourceBytes) || !budget.Hold(append.MetadataJson.capacity()) ||
			!budget.Hold(sourcePath.size()) ||
			!budget.Hold(append.Nodes.capacity() * sizeof(PxcxAppendedNode)) ||
			!budget.Hold(result.Collections.capacity() * sizeof(PxcxCollectionMetadata)))
			return fail(Status::LimitExceeded, "PXC append post-load owners exceed live bounds");
		size_t maximumIdentityBytes = 0;
		for (const auto &node : append.Nodes) {
			maximumIdentityBytes = std::max({maximumIdentityBytes, node.NodeId.size(), node.SourceId.size()});
			if (node.NodeId.empty() || node.SourceId.empty() ||
				node.NodeId.size() > bake::PxcxLimits::MaximumNodeTextBytes ||
				node.SourceId.size() > bake::PxcxLimits::MaximumNodeTextBytes ||
				!budget.Hold(node.NodeId.capacity() + node.SourceId.capacity()))
				return fail(
					Status::InvalidValue, "PXC append post-load node identity is invalid or too large"
				);
		}
		for (const auto &manager : result.Collections)
			if (manager.NodeId.size() > bake::PxcxLimits::MaximumNodeTextBytes ||
				manager.MetadataJson.size() > bake::PxcxLimits::MaximumMetadataBytes ||
				!budget.Hold(manager.NodeId.capacity() + manager.MetadataJson.capacity()))
				return fail(Status::LimitExceeded, "PXC append prior metadata exceeds live bounds");
		if (result.Source && (!ArchiveBounds(*result.Source) || !budget.Hold(ArchiveBytes(*result.Source))))
			return fail(Status::LimitExceeded, "PXC append prior post-load archive exceeds live bounds");
		JsonScope scope(budget);
		AppendWork work;
		auto graph = Parse(append.Project.Source, work);
		if (!graph) return fail(Status::InvalidValue, "PXC append post-load source JSON is invalid");
		RecordWork(*graph, work);
		const uint64_t originalDomBytes = CodecDomBytes(*graph);
		constexpr uint64_t codecFixedBytes = 2ull * 1024 * 1024;
		const auto &source = append.Project.Source;
		const uint64_t validationBytes = ArchiveBytes(source) * 4 + originalDomBytes + codecFixedBytes;
		if (!budget.Hold(validationBytes))
			return fail(Status::LimitExceeded, "PXC append post-load source validation exceeds live bounds");
		{
			std::vector<std::byte> checked;
			std::string failure;
			if (!bake::WritePxcx(source, checked, failure) || checked != source.OriginalBytes)
				return fail(Status::InvalidValue, "PXC append post-load source has untracked changes");
		}
		budget.Release(validationBytes);
		AppendJson metadata = AppendJson::object();
		if (!append.MetadataJson.empty()) {
			auto parsed = ParseText(append.MetadataJson, work);
			if (!parsed)
				return fail(Status::InvalidValue, "PXC append root metadata is invalid or ambiguous");
			RecordWork(*parsed, work);
			metadata = std::move(*parsed);
		}
		AppendJson defaults = CollectionDefaults();
		// grug source deserialize keeps constructor values for missing or null fields.
		for (auto &field : defaults.items()) {
			const auto value = metadata.find(field.key());
			if (value != metadata.end() && !value->is_null()) field.value() = *value;
		}
		const auto managerJson = defaults.dump();
		if (managerJson.size() > bake::PxcxLimits::MaximumMetadataBytes)
			return fail(Status::LimitExceeded, "PXC append metadata manager exceeds field bounds");
		PxcxAppendPostLoad candidate;
		if (!budget.Hold(append.Nodes.size() * sizeof(PxcxCollectionMetadata)))
			return fail(Status::LimitExceeded, "PXC append metadata manager table exceeds live bounds");
		candidate.Collections.reserve(append.Nodes.size());
		if (!budget.Hold(
				(candidate.Collections.capacity() - append.Nodes.size()) * sizeof(PxcxCollectionMetadata)
			))
			return fail(Status::LimitExceeded, "PXC append metadata manager capacity exceeds live bounds");
		AppendJson seen = AppendJson::object(), sourceIds = AppendJson::object();
		AppendJson *singleTopLevel = nullptr;
		size_t topLevelCount = 0;
		for (const auto &loaded : append.Nodes) {
			work.Charge(seen.size() * 2 + sourceIds.size() * 2 + 1, maximumIdentityBytes + 1);
			JsonString nodeId(loaded.NodeId.data(), loaded.NodeId.size());
			JsonString sourceId(loaded.SourceId.data(), loaded.SourceId.size());
			if (seen.contains(nodeId) || sourceIds.contains(sourceId))
				return fail(Status::InvalidValue, "PXC append post-load identities are repeated");
			seen[nodeId] = true;
			sourceIds[sourceId] = true;
			const auto node =
				std::find_if(graph->at("nodes").begin(), graph->at("nodes").end(), [&](const auto &record) {
					if (!record.is_object() || !record.contains("id") || !record["id"].is_string())
						return false;
					const auto &recordId = record["id"].template get_ref<const JsonString &>();
					work.Charge(1, std::max(recordId.size(), loaded.NodeId.size()) + 1);
					return recordId == nodeId;
				});
			if (node == graph->at("nodes").end())
				return fail(Status::InvalidValue, "PXC append post-load node is missing from source");
			if (!loaded.TopLevel) continue;
			++topLevelCount;
			singleTopLevel = &*node;
			if (!GroupType(*node)) continue;
			work.Charge(1, loaded.NodeId.size() + managerJson.size() + 2);
			const uint64_t managerBytes = std::max(loaded.NodeId.size(), std::string{}.capacity()) + 1 +
										  std::max(managerJson.size(), std::string{}.capacity()) + 1;
			if (!budget.Hold(managerBytes))
				return fail(Status::LimitExceeded, "PXC append metadata manager copies exceed live bounds");
			candidate.Collections.push_back({loaded.NodeId, {managerJson.data(), managerJson.size()}});
		}
		if (topLevelCount == 1 && singleTopLevel && GroupType(*singleTopLevel) && !sourcePath.empty()) {
			if (!singleTopLevel->contains("attri")) (*singleTopLevel)["attri"] = AppendJson::object();
			auto &attributes = singleTopLevel->at("attri");
			if (!attributes.is_object())
				return fail(Status::InvalidValue, "PXC appended Collection attributes are malformed");
			JsonString path(sourcePath.data(), sourcePath.size());
			const auto oldPath = attributes.find("path");
			if (oldPath == attributes.end() || *oldPath != path) {
				attributes["path"] = std::move(path);
				const auto serialized = graph->dump();
				if (serialized.size() >= bake::PxcxLimits::MaximumGraphJsonBytes)
					return fail(
						Status::LimitExceeded, "PXC append post-load source JSON exceeds file bounds"
					);
				const uint64_t publicationBytes = ArchiveBytes(source) * 4 + serialized.size() * 8 +
												  std::max(originalDomBytes, CodecDomBytes(*graph)) +
												  codecFixedBytes;
				if (!budget.Hold(publicationBytes))
					return fail(
						Status::LimitExceeded, "PXC append post-load publication exceeds live bounds"
					);
				bake::PxcxArchive updated = source;
				updated.GraphJson.assign(serialized.data(), serialized.size());
				updated.GraphJson.push_back('\0');
				updated.Nodes.clear();
				updated.Links.clear();
				std::vector<std::byte> bytes;
				std::string failure;
				if (!bake::WritePxcx(updated, bytes, failure))
					return fail(Status::InvalidValue, "PXC append post-load source write refused");
				bake::PxcxArchive checked;
				if (!bake::ReadPxcx(bytes, checked, failure))
					return fail(Status::InvalidValue, "PXC append post-load source readback refused");
				candidate.Source.emplace(std::move(checked));
			}
		}
		static_assert(std::is_nothrow_move_assignable_v<PxcxAppendPostLoad>);
		result = std::move(candidate);
		diagnostic = {};
		return true;
	} catch (const AppendWorkExceeded &) {
		diagnostic = {Status::LimitExceeded, {}, {}, "PXC append post-load comparison work exceeds bounds"};
		return false;
	} catch (const std::bad_alloc &) {
		diagnostic = {Status::LimitExceeded, {}, {}, "PXC append post-load allocation exceeds live bounds"};
		return false;
	} catch (const std::length_error &) {
		diagnostic = {
			Status::LimitExceeded, {}, {}, "PXC append post-load string or JSON depth exceeds bounds"
		};
		return false;
	} catch (const AppendJson::exception &) {
		diagnostic = {Status::InvalidValue, {}, {}, "PXC append post-load source record is malformed"};
		return false;
	}
}
