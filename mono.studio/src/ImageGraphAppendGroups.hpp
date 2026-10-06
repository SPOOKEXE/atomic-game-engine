#pragma once

#include "ImageGraphGroupHost.hpp"

#include <engine/core/Profiling.hpp>
#include <engine/imagegraph/FrameTime.hpp>
#include <engine/imagegraphio/PxcxAppend.hpp>

#include <array>
#include <type_traits>

namespace studio::detail {
	// grug load only incoming boundaries in source order, then bind the whole merged project.
	// caller still owns source/cache/history admission and publishes this candidate after those accept.
	// maximumBytes excludes caller-owned archives and import metadata; grug counts documents and replay here.
	// caller projects unsaved edits into the source archive before creating append.
	// compiler sees opaque callback dependencies; only a required opaque producer refuses.
	struct ImageGraphAppendGroups {
		engine::imagegraph::Document Authored;
		ImageGraphGroupHost Host;

		bool Prepare(
			const engine::imagegraphio::PxcxAppendResult &append,
			const engine::imagegraph::Document &live,
			const ImageGraphGroupHost &previous,
			uint64_t revision,
			engine::imagegraph::EvaluationRequest clock,
			engine::imagegraph::Diagnostic &diagnostic,
			uint64_t maximumBytes = engine::imagegraph::Limits::MaximumEvaluationBytes
		) try {
			using namespace engine::imagegraph;
			ENGINE_PROFILE("studio.imagegraph.append_groups");
			const auto fail = [&](Status code, const char *message) {
				diagnostic = {code, {}, {}, message};
				return false;
			};
			if (!revision || !ValidFrameTime(GetFrameTime(clock)))
				return fail(Status::InvalidValue, "Studio append Group revision or clock is invalid");
			if (!maximumBytes || maximumBytes > Limits::MaximumEvaluationBytes ||
				append.Nodes.size() > Limits::MaximumNodes ||
				append.Project.GroupBootstrap.size() > Limits::MaximumNodes)
				return fail(
					Status::LimitExceeded, "Studio append Group count, clock or budget is outside bounds"
				);
			if (previous.Replay.AuthoringRevision() != previous.Revision ||
				((live.SourceAnimators || std::any_of(
											  live.Nodes.begin(),
											  live.Nodes.end(),
											  [](const auto &node) { return !node.InstanceBase.empty(); }
										  )) &&
				 !previous.Replay.InstancesBound()))
				return fail(
					Status::InvalidValue, "Studio append requires the prepared destination Group host"
				);
			for (const auto &node : live.Nodes)
				if (node.Type == "pc.group_input" && !previous.Replay.Find(node.Id))
					return fail(
						Status::InvalidValue, "Studio append destination Group declaration is not prepared"
					);
			uint64_t remaining = maximumBytes;
			const auto charge = [&](uint64_t bytes) {
				if (bytes >= remaining) return false;
				remaining -= bytes;
				return true;
			};
			const auto document = [&](const Document &value) {
				const auto bytes = DocumentRetainedPayloadBytes(value);
				return bytes && charge(*bytes);
			};
			if (!document(live) || !document(append.Project.Graph) ||
				(append.Project.GroupPrebinding && !document(*append.Project.GroupPrebinding)) ||
				!document(Authored) || !charge(previous.BorrowedBytes) ||
				!charge(previous.Replay.RetainedBytes()) || !charge(Host.Replay.RetainedBytes()))
				return fail(Status::LimitExceeded, "Studio append Group owners exceed live payload bounds");
			uint64_t work = 64ull * 1024 * 1024;
			const auto same = [&](std::string_view a, std::string_view b) {
				const auto bytes = std::max(a.size(), b.size()) + 1;
				if (bytes > work) {
					work = 0;
					return false;
				}
				work -= bytes;
				return a == b;
			};
			std::array<GroupBootstrapTarget, Limits::MaximumNodes> order{};
			if (!charge(sizeof(order)))
				return fail(Status::LimitExceeded, "Studio append Group order exceeds payload bounds");
			size_t count = 0;
			for (const auto &node : append.Nodes)
				for (const auto &record : append.Project.GroupBootstrap)
					if (same(node.NodeId, record.NodeId)) {
						order[count++] = {record.NodeId, record.SubtypeAnimator};
						break;
					}
			if (!work)
				return fail(Status::LimitExceeded, "Studio append Group selection exceeds work bounds");
			const auto fresh = [&](std::string_view id) {
				if (!work) return false;
				return std::any_of(append.Nodes.begin(), append.Nodes.end(), [&](const auto &node) {
					return same(id, node.NodeId);
				});
			};
			const auto destination = [&](std::string_view id) {
				if (!work) return false;
				const auto contains = [&](const auto &items) {
					return std::any_of(items.begin(), items.end(), [&](const auto &item) {
						return same(item.Id, id);
					});
				};
				return contains(live.Nodes) || contains(live.Groups) || contains(live.Junctions);
			};
			const Document &saved =
				append.Project.GroupPrebinding ? *append.Project.GroupPrebinding : append.Project.Graph;
			const auto callbackBytes = DocumentRetainedPayloadBytes(saved);
			const auto mergedBytes = DocumentRetainedPayloadBytes(append.Project.Graph);
			const auto liveBytes = DocumentRetainedPayloadBytes(live);
			// grug reserve copies and vector growth before patching live destination fields.
			if (!callbackBytes || !mergedBytes || !liveBytes || *liveBytes > remaining / 2 ||
				!charge(*liveBytes * 2) || *callbackBytes > remaining / 2 || !charge(*callbackBytes * 2) ||
				*mergedBytes > remaining / 2 || !charge(*mergedBytes * 2))
				return fail(Status::LimitExceeded, "Studio append Group document copies exceed live bytes");
			Document callbacks = saved, merged = append.Project.Graph;
			for (auto *candidate : {&callbacks, &merged}) {
				for (const auto &old : live.Nodes) {
					if (fresh(old.Id))
						return fail(Status::DuplicateId, "Studio append identity overlaps destination");
					const auto found =
						std::find_if(candidate->Nodes.begin(), candidate->Nodes.end(), [&](const auto &item) {
							return same(item.Id, old.Id);
						});
					if (found != candidate->Nodes.end())
						*found = old;
					else {
						if (candidate->Nodes.size() >= Limits::MaximumNodes)
							return fail(
								Status::LimitExceeded, "Studio append live nodes exceed count bounds"
							);
						candidate->Nodes.push_back(old);
					}
				}
				for (const auto &old : live.Groups) {
					const auto found = std::find_if(
						candidate->Groups.begin(), candidate->Groups.end(), [&](const auto &item) {
							return same(item.Id, old.Id);
						}
					);
					if (found != candidate->Groups.end())
						*found = old;
					else
						candidate->Groups.push_back(old);
				}
				std::erase_if(candidate->Keyframes, [&](const auto &key) { return destination(key.NodeId); });
				candidate->Keyframes.insert(
					candidate->Keyframes.end(), live.Keyframes.begin(), live.Keyframes.end()
				);
				std::erase_if(candidate->Tracks, [&](const auto &track) {
					return destination(track.NodeId);
				});
				candidate->Tracks.insert(candidate->Tracks.end(), live.Tracks.begin(), live.Tracks.end());
				for (const auto &junction : live.Junctions) {
					const auto found = std::find_if(
						candidate->Junctions.begin(), candidate->Junctions.end(), [&](const auto &item) {
							return same(item.Id, junction.Id);
						}
					);
					if (found != candidate->Junctions.end())
						*found = junction;
					else
						candidate->Junctions.push_back(junction);
				}
				std::erase_if(candidate->Links, [&](const auto &link) {
					return destination(link.FromNode) && destination(link.ToNode);
				});
				candidate->Links.insert(candidate->Links.end(), live.Links.begin(), live.Links.end());
				if (candidate->Groups.size() > Limits::MaximumGroups ||
					candidate->Links.size() > Limits::MaximumLinks ||
					candidate->Junctions.size() > Limits::MaximumJunctions ||
					candidate->Keyframes.size() > Limits::MaximumKeyframes ||
					candidate->Tracks.size() > Limits::MaximumTracks)
					return fail(Status::LimitExceeded, "Studio append live topology exceeds count bounds");
				candidate->SourceAnimators = {};
			}
			if (!work)
				return fail(Status::LimitExceeded, "Studio append Group reconciliation exceeds work bounds");
			// grug keep live detached writers and axis generations, not stale archive guesses.
			std::vector<GroupSubtypeBinding> bindings;
			const auto addBinding = [&](const GroupSubtypeBinding &binding) {
				const uint64_t names = binding.NodeId.capacity() + binding.OwnerId.capacity() +
									   binding.Port.capacity() + binding.AnimatorPort.capacity() +
									   binding.Axes.OwnerId.capacity() + binding.Axes.Port.capacity() +
									   binding.Axes.InstanceBase.capacity() + 7;
				const auto capacity = bindings.size() == bindings.capacity()
										  ? std::max(size_t{8}, bindings.capacity() * 2)
										  : bindings.capacity();
				const uint64_t growth = (capacity - bindings.capacity()) * sizeof(GroupSubtypeBinding);
				if (!charge(
						names + growth +
						(capacity != bindings.capacity() ? bindings.capacity() * sizeof(GroupSubtypeBinding)
														 : 0)
					))
					return false;
				if (capacity != bindings.capacity()) {
					const uint64_t old = bindings.capacity() * sizeof(GroupSubtypeBinding);
					bindings.reserve(capacity);
					remaining += old;
				}
				bindings.push_back(binding);
				return true;
			};
			for (const auto &binding : previous.Replay.Bindings())
				if (!fresh(binding.NodeId) && !addBinding(binding))
					return fail(
						Status::LimitExceeded, "Studio append destination bindings exceed payload bounds"
					);
			for (const auto &binding : append.Project.GroupBindings)
				if (fresh(binding.NodeId) && !addBinding(binding))
					return fail(
						Status::LimitExceeded, "Studio append incoming bindings exceed payload bounds"
					);
			if (!work)
				return fail(Status::LimitExceeded, "Studio append binding selection exceeds work bounds");
			const auto actualCallbacks = DocumentRetainedPayloadBytes(callbacks);
			const auto actualMerged = DocumentRetainedPayloadBytes(merged);
			const uint64_t reservedCopies = *liveBytes * 2 + *callbackBytes * 2 + *mergedBytes * 2;
			if (!actualCallbacks || !actualMerged || *actualCallbacks > reservedCopies ||
				*actualMerged > reservedCopies - *actualCallbacks)
				return fail(Status::LimitExceeded, "Studio append patched documents exceed reserved payload");
			remaining += reservedCopies - *actualCallbacks - *actualMerged;
			if (PrepareGroupCallbackDocument(callbacks, callbacks, diagnostic, remaining) != Status::Ok)
				return false;
			const auto opaqueCallbacks = DocumentRetainedPayloadBytes(callbacks);
			if (!opaqueCallbacks || *opaqueCallbacks >= remaining + *actualCallbacks)
				return fail(
					Status::LimitExceeded, "Studio append opaque callback scratch exceeds payload bounds"
				);
			remaining += *actualCallbacks;
			remaining -= *opaqueCallbacks;
			GroupReplayState rebound, loaded, bound, projected;
			if (RebindGroupReplay(callbacks, previous.Replay, revision, rebound, diagnostic, remaining) !=
				Status::Ok)
				return false;
			if (count) {
				Plan plan;
				if (rebound.RetainedBytes() >= remaining)
					return fail(
						Status::LimitExceeded, "Studio append rebound state leaves no compiler allowance"
					);
				const uint64_t compilerBytes = (remaining - rebound.RetainedBytes()) / 2;
				if (!compilerBytes || Compile(callbacks, plan, diagnostic, compilerBytes) != Status::Ok)
					return false;
				if (compilerBytes >= remaining)
					return fail(Status::LimitExceeded, "Studio append compiler leaves no callback allowance");
				auto sourceProject = clock.SourceCacheProject.value_or(SourceFrameCacheProjectObservation{});
				sourceProject.ProjectAppending = true;
				sourceProject.ProjectLoading = false;
				clock.SourceCacheProject = sourceProject;
				if (ReplayGroupBootstrap(
						callbacks,
						plan,
						std::span(order).first(count),
						clock,
						rebound,
						revision,
						loaded,
						diagnostic,
						remaining - compilerBytes
					) != Status::Ok)
					return false;
			} else
				loaded = std::move(rebound);
			rebound = {};
			const auto allowance = remaining;
			if (RebindGroupReplay(merged, loaded, revision, rebound, diagnostic, allowance) != Status::Ok)
				return false;
			loaded = {};
			if (BindGroupReplay(merged, bindings, rebound, revision, bound, diagnostic, allowance) !=
				Status::Ok)
				return false;
			rebound = {};
			Document authored;
			if (ProjectGroupReplay(merged, bound, revision, authored, diagnostic, allowance) != Status::Ok)
				return false;
			if (bound.RetainedBytes() >= allowance ||
				RebindProjectedGroupReplay(
					authored, bound, revision, projected, diagnostic, allowance - bound.RetainedBytes()
				) != Status::Ok)
				return false;
			static_assert(std::is_nothrow_move_assignable_v<Document>);
			static_assert(std::is_nothrow_move_assignable_v<GroupReplayState>);
			Authored = std::move(authored);
			Host.Replay = std::move(projected);
			Host.Revision = revision;
			Host.BorrowedBytes = 0;
			diagnostic = {};
			return true;
		} catch (const std::bad_alloc &) {
			diagnostic = {
				engine::imagegraph::Status::LimitExceeded, {}, {}, "Studio append Group allocation refused"
			};
			return false;
		}
	};
}
