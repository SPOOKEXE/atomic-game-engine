#pragma once

#include "ImageGraphAppendGroups.hpp"
#include "ImageGraphCacheEditObservation.hpp"
#include "ImageGraphHistoryCanvas.hpp"
#include "ImageGraphHistorySource.hpp"
#include "ImageGraphHost.hpp"

#include <engine/imagegraphio/PxcxStructureEdit.hpp>

#include <memory>

namespace studio::detail {
	// grug borrow immutable arguments; file observations remain private to this append.
	struct ImageGraphAppendHost final : engine::imagegraph::HostNodeProvider {
		ImageGraphHost Files;
		engine::imagegraph::SourceArgumentHost *Arguments = nullptr;
		bool Capture(
			const engine::imagegraph::HostNodeInvocation &invocation,
			engine::imagegraph::HostNodeCapture &output,
			std::string &failure
		) override {
			if (invocation.Authored.Type == "pc.argument" && Arguments)
				return Arguments->Capture(invocation, output, failure);
			return Files.Capture(invocation, output, failure);
		}
	};
	inline std::optional<uint64_t>
	ImageGraphCollectionBytes(const std::vector<engine::imagegraphio::PxcxCollectionMetadata> &values) {
		const auto *items = &values;
		using namespace engine::imagegraph;
		if (items->size() > Limits::MaximumNodes ||
			items->capacity() >
				Limits::MaximumEvaluationBytes / sizeof(ImageGraphHistory::CollectionMetadata))
			return {};
		uint64_t bytes = items->capacity() * sizeof(ImageGraphHistory::CollectionMetadata);
		for (const auto &item : *items)
			for (const auto *text : {&item.NodeId, &item.MetadataJson}) {
				if (text->capacity() > Limits::MaximumEvaluationBytes - bytes) return {};
				bytes += text->capacity();
			}
		return bytes;
	}

	inline std::optional<uint64_t>
	ImageGraphCollectionBytes(const ImageGraphHistory::CollectionSnapshot &items) {
		return items ? ImageGraphCollectionBytes(*items) : std::optional<uint64_t>{0};
	}

	// grug reserve conservative writer scratch before the legacy adapter clones source DOMs.
	// keep source writing and checked decode private; large projects refuse before writer entry.
	inline bool PrepareImageGraphAppendDestination(
		const engine::imagegraphio::PxcxImport &imported,
		const engine::imagegraph::Document &saved,
		engine::imagegraph::FrameTime frame,
		engine::bake::PxcxArchive &destination,
		engine::imagegraph::Diagnostic &diagnostic,
		uint64_t maximumBytes = engine::imagegraph::Limits::MaximumEvaluationBytes
	) try {
		using namespace engine::imagegraph;
		uint64_t remaining = maximumBytes;
		const auto charge = [&](std::optional<uint64_t> bytes, uint64_t copies = 1) {
			if (!bytes || !copies || *bytes >= remaining / copies) return false;
			remaining -= *bytes * copies;
			return true;
		};
		if (!maximumBytes || maximumBytes > Limits::MaximumEvaluationBytes ||
			!charge(ImageGraphHistoryArchiveBytes(destination)) ||
			!charge(ImageGraphHistoryArchiveBytes(imported.Source), 16) ||
			!charge(DocumentRetainedPayloadBytes(imported.Graph), 16) ||
			!charge(DocumentRetainedPayloadBytes(saved), 16) ||
			(imported.GroupPrebinding && !charge(DocumentRetainedPayloadBytes(*imported.GroupPrebinding))) ||
			!charge(ImageGraphHistoryDiagnosticBytes(imported.Diagnostics)) ||
			!charge(imported.Source.GraphJson.capacity(), 512) || !charge(4ull * 1024 * 1024)) {
			diagnostic = {
				Status::LimitExceeded, {}, {}, "Studio append source writer scratch exceeds allowance"
			};
			return false;
		}
		std::vector<std::byte> bytes;
		if (!engine::imagegraphio::WritePxcxProjection(imported, saved, frame, bytes, diagnostic))
			return false;
		engine::bake::PxcxArchive checked;
		std::string error;
		if (!engine::bake::ReadPxcx(bytes, checked, error)) {
			diagnostic = {Status::InvalidValue, {}, {}, std::move(error)};
			return false;
		}
		destination = std::move(checked);
		diagnostic = {};
		return true;
	} catch (const std::bad_alloc &) {
		diagnostic = {
			engine::imagegraph::Status::LimitExceeded,
			{},
			{},
			"Studio append source writer allocation refused"
		};
		return false;
	}

	// grug stage source, callbacks, managers and canvas before cache/history admission.
	// caller supplies a checked destination containing unsaved edits, and a prepared live Group host.
	struct ImageGraphAppendProject {
		engine::bake::PxcxArchive Source;
		engine::imagegraph::Document Projection;
		std::vector<engine::imagegraph::Diagnostic> Diagnostics;
		ImageGraphAppendGroups Groups;
		nodegraph::Graph Graph;
		ImageGraphCanvasIds Ids;
		std::vector<nodegraph::NodeId> Selection;
		// grug empty Collections live in the Groups inspector, without a fake canvas node.
		std::vector<std::string> SelectedGroups;
		ImageGraphCacheEditObservation Observation;
		ImageGraphHistory::CollectionSnapshot Collections;
		ImageGraphHistory::SourceSnapshot BeforeSource, AfterSource;
		std::vector<std::string> LoadedCacheOwners;
		uint64_t Remaining = 0;

		bool Prepare(
			const engine::bake::PxcxArchive &baseline,
			const engine::bake::PxcxArchive &destination,
			const engine::bake::PxcxArchive &incoming,
			engine::imagegraphio::PxcxAppendOptions options,
			std::string_view sourcePath,
			const engine::imagegraph::Document &live,
			const ImageGraphGroupHost &previous,
			const ImageGraphHistory::CollectionSnapshot &collections,
			const ImageGraphCanvasIds &oldIds,
			uint64_t revision,
			engine::imagegraph::EvaluationRequest clock,
			engine::imagegraph::Diagnostic &diagnostic,
			uint64_t maximumBytes = engine::imagegraph::Limits::MaximumEvaluationBytes
		) try {
			using namespace engine::imagegraph;
			using namespace engine::imagegraphio;
			ENGINE_PROFILE("studio.imagegraph.append_project");
			const auto fail = [&](Status status, const char *message) {
				diagnostic = {status, {}, {}, message};
				return false;
			};
			if (Remaining || !maximumBytes || maximumBytes > Limits::MaximumEvaluationBytes)
				return fail(Status::LimitExceeded, "Studio append candidate or allowance is invalid");
			uint64_t remaining = maximumBytes;
			const auto charge = [&](std::optional<uint64_t> bytes) {
				if (!bytes || *bytes >= remaining) return false;
				remaining -= *bytes;
				return true;
			};
			if (!charge(ImageGraphHistoryArchiveBytes(baseline)) ||
				(&baseline != &destination && !charge(ImageGraphHistoryArchiveBytes(destination))) ||
				!charge(ImageGraphHistoryArchiveBytes(incoming)) ||
				!charge(ImageGraphCollectionBytes(collections)))
				return fail(Status::LimitExceeded, "Studio append source owners exceed live bytes");
			PxcxAppendResult append;
			options.MaximumOperationBytes = std::min(options.MaximumOperationBytes, remaining);
			if (!AppendPxcxProject(destination, incoming, options, append, diagnostic)) return false;
			// grug reserve import storage before callbacks borrow it.
			if (!charge(ImageGraphHistoryArchiveBytes(append.Project.Source)) ||
				!charge(DocumentRetainedPayloadBytes(append.Project.Graph)) ||
				(append.Project.GroupPrebinding &&
				 !charge(DocumentRetainedPayloadBytes(*append.Project.GroupPrebinding))) ||
				!charge(ImageGraphHistoryDiagnosticBytes(append.Project.Diagnostics)))
				return fail(Status::LimitExceeded, "Studio append import exceeds live bytes");
			uint64_t metadataBytes =
				append.MetadataJson.capacity() + append.Nodes.capacity() * sizeof(PxcxAppendedNode) +
				append.Project.GroupBootstrap.capacity() * sizeof(PxcxGroupBootstrapRecord) +
				append.Project.GroupBindings.capacity() * sizeof(GroupSubtypeBinding);
			for (const auto &node : append.Nodes)
				metadataBytes += node.NodeId.capacity() + node.SourceId.capacity();
			for (const auto &target : append.Project.GroupBootstrap)
				metadataBytes += target.NodeId.capacity();
			for (const auto &binding : append.Project.GroupBindings)
				metadataBytes += binding.NodeId.capacity() + binding.OwnerId.capacity() +
								 binding.Port.capacity() + binding.AnimatorPort.capacity() +
								 binding.Axes.OwnerId.capacity() + binding.Axes.Port.capacity() +
								 binding.Axes.InstanceBase.capacity();
			if (!charge(metadataBytes))
				return fail(Status::LimitExceeded, "Studio append identities exceed live bytes");
			ImageGraphAppendProject candidate;
			if (!candidate.Groups.Prepare(append, live, previous, revision, clock, diagnostic, remaining))
				return false;
			if (!charge(DocumentRetainedPayloadBytes(candidate.Groups.Authored)) ||
				!charge(candidate.Groups.Host.Replay.RetainedBytes()))
				return fail(Status::LimitExceeded, "Studio append callbacks exceed live bytes");
			PxcxAppendPostLoad postLoad;
			if (!PreparePxcxAppendPostLoad(append, sourcePath, postLoad, diagnostic, remaining)) return false;
			if (!charge(ImageGraphCollectionBytes(postLoad.Collections)) ||
				(postLoad.Source && !charge(ImageGraphHistoryArchiveBytes(*postLoad.Source))))
				return fail(Status::LimitExceeded, "Studio append post-load owners exceed live bytes");
			std::vector<PxcxCollectionMetadata> managers;
			if (!PreparePxcxCollectionMetadata(append.Project.Source, managers, diagnostic, remaining))
				return false;
			if (!charge(ImageGraphCollectionBytes(managers)))
				return fail(Status::LimitExceeded, "Studio append default managers exceed live bytes");
			uint64_t work = 64ull * 1024 * 1024;
			const auto same = [&](std::string_view left, std::string_view right) {
				const auto count = std::max(left.size(), right.size()) + 1;
				if (count > work) {
					work = 0;
					return false;
				}
				work -= count;
				return left == right;
			};
			for (auto &manager : managers) {
				if (collections)
					for (const auto &old : *collections)
						if (same(manager.NodeId, old.NodeId)) {
							if (!charge(old.MetadataJson.size() * 2 + 16))
								return fail(
									Status::LimitExceeded,
									"Studio append manager replacement exceeds live bytes"
								);
							manager.MetadataJson = old.MetadataJson;
							break;
						}
				for (const auto &loaded : postLoad.Collections)
					if (same(manager.NodeId, loaded.NodeId)) {
						if (!charge(loaded.MetadataJson.size() * 2 + 16))
							return fail(
								Status::LimitExceeded, "Studio append loaded manager exceeds live bytes"
							);
						manager.MetadataJson = loaded.MetadataJson;
						break;
					}
			}
			if (!work)
				return fail(Status::LimitExceeded, "Studio append manager matching exceeds work bounds");
			candidate.Collections =
				std::make_shared<const std::vector<PxcxCollectionMetadata>>(std::move(managers));
			if (!charge(ImageGraphCollectionBytes(candidate.Collections)))
				return fail(Status::LimitExceeded, "Studio append managers exceed live bytes");
			candidate.Source =
				postLoad.Source ? std::move(*postLoad.Source) : std::move(append.Project.Source);
			candidate.Projection = std::move(append.Project.Graph);
			candidate.Diagnostics = std::move(append.Project.Diagnostics);
			// grug source path stays in the archive. reimport it for the save baseline.
			if (postLoad.Source) {
				PxcxImport checked;
				std::string error;
				PxcxImportOptions importOptions;
				importOptions.MaximumOperationBytes = remaining;
				if (!ImportPxcxImageGraph(candidate.Source, checked, error, importOptions)) {
					diagnostic = {Status::InvalidValue, {}, {}, std::move(error)};
					return false;
				}
				candidate.Projection = std::move(checked.Graph);
				candidate.Diagnostics = std::move(checked.Diagnostics);
			}
			if (Migrate(candidate.Projection, diagnostic) != Status::Ok) return false;
			// grug destination preview outputs are host choices, not imported file outputs.
			const auto liveBytes = DocumentRetainedPayloadBytes(live);
			if (!liveBytes || !charge(*liveBytes * 2))
				return fail(Status::LimitExceeded, "Studio append output copies exceed live bytes");
			candidate.Groups.Authored.Outputs = live.Outputs;
			std::string error;
			if (!LoadImageGraphCanvas(candidate.Groups.Authored, candidate.Graph, candidate.Ids, error)) {
				diagnostic = {Status::InvalidValue, {}, {}, std::move(error)};
				return false;
			}
			candidate.Ids.IssuedNodeIds.insert(oldIds.IssuedNodeIds.begin(), oldIds.IssuedNodeIds.end());
			candidate.Ids.IssuedGroupIds.insert(oldIds.IssuedGroupIds.begin(), oldIds.IssuedGroupIds.end());
			candidate.Ids.NextNodeId = std::max(candidate.Ids.NextNodeId, oldIds.NextNodeId);
			candidate.Ids.NextGroupId = std::max(candidate.Ids.NextGroupId, oldIds.NextGroupId);
			for (const auto &node : append.Nodes) {
				if (node.TopLevel) {
					const auto found = candidate.Ids.ToCanvas.find(node.NodeId);
					if (found != candidate.Ids.ToCanvas.end())
						candidate.Selection.push_back(found->second);
					else if (std::any_of(
								 candidate.Groups.Authored.Groups.begin(),
								 candidate.Groups.Authored.Groups.end(),
								 [&](const auto &group) { return same(group.Id, node.NodeId); }
							 ))
						candidate.SelectedGroups.push_back(node.NodeId);
				}
				const auto loaded = std::find_if(
					candidate.Groups.Authored.Nodes.begin(),
					candidate.Groups.Authored.Nodes.end(),
					[&](const auto &item) { return item.Id == node.NodeId; }
				);
				if (loaded != candidate.Groups.Authored.Nodes.end() &&
					(loaded->Type == "pc.cache" || loaded->Type == "pc.cache_array"))
					candidate.LoadedCacheOwners.push_back(node.NodeId);
			}
			if (!work)
				return fail(Status::LimitExceeded, "Studio append group selection exceeds work bounds");
			const auto observerBytes = DocumentRetainedPayloadBytes(candidate.Groups.Authored);
			if (!observerBytes || !charge(*observerBytes * 2) || !charge(baseline.OriginalBytes.size()) ||
				!charge(candidate.Source.OriginalBytes.size()))
				return fail(Status::LimitExceeded, "Studio append snapshots exceed live bytes");
			candidate.Observation.Inputs = candidate.Groups.Authored;
			candidate.Observation.Ready = true;
			candidate.BeforeSource = std::make_shared<const std::vector<std::byte>>(baseline.OriginalBytes);
			candidate.AfterSource =
				std::make_shared<const std::vector<std::byte>>(candidate.Source.OriginalBytes);
			if (!charge(ImageGraphHistoryCanvasBytes(candidate.Graph, candidate.Ids)) ||
				!charge(DocumentRetainedPayloadBytes(candidate.Observation.Inputs)) ||
				!charge(candidate.BeforeSource->capacity()) || !charge(candidate.AfterSource->capacity()) ||
				!charge(candidate.Selection.capacity() * sizeof(nodegraph::NodeId)) ||
				!charge(candidate.SelectedGroups.capacity() * sizeof(std::string)) ||
				!charge(candidate.LoadedCacheOwners.capacity() * sizeof(std::string)))
				return fail(Status::LimitExceeded, "Studio append publication exceeds live bytes");
			for (const auto &group : candidate.SelectedGroups)
				if (!charge(group.capacity()))
					return fail(Status::LimitExceeded, "Studio append group selection exceeds live bytes");
			for (const auto &owner : candidate.LoadedCacheOwners)
				if (!charge(owner.capacity()))
					return fail(Status::LimitExceeded, "Studio append cache owners exceed live bytes");
			candidate.Remaining = remaining;
			*this = std::move(candidate);
			diagnostic = {};
			return true;
		} catch (const std::bad_alloc &) {
			diagnostic = {
				engine::imagegraph::Status::LimitExceeded, {}, {}, "Studio append staging allocation refused"
			};
			return false;
		}

		bool Admit(
			const engine::imagegraph::Document &live,
			ImageGraphHistory &history,
			engine::imagegraph::CapturedFeedbackHost &feedback,
			const ImageGraphHistory::CollectionSnapshot &beforeCollections,
			engine::imagegraph::Diagnostic &diagnostic
		) {
			if (!Remaining || !BeforeSource || !AfterSource) {
				diagnostic = {
					engine::imagegraph::Status::InvalidValue,
					{},
					{},
					"Studio append candidate is not prepared"
				};
				return false;
			}
			std::array<std::string_view, engine::imagegraph::Limits::MaximumNodes> owners{};
			if (LoadedCacheOwners.size() > owners.size()) return false;
			for (size_t index = 0; index < LoadedCacheOwners.size(); ++index)
				owners[index] = LoadedCacheOwners[index];
			return feedback.RefreshLoadedSourceCacheGroups(
				Groups.Authored,
				std::span(owners).first(LoadedCacheOwners.size()),
				diagnostic,
				[&](uint64_t) {
					return history.TryRecord(
						live, Groups.Authored, BeforeSource, AfterSource, beforeCollections, Collections
					);
				},
				Remaining
			);
		}
	};
}
