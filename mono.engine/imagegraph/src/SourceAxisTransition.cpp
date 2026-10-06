#include "EvaluationAllocator.hpp"
#include "GroupReplayInternal.hpp"
#include "SourceAxisStorage.hpp"
#include "SourceInputEvaluation.hpp"
#include "ValuePayload.hpp"

#include <engine/core/Profiling.hpp>
#include <engine/imagegraph/SourceAxisTransition.hpp>

#include <algorithm>
#include <new>

namespace engine::imagegraph {
	namespace {
		uint64_t TextBytes(std::string_view text) {
			return std::max(text.size(), std::string{}.capacity()) + 1;
		}
		Node *FindNode(Document &document, std::string_view id) {
			const auto node =
				std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const auto &item) {
					return item.Id == id;
				});
			return node == document.Nodes.end() ? nullptr : &*node;
		}
		SourceSeparatedVec2Animator *FindAxes(Node &node, std::string_view port) {
			if (!node.SourceSeparatedVec2Animators) return nullptr;
			for (auto &axes : node.SourceSeparatedVec2Animators->Inputs)
				if (axes.Port == port) return &axes;
			return nullptr;
		}
		bool Charge(detail::EvaluationBudget &budget, detail::AllocationReservation &held, uint64_t bytes) {
			auto extra = budget.Reserve(bytes);
			return extra && held.Merge(std::move(*extra));
		}
		// stage replacements share one outer ledger; adapters count their borrowed stage again internally.
		Status ProjectStage(
			Document &document,
			const GroupReplayState &replay,
			uint64_t revision,
			detail::EvaluationBudget &budget,
			detail::AllocationReservation &held,
			Diagnostic &diagnostic
		) {
			Document projected;
			const auto bytes = DocumentRetainedPayloadBytes(document);
			if (!bytes) return Status::LimitExceeded;
			const auto status = ProjectGroupReplay(
				document,
				replay,
				revision,
				projected,
				diagnostic,
				budget.Available() + *bytes + replay.RetainedBytes()
			);
			if (status != Status::Ok) return status;
			const auto newBytes = DocumentRetainedPayloadBytes(projected);
			auto replacement = newBytes ? budget.Reserve(*newBytes) : std::nullopt;
			if (!replacement) return Status::LimitExceeded;
			document = std::move(projected);
			held = std::move(*replacement);
			return Status::Ok;
		}
		Status RebindStage(
			const Document &document,
			GroupReplayState &replay,
			uint64_t revision,
			detail::EvaluationBudget &budget,
			detail::AllocationReservation &held,
			Diagnostic &diagnostic
		) {
			GroupReplayState rebound;
			const auto bytes = DocumentRetainedPayloadBytes(document);
			if (!bytes) return Status::LimitExceeded;
			const auto status = RebindProjectedGroupReplay(
				document,
				replay,
				revision,
				rebound,
				diagnostic,
				budget.Available() + *bytes + replay.RetainedBytes()
			);
			if (status != Status::Ok) return status;
			auto replacement = budget.Reserve(rebound.RetainedBytes());
			if (!replacement) return Status::LimitExceeded;
			replay = std::move(rebound);
			held = std::move(*replacement);
			return Status::Ok;
		}
		bool SetFlag(
			Document &document,
			std::string_view nodeId,
			std::string_view port,
			bool separated,
			detail::EvaluationBudget &budget,
			detail::AllocationReservation &held
		) {
			auto &node = *FindNode(document, nodeId);
			auto *axes = FindAxes(node, port);
			if (!axes) {
				const auto count =
					node.SourceSeparatedVec2Animators ? node.SourceSeparatedVec2Animators->Inputs.size() : 0;
				if (count >= Limits::MaximumArrayElements ||
					!Charge(
						budget,
						held,
						sizeof(SourceSeparatedVec2Data) + (count + 1) * sizeof(SourceSeparatedVec2Animator) +
							TextBytes(port)
					))
					return false;
				if (!node.SourceSeparatedVec2Animators) node.SourceSeparatedVec2Animators.emplace();
				auto &inputs = node.SourceSeparatedVec2Animators->Inputs;
				inputs.reserve(count + 1);
				SourceSeparatedVec2Animator cold;
				cold.Port = std::string(port);
				cold.Initialized = false;
				inputs.push_back(std::move(cold));
				axes = &inputs.back();
			}
			axes->Separated = separated;
			return true;
		}
		Keyframe FreshKey(std::string_view owner, std::string_view port, const Keyframe &raw, Value value) {
			Keyframe key;
			key.NodeId = std::string(owner);
			key.Port = std::string(port);
			key.Tick = raw.Tick;
			key.Subframe = raw.Subframe;
			key.NegativeFrame = raw.NegativeFrame;
			key.Data = std::move(value);
			key.Interpolation = "source";
			key.Ease = raw.Ease.value_or(KeyframeEase{});
			return key;
		}
		uint64_t FreshKeyBytes(std::string_view owner, std::string_view port, const KeyframeEase &ease) {
			return sizeof(Keyframe) + TextBytes(owner) + TextBytes(port) + TextBytes("source") +
				   TextBytes("") + TextBytes(ease.InType) + TextBytes(ease.OutType);
		}
		// arrays retain their first two raw components. absent components become zero in separateAxis.
		std::optional<Vector2> RawComponents(const Value &value) {
			if (const auto *vector = std::get_if<Vector2>(&value)) return *vector;
			const auto *array = std::get_if<ArrayValue>(&value);
			if (!array) return std::nullopt;
			Vector2 result;
			if (!array->Nested.empty() || (!array->Items.empty() && !array->Elements.empty()))
				return std::nullopt;
			const auto count = array->Items.empty() ? array->Elements.size() : array->Items.size();
			for (size_t axis = 0; axis < std::min(size_t{2}, count); ++axis) {
				const auto *element = array->Items.empty()
										  ? &array->Elements[axis]
										  : std::get_if<ElementValue>(&array->Items[axis].Data);
				if (!element) return std::nullopt;
				double component;
				if (const auto *scalar = std::get_if<double>(element))
					component = *scalar;
				else if (const auto *integer = std::get_if<int64_t>(element))
					component = double(*integer);
				else
					return std::nullopt;
				if (axis == 0)
					result.X = component;
				else
					result.Y = component;
			}
			return result;
		}
	}
	bool SupportsSourceAxisTransition(const Node &node, std::string_view port) {
		return detail::SourceSeparatedVec2Input(node, port) != nullptr;
	}
	Status ToggleSourceAxes(
		const Document &document,
		const GroupReplayState &replay,
		uint64_t revision,
		const SourceAxisTransition &transition,
		const EvaluationRequest &request,
		Document &result,
		GroupReplayState &replayResult,
		Diagnostic &diagnostic,
		uint64_t maximumBytes
	) try {
		ENGINE_PROFILE("imagegraph.source_axis_transition");
		const auto fail = [&](Status code, std::string_view message) {
			diagnostic = {
				code,
				transition.NodeId.size() <= Limits::MaximumTextBytes ? std::string(transition.NodeId)
																	 : std::string{},
				transition.Port.size() <= Limits::MaximumTextBytes ? std::string(transition.Port)
																   : std::string{},
				std::string(message)
			};
			return code;
		};
		if (!maximumBytes || maximumBytes > Limits::MaximumEvaluationBytes ||
			transition.NodeId.size() > Limits::MaximumTextBytes ||
			transition.Port.size() > Limits::MaximumTextBytes)
			return fail(Status::LimitExceeded, "source axis transaction bounds are invalid");
		if (!replay.InstancesBound() || replay.AuthoringRevision() != revision)
			return fail(
				Status::InvalidValue, "source axis transaction requires bound replay at this revision"
			);

		if (document.Nodes.size() > Limits::MaximumNodes ||
			document.Keyframes.size() > Limits::MaximumKeyframes ||
			document.Links.size() > Limits::MaximumLinks || document.Tracks.size() > Limits::MaximumTracks ||
			document.Groups.size() > Limits::MaximumGroups ||
			document.Outputs.size() > Limits::MaximumOutputs ||
			document.Junctions.size() > Limits::MaximumJunctions)
			return fail(Status::LimitExceeded, "source axis document counts exceed bounds");
		uint64_t observedBytes = 0;
		uint64_t mapWork = 0;
		if (transition.ObservedInputOwner.size() > Limits::MaximumTextBytes)
			return fail(Status::LimitExceeded, "source axis observed map owner exceeds bounds");
		if (transition.ObservedInputs) {
			if (transition.ObservedInputs->size() > Limits::MaximumArrayElements)
				return fail(Status::LimitExceeded, "source axis observed map count exceeds bounds");
			uint64_t longestName = 0;
			for (const auto &input : *transition.ObservedInputs) {
				if (input.Port.size() > Limits::MaximumTextBytes || !ValueClonePayloadBytes(input.Data))
					return fail(Status::LimitExceeded, "source axis observed map exceeds bounds");
				const uint64_t bytes =
					sizeof(AuthoredValue) + input.Port.capacity() + detail::RetainedPayloadBytes(input.Data);
				if (bytes > maximumBytes - std::min(observedBytes, maximumBytes))
					return fail(Status::LimitExceeded, "source axis observed map payload exceeds bounds");
				observedBytes += bytes;
				longestName = std::max(longestName, uint64_t(input.Port.size()));
			}
			mapWork =
				transition.ObservedInputs->size() * transition.ObservedInputs->size() * (1 + longestName);
			if (mapWork > 64'000'000)
				return fail(Status::LimitExceeded, "source axis observed map work exceeds bounds");
		}
		const auto documentBytes = DocumentRetainedPayloadBytes(document);
		const auto oldResultBytes =
			&document == &result ? std::optional<uint64_t>{0} : DocumentRetainedPayloadBytes(result);
		if (!documentBytes || !oldResultBytes)
			return fail(Status::LimitExceeded, "source axis borrowed document exceeds bounds");
		detail::EvaluationBudget budget(maximumBytes);
		auto sourceCharge = budget.Reserve(*documentBytes);
		auto observedCharge = budget.Reserve(observedBytes);
		auto oldOutputCharge = budget.Reserve(*oldResultBytes);
		auto sourceReplayCharge = budget.Reserve(replay.RetainedBytes());
		auto oldReplayCharge = budget.Reserve(&replay == &replayResult ? 0 : replayResult.RetainedBytes());
		if (!sourceCharge || !observedCharge || !oldOutputCharge || !sourceReplayCharge || !oldReplayCharge)
			return fail(Status::LimitExceeded, "source axis borrowed owners exceed budget");
		const auto target = std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const auto &node) {
			return node.Id == transition.NodeId;
		});
		if (target == document.Nodes.end()) return fail(Status::UnknownNode, "source axis target is absent");
		if (!detail::SourceSeparatedVec2Input(*target, transition.Port))
			return fail(Status::UnknownPort, "source axis target needs a represented two-axis input");
		const bool wasSeparated = detail::SourcePropertyLocallySeparated(*target, transition.Port, &replay);
		Document staged;
		auto status = ProjectGroupReplay(
			document,
			replay,
			revision,
			staged,
			diagnostic,
			budget.Available() + *documentBytes + replay.RetainedBytes()
		);
		if (status != Status::Ok) return status;
		const auto stagedBytes = DocumentRetainedPayloadBytes(staged);
		auto stagedCharge = stagedBytes ? budget.Reserve(*stagedBytes) : std::nullopt;
		if (!stagedCharge) return fail(Status::LimitExceeded, "source axis stage exceeds budget");
		GroupReplayState stagedReplay;
		status = RebindProjectedGroupReplay(
			staged,
			replay,
			revision,
			stagedReplay,
			diagnostic,
			budget.Available() + *stagedBytes + replay.RetainedBytes()
		);
		if (status != Status::Ok) return status;
		auto stagedReplayCharge = budget.Reserve(stagedReplay.RetainedBytes());
		if (!stagedReplayCharge)
			return fail(Status::LimitExceeded, "source axis replay stage exceeds budget");
		detail::AllocationReservation mutationCharge;
		staged.FormatVersion = 9;
		if (wasSeparated != transition.Separated && transition.SetValue) {
			const SourceAxisInitialization initialization{transition.NodeId, transition.Port};
			status = InitializeSourceVec2Axes(
				staged,
				{&initialization, 1},
				stagedReplay,
				revision,
				stagedReplay,
				diagnostic,
				budget.Available() + *stagedBytes + stagedReplay.RetainedBytes()
			);
			if (status != Status::Ok) return status;
			if (!stagedReplayCharge->Resize(stagedReplay.RetainedBytes()))
				return fail(Status::LimitExceeded, "source axis initialized replay exceeds budget");
			status = ProjectStage(staged, stagedReplay, revision, budget, *stagedCharge, diagnostic);
			if (status != Status::Ok)
				return diagnostic.Code == status ? status
												 : fail(status, "source axis initialized projection failed");
			status = RebindStage(staged, stagedReplay, revision, budget, *stagedReplayCharge, diagnostic);
			if (status != Status::Ok)
				return diagnostic.Code == status ? status
												 : fail(status, "source axis initialized rebind failed");
			auto *selected = FindNode(staged, transition.NodeId);
			const auto *binding = stagedReplay.Binding(transition.NodeId, transition.Port);
			const std::string_view combinedOwner =
				binding ? std::string_view(binding->OwnerId) : transition.NodeId;
			const std::string_view combinedPort =
				binding ? detail::BindingAnimatorPort(*binding) : transition.Port;
			if (!binding && !selected->InstanceBase.empty())
				return fail(Status::InvalidGroup, "source axis instance has no retained binding");
			const bool writerAnimated = binding ? binding->Writer == GroupSubtypeAnimator::Animated
												: std::find(
													  selected->SourceAnimatedInputs.begin(),
													  selected->SourceAnimatedInputs.end(),
													  transition.Port
												  ) != selected->SourceAnimatedInputs.end();
			uint64_t work = 0;
			const auto storage = detail::ResolveLocalSourceAxes(
				staged,
				*selected,
				transition.Port,
				stagedReplay.Bindings(),
				stagedReplay.SharedSubtypes(),
				stagedReplay.DetachedAnimators(),
				combinedOwner,
				combinedPort,
				writerAnimated,
				work,
				false
			);
			if (storage.Code != Status::Ok) return fail(storage.Code, storage.Message);
			if (!storage.Owner || !storage.Axes || !storage.Axes->Initialized)
				return fail(Status::UnsupportedExecution, "source axis physical storage is absent");
			const auto *combinedDetached = stagedReplay.DetachedAnimator(combinedOwner, combinedPort);
			const auto *combinedOverlay =
				combinedDetached ? stagedReplay.SharedSubtype(combinedOwner, combinedPort) : nullptr;
			if (combinedDetached && !combinedOverlay)
				return fail(Status::InvalidGroup, "source axis combined retained animator is absent");
			if (transition.Separated) {
				size_t keyCount =
					combinedOverlay
						? combinedOverlay->Keys.size()
						: std::count_if(
							  staged.Keyframes.begin(), staged.Keyframes.end(), [&](const auto &key) {
								  return key.NodeId == combinedOwner && key.Port == combinedPort;
							  }
						  );
				std::optional<Keyframe> implicit;
				const bool configured =
					combinedOverlay ||
					std::any_of(staged.Tracks.begin(), staged.Tracks.end(), [&](const auto &track) {
						return track.NodeId == combinedOwner && track.Port == combinedPort;
					});
				if (!keyCount && ((combinedOverlay && combinedOverlay->Fixed) || !configured)) {
					const auto *combinedNode = FindNode(staged, combinedOwner);
					const Value *raw =
						combinedOverlay && combinedOverlay->Fixed ? &*combinedOverlay->Fixed : nullptr;
					for (const auto &value : combinedNode->Values)
						if (!raw && value.Port == combinedPort) raw = &value.Data;
					for (const auto &input : combinedNode->DynamicInputs)
						if (!raw && input.Id == combinedPort && input.Default) raw = &*input.Default;
					if (!raw)
						return fail(
							Status::UnsupportedExecution, "source implicit combined raw row is absent"
						);
					const auto bytes = ValueClonePayloadBytes(*raw);
					if (!bytes || !Charge(budget, mutationCharge, *bytes + sizeof(Keyframe)))
						return fail(Status::LimitExceeded, "source implicit combined row exceeds budget");
					implicit.emplace();
					implicit->Data = *raw;
					keyCount = 1;
				}

				if (keyCount > Limits::MaximumKeyframes / 2)
					return fail(Status::LimitExceeded, "source axis split key count exceeds bounds");
				uint64_t freshBytes = sizeof(SourceSeparatedVec2Animator) + TextBytes(storage.Port);
				const auto admitKey = [&](const Keyframe &raw) {
					const auto components = RawComponents(raw.Data);
					if (!components) return false;
					const uint64_t bytes =
						2 * FreshKeyBytes(storage.Owner->Id, storage.Port, raw.Ease.value_or(KeyframeEase{}));
					if (bytes > maximumBytes - std::min(freshBytes, maximumBytes)) return false;
					freshBytes += bytes;
					return true;
				};
				if (implicit && !admitKey(*implicit))
					return fail(
						Status::UnsupportedExecution, "source implicit raw components are not represented"
					);
				if (combinedOverlay) {
					for (const auto &key : combinedOverlay->Keys)
						if (!admitKey(key))
							return fail(
								Status::UnsupportedExecution,
								"source axis raw components are not represented within budget"
							);
				} else {
					for (const auto &key : staged.Keyframes)
						if (key.NodeId == combinedOwner && key.Port == combinedPort && !admitKey(key))
							return fail(
								Status::UnsupportedExecution,
								"source axis raw components are not represented within budget"
							);
				}
				if (!Charge(budget, mutationCharge, freshBytes))
					return fail(Status::LimitExceeded, "source axis split rows exceed budget");
				SourceSeparatedVec2Animator replacement;
				replacement.Port = std::string(storage.Port);
				replacement.Separated = storage.Axes->Separated;
				for (auto &axis : replacement.Axes)
					axis.Keys.reserve(keyCount);
				const auto append = [&](const Keyframe &raw) {
					const auto components = *RawComponents(raw.Data);
					replacement.Axes[0].Keys.push_back(
						FreshKey(storage.Owner->Id, storage.Port, raw, components.X)
					);
					replacement.Axes[1].Keys.push_back(
						FreshKey(storage.Owner->Id, storage.Port, raw, components.Y)
					);
				};
				if (implicit) append(*implicit);
				if (combinedOverlay) {
					for (const auto &key : combinedOverlay->Keys)
						append(key);
				} else {
					for (const auto &key : staged.Keyframes)
						if (key.NodeId == combinedOwner && key.Port == combinedPort) append(key);
				}
				if (storage.Detached) {
					auto *owner = detail::GroupReplayAccess::Get(stagedReplay);
					auto physical = std::find_if(
						owner->SharedSubtypes.begin(), owner->SharedSubtypes.end(), [&](const auto &item) {
							return item.NodeId == storage.Owner->Id && item.Port == storage.Port;
						}
					);
					const auto oldBytes = detail::SeparatedOverlayBytes(*physical);
					auto charge = owner->Budget.Reserve(freshBytes);
					if (!charge)
						return fail(Status::LimitExceeded, "source retained split rows exceed replay budget");
					physical->SeparatedVec2.emplace() = std::move(replacement);
					if (!owner->Charge.Merge(std::move(*charge)) ||
						!owner->Charge.Resize(owner->Charge.Bytes() - oldBytes))
						std::terminate();
				} else
					*FindAxes(*FindNode(staged, storage.Owner->Id), storage.Port) = std::move(replacement);
			} else {
				const size_t count = storage.Axes->Axes[0].Keys.size() + storage.Axes->Axes[1].Keys.size();
				if (count > Limits::MaximumKeyframes)
					return fail(Status::LimitExceeded, "source axis union exceeds key bounds");
				detail::EvaluationVector<FrameTime> times{detail::EvaluationAllocator<FrameTime>{budget}};
				times.reserve(count);
				for (const auto &axis : storage.Axes->Axes)
					for (const auto &key : axis.Keys)
						times.push_back(GetFrameTime(key));
				std::sort(times.begin(), times.end(), [](const auto &left, const auto &right) {
					return CompareFrameTime(left, right) < 0;
				});
				times.erase(std::unique(times.begin(), times.end()), times.end());
				// each sample compiles the progressively rebuilt source animator. bound the whole conversion.
				uint64_t perSampleWork = 1 + staged.Nodes.size() * (1 + staged.Links.size()) +
										 staged.Keyframes.size() + count + mapWork;
				if (perSampleWork > 64'000'000 || times.size() > (64'000'000 - work) / perSampleWork)
					return fail(Status::LimitExceeded, "source axis sampling batch exceeds work bounds");
				const uint64_t rowsBytes =
					times.size() * FreshKeyBytes(combinedOwner, combinedPort, KeyframeEase{});
				if (!Charge(
						budget,
						mutationCharge,
						rowsBytes + (staged.Keyframes.size() + times.size()) * sizeof(Keyframe)
					))
					return fail(Status::LimitExceeded, "source axis combined rows exceed budget");
				auto *owner = detail::GroupReplayAccess::Get(stagedReplay);
				GroupSubtypeOverlay *detachedKeys = nullptr;
				detail::AllocationReservation replayRows;
				uint64_t oldPayload = 0;
				if (combinedDetached) {
					const auto found = std::find_if(
						owner->SharedSubtypes.begin(), owner->SharedSubtypes.end(), [&](const auto &item) {
							return item.NodeId == combinedOwner && item.Port == combinedPort;
						}
					);
					detachedKeys = &*found;
					oldPayload = detachedKeys->Keys.capacity() * sizeof(Keyframe);
					if (detachedKeys->Fixed) oldPayload += detail::RetainedPayloadBytes(*detachedKeys->Fixed);
					for (const auto &key : detachedKeys->Keys)
						oldPayload += *KeyframePayloadBytes(key) - sizeof(Keyframe);
					auto charge = owner->Budget.Reserve(rowsBytes);
					if (!charge)
						return fail(
							Status::LimitExceeded, "source retained combined rows exceed replay budget"
						);
					replayRows = std::move(*charge);
					detachedKeys->Fixed.reset();
					std::vector<Keyframe>{}.swap(detachedKeys->Keys);
					detachedKeys->Keys.reserve(times.size());
				} else {
					std::erase_if(staged.Keyframes, [&](const auto &key) {
						return key.NodeId == combinedOwner && key.Port == combinedPort;
					});
					staged.Keyframes.reserve(staged.Keyframes.size() + times.size());
				}
				// combineAxis empties the writer first, then observes getValue(time,false) after each new
				// row.
				for (const auto &time : times) {
					EvaluationRequest at = request;
					at.GroupReplay = &stagedReplay;
					at.GroupAuthoringRevision = revision;
					if (!SetFrameTime(at, time))
						return fail(Status::InvalidValue, "source axis key clock is invalid");
					Value value;
					detail::AllocationReservation valueCharge;
					status = detail::EvaluateSourceInput(
						staged,
						transition.NodeId,
						transition.Port,
						at,
						budget,
						value,
						valueCharge,
						diagnostic,
						transition.ObservedInputs,
						transition.ObservedInputOwner
					);
					if (status != Status::Ok) return status;
					if (!std::holds_alternative<Vector2>(value))
						return fail(Status::TypeMismatch, "source axis getter did not return Vec2");
					Keyframe raw;
					if (!SetFrameTime(raw, time))
						return fail(Status::InvalidValue, "source combined clock is invalid");
					auto key = FreshKey(combinedOwner, combinedPort, raw, std::move(value));
					if (detachedKeys)
						detachedKeys->Keys.push_back(std::move(key));
					else
						staged.Keyframes.push_back(std::move(key));
				}
				if (detachedKeys && (!owner->Charge.Merge(std::move(replayRows)) ||
									 !owner->Charge.Resize(owner->Charge.Bytes() - oldPayload)))
					std::terminate();
			}
		}
		if (wasSeparated != transition.Separated &&
			!SetFlag(
				staged, transition.NodeId, transition.Port, transition.Separated, budget, mutationCharge
			))
			return fail(Status::LimitExceeded, "source axis local flag exceeds budget");
		// the physical arrays can be detached; the selected property's local flag always remains authored.
		staged.FormatVersion = 9;
		const auto changedBytes = DocumentRetainedPayloadBytes(staged);
		if (!changedBytes || !stagedCharge->Resize(*changedBytes) ||
			!stagedReplayCharge->Resize(stagedReplay.RetainedBytes()))
			return fail(Status::LimitExceeded, "source axis changed owners exceed budget");
		status = ProjectStage(staged, stagedReplay, revision, budget, *stagedCharge, diagnostic);
		if (status != Status::Ok)
			return diagnostic.Code == status ? status : fail(status, "source axis final projection failed");
		status = RebindStage(staged, stagedReplay, revision, budget, *stagedReplayCharge, diagnostic);
		if (status != Status::Ok)
			return diagnostic.Code == status ? status : fail(status, "source axis final rebind failed");
		Plan plan;
		detail::AllocationReservation planCharge;
		status = detail::CompileSourceDocument(staged, plan, budget, planCharge, diagnostic);
		if (status != Status::Ok) return status;
		result = std::move(staged);
		replayResult = std::move(stagedReplay);
		diagnostic = {};
		return Status::Ok;
	} catch (const std::bad_alloc &) {
		diagnostic = {Status::LimitExceeded, {}, {}, "source axis transaction allocation failed"};
		return diagnostic.Code;
	}
}
