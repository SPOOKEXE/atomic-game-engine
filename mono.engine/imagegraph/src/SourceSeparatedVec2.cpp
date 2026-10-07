#include "SourceSeparatedVec2.hpp"

#include "TimelineDrivers.hpp"
#include "TimelineSchedule.hpp"

#include <engine/core/Profiling.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <tuple>

namespace engine::imagegraph::detail {
	bool SourceNumericVec2Tuple(const Value &value) {
		const auto *array = std::get_if<ArrayValue>(&value);
		if (!array || !array->Nested.empty() ||
			(array->Items.empty() ? array->Elements.size() != 2
								  : !array->Elements.empty() || array->Items.size() != 2))
			return false;
		for (size_t axis = 0; axis < 2; ++axis) {
			const auto *component = array->Items.empty()
										? &array->Elements[axis]
										: std::get_if<ElementValue>(&array->Items[axis].Data);
			if (!component) return false;
			if (const auto *number = std::get_if<double>(component)) {
				if (!std::isfinite(*number)) return false;
			} else if (!std::holds_alternative<int64_t>(*component))
				return false;
		}
		return true;
	}
	const CatalogueInput *SourceSeparatedVec2Input(const Node &node, std::string_view port) {
		const auto *entry = FindCatalogueEntry(node.Type);
		if (!entry) return nullptr;
		const auto *input = FindCatalogueInput(*entry, port);
		if (!input) {
			const auto declared =
				std::find_if(node.DynamicInputs.begin(), node.DynamicInputs.end(), [&](const auto &value) {
					return value.Id == port;
				});
			if (declared == node.DynamicInputs.end()) return nullptr;
			size_t group = 0;
			input = FindDynamicTemplate(*entry, port, group);
		}
		if (!input || input->SourceIndex < 0 || input->Type != ValueType::Vector2) return nullptr;
		return input->SourceKind == "Vec2" || input->SourceKind == "IVec2" ||
					   input->SourceKind == "Dimension" || input->SourceKind == "Range"
				   ? input
				   : nullptr;
	}
	size_t SourceSeparatedVec2InputCount(const Node &node) {
		const auto *entry = FindCatalogueEntry(node.Type);
		if (!entry) return 0;
		size_t count = 0;
		for (const auto &input : entry->Inputs)
			count += SourceSeparatedVec2Input(node, input.Id) != nullptr;
		for (const auto &input : node.DynamicInputs)
			count += SourceSeparatedVec2Input(node, input.Id) != nullptr;
		return count;
	}
	const SourceSeparatedVec2Animator *FindSeparatedVec2(const Node &node, std::string_view port) {
		if (!node.SourceSeparatedVec2Animators) return nullptr;
		for (const auto &input : node.SourceSeparatedVec2Animators->Inputs)
			if (input.Port == port) return &input;
		return nullptr;
	}
	const SourceSeparatedVec2Animator *FindInitializedSeparatedVec2(const Node &node, std::string_view port) {
		const auto *input = FindSeparatedVec2(node, port);
		return input && input->Initialized ? input : nullptr;
	}

	namespace {
		bool Add(uint64_t &bytes, uint64_t added) {
			if (added > UINT64_MAX - bytes) return false;
			bytes += added;
			return true;
		}
		bool Text(
			uint64_t &bytes, const std::string &text, bool retained, size_t maximum = Limits::MaximumTextBytes
		) {
			return text.size() <= maximum &&
				   Add(bytes, retained ? text.capacity() : std::max(text.size(), std::string{}.capacity()));
		}
		bool ScalarKey(const Keyframe &key) {
			const auto *number = std::get_if<double>(&key.Data);
			if (!number || !std::isfinite(*number) || !ValidFrameTime(GetFrameTime(key)) ||
				(key.Kind != KeyframeKind::Normal && key.Kind != KeyframeKind::Adder) ||
				(key.Interpolation != "source" && key.Interpolation != "step" &&
				 key.Interpolation != "linear" && key.Interpolation != "cubic") ||
				(key.Interpolation == "source") != key.Ease.has_value())
				return false;
			if (key.Ease) {
				const auto side = [](std::string_view type) {
					return type == "linear" || type == "bezier" || type == "cut";
				};
				const auto &ease = *key.Ease;
				if (!side(ease.InType) || !side(ease.OutType) || !std::isfinite(ease.In.X) ||
					!std::isfinite(ease.In.Y) || !std::isfinite(ease.Out.X) || !std::isfinite(ease.Out.Y))
					return false;
			}
			if (key.SourceDriver && (key.Interpolation != "source" || key.SineDriver ||
									 !ValidKeyframeSourceDriver(*key.SourceDriver)))
				return false;
			if (key.SineDriver) {
				const auto &sine = *key.SineDriver;
				if (key.Interpolation != "source" || !std::isfinite(sine.Frequency) ||
					!std::isfinite(sine.Amplitude) || !std::isfinite(sine.Phase) ||
					!std::isfinite(sine.Smooth))
					return false;
			}
			return true;
		}
		bool KeyBytes(uint64_t &bytes, const Keyframe &key, bool retained) {
			if (!ScalarKey(key) || !Text(bytes, key.NodeId, retained) || !Text(bytes, key.Port, retained) ||
				!Text(bytes, key.Interpolation, retained) ||
				!Text(bytes, key.SourceKeyId, retained, Limits::MaximumSourceKeyIdBytes))
				return false;
			if (key.Ease &&
				(!Text(bytes, key.Ease->InType, retained) || !Text(bytes, key.Ease->OutType, retained)))
				return false;
			if (key.SourceDriver) {
				if (const auto *curve = std::get_if<KeyframeCurveDriver>(&*key.SourceDriver)) {
					const auto count = retained ? curve->Data.Anchors.capacity() : curve->Data.Anchors.size();
					if (count > UINT64_MAX / sizeof(std::array<double, 6>) ||
						!Add(bytes, count * sizeof(std::array<double, 6>)))
						return false;
				} else if (const auto *audio = std::get_if<KeyframeAudioDriver>(&*key.SourceDriver)) {
					if (!Text(bytes, audio->SourceId, retained) || !Text(bytes, audio->Metric, retained))
						return false;
				}
			}
			return true;
		}
	}

	Status ValidateSeparatedVec2(const Node &node, size_t &aggregateKeys, Diagnostic &diagnostic) {
		if (!node.SourceSeparatedVec2Animators) return Status::Ok;
		const auto fail = [&](Status code, std::string_view message, std::string_view port = {}) {
			diagnostic = {code, node.Id, std::string(port), std::string(message)};
			return code;
		};
		if (node.DynamicInputs.size() > MaximumDynamicInputsForNode(node))
			return fail(Status::LimitExceeded, "separated Vec2 dynamic declarations exceed bounds");
		const auto &inputs = node.SourceSeparatedVec2Animators->Inputs;
		size_t totalKeys = aggregateKeys;
		if (inputs.empty())
			return fail(Status::InvalidValue, "separated Vec2 storage requires a source input");
		if (inputs.size() > Limits::MaximumArrayElements ||
			inputs.size() > SourceSeparatedVec2InputCount(node))
			return fail(Status::LimitExceeded, "separated Vec2 input count exceeds declared source controls");
		std::array<std::string_view, Limits::MaximumArrayElements> ports{};
		size_t portCount = 0;
		// One bounded stack table validates both time and source identity without uncharged heap scratch.
		std::array<const Keyframe *, Limits::MaximumKeyframes> keys{};
		std::array<const Keyframe *, Limits::MaximumKeyframes> identities{};
		size_t identityCount = 0;
		for (const auto &input : inputs) {
			if (!input.Initialized && (!input.Axes[0].Keys.empty() || !input.Axes[1].Keys.empty()))
				return fail(Status::InvalidValue, "cold scalar storage must not contain keys", input.Port);
			if (!SourceSeparatedVec2Input(node, input.Port))
				return fail(
					Status::UnknownPort,
					"separated Vec2 storage has no declared two-axis source input",
					input.Port
				);
			if (std::find(ports.begin(), ports.begin() + portCount, input.Port) != ports.begin() + portCount)
				return fail(Status::DuplicateId, "separated Vec2 input is repeated", input.Port);
			ports[portCount++] = input.Port;
			for (const auto &axis : input.Axes) {
				if (totalKeys > Limits::MaximumKeyframes ||
					axis.Keys.size() > Limits::MaximumKeyframes - totalKeys)
					return fail(
						Status::LimitExceeded,
						"separated axes and ordinary keys exceed aggregate key bounds",
						input.Port
					);
				totalKeys += axis.Keys.size();
				for (size_t i = 0; i < axis.Keys.size(); ++i) {
					const auto &key = axis.Keys[i];
					if (key.NodeId != node.Id || key.Port != input.Port || !ScalarKey(key))
						return fail(
							Status::InvalidValue,
							"separated Vec2 key must target its owner and contain a finite scalar",
							input.Port
						);
					if (key.SourceKeyId.size() > Limits::MaximumSourceKeyIdBytes ||
						key.Interpolation.size() > Limits::MaximumTextBytes)
						return fail(
							Status::LimitExceeded, "separated Vec2 key text exceeds byte bounds", input.Port
						);
					keys[i] = &key;
					if (!key.SourceKeyId.empty()) identities[identityCount++] = &key;
				}
				auto end = keys.begin() + axis.Keys.size();
				std::sort(keys.begin(), end, [](const auto *a, const auto *b) {
					return CompareFrameTime(GetFrameTime(*a), GetFrameTime(*b)) < 0;
				});
				for (size_t i = 1; i < axis.Keys.size(); ++i)
					if (GetFrameTime(*keys[i - 1]) == GetFrameTime(*keys[i]) &&
						(keys[i - 1]->Interpolation != "source" || keys[i]->Interpolation != "source"))
						return fail(
							Status::DuplicateId, "separated Vec2 axis repeats a key time", input.Port
						);
				std::sort(keys.begin(), end, [](const auto *a, const auto *b) {
					return a->SourceKeyId < b->SourceKeyId;
				});
				for (size_t i = 1; i < axis.Keys.size(); ++i)
					if (!keys[i]->SourceKeyId.empty() && keys[i]->SourceKeyId == keys[i - 1]->SourceKeyId)
						return fail(
							Status::DuplicateId,
							"separated Vec2 axis repeats a source key identity",
							input.Port
						);
			}
		}
		const auto identityEnd = identities.begin() + identityCount;
		std::sort(identities.begin(), identityEnd, [](const auto *a, const auto *b) {
			return std::tie(a->Port, a->SourceKeyId) < std::tie(b->Port, b->SourceKeyId);
		});
		for (size_t i = 1; i < identityCount; ++i)
			if (identities[i - 1]->Port == identities[i]->Port &&
				identities[i - 1]->SourceKeyId == identities[i]->SourceKeyId)
				return fail(
					Status::DuplicateId, "separated axes repeat a source key identity", identities[i]->Port
				);
		aggregateKeys = totalKeys;
		return Status::Ok;
	}
	std::optional<uint64_t> SeparatedKeyBytes(const Keyframe &key, bool retained) {
		uint64_t bytes = sizeof(Keyframe);
		if (!KeyBytes(bytes, key, retained)) return std::nullopt;
		return bytes;
	}
	std::optional<uint64_t> SeparatedScalarBytes(const SourceScalarAnimator &axis, bool retained) {
		if (axis.Keys.size() > Limits::MaximumKeyframes) return std::nullopt;
		const auto slots = retained ? axis.Keys.capacity() : axis.Keys.size();
		if (slots > UINT64_MAX / sizeof(Keyframe)) return std::nullopt;
		uint64_t bytes = slots * sizeof(Keyframe);
		for (const auto &key : axis.Keys)
			if (!KeyBytes(bytes, key, retained)) return std::nullopt;
		return bytes;
	}
	std::optional<uint64_t> SeparatedAnimatorBytes(const SourceSeparatedVec2Animator &input, bool retained) {
		if (!input.Initialized && (!input.Axes[0].Keys.empty() || !input.Axes[1].Keys.empty()))
			return std::nullopt;
		uint64_t bytes = sizeof(SourceSeparatedVec2Animator);
		if (input.Port.empty() || !Text(bytes, input.Port, retained)) return std::nullopt;
		for (const auto &axis : input.Axes) {
			const auto count = SeparatedScalarBytes(axis, retained);
			if (!count || !Add(bytes, *count)) return std::nullopt;
		}
		return bytes;
	}
	std::optional<uint64_t> SeparatedVec2Bytes(const Node &node, bool retained) {
		if (!node.SourceSeparatedVec2Animators) return uint64_t{0};
		size_t count = 0;
		Diagnostic diagnostic;
		if (ValidateSeparatedVec2(node, count, diagnostic) != Status::Ok) return std::nullopt;
		uint64_t bytes = sizeof(SourceSeparatedVec2Data);
		const auto &inputs = node.SourceSeparatedVec2Animators->Inputs;
		const auto slots = retained ? inputs.capacity() : inputs.size();
		if (slots > UINT64_MAX / sizeof(SourceSeparatedVec2Animator) ||
			!Add(bytes, slots * sizeof(SourceSeparatedVec2Animator)))
			return std::nullopt;
		for (const auto &input : inputs) {
			if (!Text(bytes, input.Port, retained)) return std::nullopt;
			for (const auto &axis : input.Axes) {
				const auto keys = retained ? axis.Keys.capacity() : axis.Keys.size();
				if (keys > UINT64_MAX / sizeof(Keyframe) || !Add(bytes, keys * sizeof(Keyframe)))
					return std::nullopt;
				for (const auto &key : axis.Keys)
					if (!KeyBytes(bytes, key, retained)) return std::nullopt;
			}
		}
		return bytes;
	}

	Status SampleSeparatedScalar(
		const SourceScalarAnimator &animator,
		const AnimationTrack *track,
		const TimelineSettings *timeline,
		const EvaluationRequest &request,
		bool getterAnimated,
		bool writerAnimated,
		EvaluationBudget &budget,
		double &result,
		Diagnostic &diagnostic
	) {
		ENGINE_PROFILE("imagegraph source separated scalar");
		const auto fail = [&](Status status, std::string_view message) {
			const auto *key = animator.Keys.empty() ? nullptr : &animator.Keys.front();
			diagnostic = {status, key ? key->NodeId : "", key ? key->Port : "", std::string(message)};
			return status;
		};
		if (animator.Keys.size() > Limits::MaximumKeyframes)
			return fail(Status::LimitExceeded, "separated scalar key count exceeds bounds");
		if (!ValidFrameTime(GetFrameTime(request)))
			return fail(Status::InvalidValue, "separated scalar sample has an invalid frame time");
		if (animator.Keys.empty()) {
			if (!getterAnimated)
				return fail(Status::UnsupportedExecution, "static separated getter has no first stored key");
			result = 0;
			return Status::Ok;
		}
		for (const auto &key : animator.Keys)
			if (!ScalarKey(key))
				return fail(Status::InvalidValue, "separated animator requires finite scalar keys");
		// A static getter reads storage directly. An animated delegated getter still reads
		// the original writer's static multi-key fast path, after the lone-key driver path.
		if (!getterAnimated || (animator.Keys.size() > 1 && !writerAnimated)) {
			result = std::get<double>(animator.Keys.front().Data);
			return Status::Ok;
		}
		const uint64_t workspace = animator.Keys.size() * (sizeof(const Keyframe *) + sizeof(FrameTime)) +
								   3 * sizeof(Value) + sizeof(DriverCurveMap);
		auto charge = budget.Reserve(workspace);
		if (!charge)
			return fail(
				Status::LimitExceeded, "separated scalar sampling workspace exceeds live byte budget"
			);
		std::array<const Keyframe *, Limits::MaximumKeyframes> keys{};
		std::array<FrameTime, Limits::MaximumKeyframes> times{};
		for (size_t i = 0; i < animator.Keys.size(); ++i)
			keys[i] = &animator.Keys[i];
		const auto endKeys = keys.begin() + animator.Keys.size();
		const bool sourceStorage = std::all_of(keys.begin(), endKeys, [](const auto *key) {
			return key->Interpolation == "source";
		});
		bool ordered = true;
		for (size_t i = 1; i < animator.Keys.size(); ++i)
			if (CompareFrameTime(GetFrameTime(*keys[i - 1]), GetFrameTime(*keys[i])) >= 0) ordered = false;
		if (!sourceStorage || ordered)
			std::sort(keys.begin(), endKeys, [](const auto *a, const auto *b) {
				return CompareFrameTime(GetFrameTime(*a), GetFrameTime(*b)) < 0;
			});
		for (size_t i = 0; i < animator.Keys.size(); ++i)
			times[i] = GetFrameTime(*keys[i]);
		const Keyframe *left = keys.front();
		const Keyframe *right = nullptr;
		long double frame = FrameTimeToReal(GetFrameTime(request));
		double ratio = .5;
		bool suppressDriver = frame < FrameTimeToReal(GetFrameTime(*left));
		bool wrapping = false;
		const uint64_t totalFrames = timeline ? timeline->Frames : keys[animator.Keys.size() - 1]->Tick + 1;
		if (animator.Keys.size() > 1) {
			if (track &&
				(track->LoopRange < -1 ||
				 (track->LoopRange >= 0 && static_cast<uint64_t>(track->LoopRange) >= animator.Keys.size())))
				return fail(Status::UnsupportedExecution, "separated scalar loop range is unrepresented");
			if (track && track->End != "hold" && track->End != "loop" && track->End != "ping" &&
				track->End != "wrap")
				return fail(Status::InvalidValue, "separated scalar end mode is invalid");
			if (track && track->End == "wrap" && !timeline)
				return fail(Status::UnsupportedExecution, "separated scalar wrap needs timeline frame count");
			const KeyEnd end = !track || track->End == "hold" ? KeyEnd::Hold
							   : track->End == "loop"		  ? KeyEnd::Loop
							   : track->End == "ping"		  ? KeyEnd::Ping
															  : KeyEnd::Wrap;
			const size_t loopStart = !track || track->LoopRange < 0
										 ? 0
										 : animator.Keys.size() - 1 - static_cast<size_t>(track->LoopRange);
			if (sourceStorage && !ordered) {
				// updateKeyMap writes intervals in stored order and overwrites the final tail.
				// setAnim can append a duplicate time without calling recalculateKeys.
				if (request.Subframe != 0 ||
					std::any_of(keys.begin(), endKeys, [](const auto *key) { return key->Subframe != 0; }))
					return fail(
						Status::UnsupportedExecution,
						"source unordered fractional key-map index coercion is unverified"
					);
				const auto timeOf = [](const Keyframe *key) { return FrameTimeToReal(GetFrameTime(*key)); };
				const long double first = timeOf(keys[loopStart]),
								  last = timeOf(keys[animator.Keys.size() - 1]);
				const long double duration = last - first;
				const auto mod = [](long double value, long double divisor) {
					return divisor == 0 ? 0.L : std::fmod(value, divisor);
				};
				if (frame > last) {
					if (end == KeyEnd::Loop)
						frame = first + mod(frame - last, duration + 1);
					else if (end == KeyEnd::Ping) {
						const auto at = mod(frame - first, duration * 2);
						frame = at < duration ? first + at : first + duration * 2 - at;
					}
				}
				if (!std::isfinite(frame) || frame != std::trunc(frame))
					return fail(
						Status::UnsupportedExecution,
						"source unordered key-map lookup requires an integer frame"
					);
				enum class Position { Undefined, Before, Between, After };
				Position position = Position::Undefined;
				size_t interval = 0;
				const long double extent = std::max(static_cast<long double>(totalFrames), last);
				if (frame >= extent)
					position = Position::After;
				else if (frame <= 0)
					position = Position::Before;
				else {
					if (frame < timeOf(keys.front())) position = Position::Before;
					for (size_t i = 1; i < animator.Keys.size(); ++i)
						if (frame >= std::max(0.L, timeOf(keys[i - 1])) && frame < timeOf(keys[i])) {
							position = Position::Between;
							interval = i - 1;
						}
					if (frame >= std::max(0.L, last)) position = Position::After;
				}
				if (position == Position::Undefined)
					return fail(
						Status::UnsupportedExecution, "source unordered key-map cell has no owned value"
					);
				if (position == Position::Between) {
					left = keys[interval];
					right = keys[interval + 1];
					const long double span = timeOf(right) - timeOf(left);
					if (span <= 0)
						return fail(Status::InvalidValue, "source stored key interval has no duration");
					ratio = static_cast<double>((frame - timeOf(left)) / span);
					suppressDriver = false;
				} else if (end == KeyEnd::Wrap) {
					left = keys[animator.Keys.size() - 1];
					right = keys.front();
					const long double span = totalFrames - last + timeOf(right);
					if (!std::isfinite(span) || span <= 0)
						return fail(Status::InvalidValue, "source stored wrap interval has no duration");
					ratio = static_cast<double>(
						(position == Position::Before ? totalFrames - last + frame : frame - last) / span
					);
					wrapping = true;
					suppressDriver = false;
				} else {
					left = position == Position::Before ? keys.front() : keys[animator.Keys.size() - 1];
					right = nullptr;
					suppressDriver = position == Position::Before;
				}
			} else {

				FractionalKeySelection selection;
				FrameTime mapped;
				if (!SelectFrameTimes(
						{times.data(), animator.Keys.size()},
						GetFrameTime(request),
						totalFrames,
						end,
						loopStart,
						selection,
						mapped
					))
					return fail(Status::InvalidValue, "separated scalar time is outside the bounded track");
				frame = FrameTimeToReal(mapped);
				left = keys[selection.From];
				if (selection.To != selection.From) right = keys[selection.To];
				ratio = selection.Ratio;
				wrapping =
					end == KeyEnd::Wrap && selection.From == animator.Keys.size() - 1 && selection.To == 0;
				suppressDriver = !wrapping && frame < FrameTimeToReal(GetFrameTime(*left));
				if (left->Interpolation == "source" && frame <= 0) {
					if (end == KeyEnd::Wrap) {
						left = keys[animator.Keys.size() - 1];
						right = keys.front();
						const long double span = totalFrames - FrameTimeToReal(GetFrameTime(*left)) +
												 FrameTimeToReal(GetFrameTime(*right));
						if (!std::isfinite(span) || span <= 0)
							return fail(
								Status::InvalidValue, "separated scalar wrap interval has no duration"
							);
						ratio = static_cast<double>(
							(totalFrames - FrameTimeToReal(GetFrameTime(*left)) + frame) / span
						);
						wrapping = true;
						suppressDriver = false;
					} else {
						left = keys.front();
						right = nullptr;
						suppressDriver = true;
					}
				}
				if (right && left->Interpolation == "source" && frame > 0 &&
					std::any_of(keys.begin(), endKeys, [](const auto *key) { return key->Subframe != 0; }) &&
					frame != FrameTimeToReal(GetFrameTime(*left)) &&
					frame != FrameTimeToReal(GetFrameTime(*right)))
					return fail(
						Status::UnsupportedExecution, "source fractional key-map index coercion is unverified"
					);
			}
		}
		double ease = ratio;
		if (right && left->Interpolation == "source") {
			const auto side = [](std::string_view type) {
				return type == "bezier" ? CurveSide::Bezier
					   : type == "cut"	? CurveSide::Cut
										: CurveSide::Linear;
			};
			KeyBlend blend;
			if (!EaseKeys(
					{side(left->Ease->OutType),
					 side(right->Ease->InType),
					 left->Ease->Out.X,
					 left->Ease->Out.Y,
					 right->Ease->In.X,
					 right->Ease->In.Y},
					ratio,
					blend,
					wrapping
				))
				return fail(Status::InvalidValue, "separated scalar easing cannot be evaluated");
			if (blend.Choice != KeyChoice::Blend) {
				left = blend.Choice == KeyChoice::From ? left : right;
				right = nullptr;
				suppressDriver = true;
			} else
				ease = blend.Ratio;
		} else if (right && left->Interpolation == "cubic")
			return fail(
				Status::UnsupportedExecution, "cubic separated scalar key needs authored tangent controls"
			);
		else if (right && left->Interpolation == "step")
			right = nullptr;
		Value candidate;
		const KeyframeSourceDriver *driver =
			!suppressDriver && left->SourceDriver ? &*left->SourceDriver : nullptr;
		KeyframeSourceDriver sine;
		if (!suppressDriver && left->SineDriver) {
			sine = *left->SineDriver;
			driver = &sine;
		}
		const Status status = ApplySourceDriver(
			driver,
			left->Data,
			right ? right->Data : left->Data,
			ease,
			ratio,
			static_cast<double>(frame),
			right != nullptr,
			-1,
			candidate,
			static_cast<double>(totalFrames),
			false,
			&request
		);
		const auto *scalar = std::get_if<double>(&candidate);
		if (status != Status::Ok || !scalar || !std::isfinite(*scalar))
			return fail(
				status == Status::Ok ? Status::InvalidValue : status,
				"separated scalar driver has no finite numeric result"
			);
		result = *scalar;
		return Status::Ok;
	}

}
