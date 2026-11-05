#pragma once

#include "PointerDecision.hpp"

#include <engine/ecs/Store.hpp>
#include <engine/gui/Components.hpp>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace engine::gui::detail {
	struct CaptureOwner {
		ecs::Entity Target, Collector;
		bool operator==(const CaptureOwner &) const = default;
	};

	// Router supplies collector IDs through its existing CollectorOf rule.
	struct CaptureState {
		CaptureOwner Holding, Dragging, Detector, Dragged, Canvas;
		ecs::Entity Over;
		core::Vector2 Last, DragFrom;
		core::UDim2 DragStart;
		float DragAngle = 0;
		bool Started = false, WasDown = false;
		ecs::Entity HoldingCollection;
		std::string_view HoldingKey;
		uint32_t HoldingIndex = UINT32_MAX;
		bool ModalOverflow = false;
		std::span<const CaptureOwner> Modals, SavedFocus, SavedSelection;
	};
	struct CaptureStateStorage {
		std::array<CaptureOwner, 64> Modals{}, SavedFocus{}, SavedSelection{};
		CaptureState State;
		bool Valid = false;
	};
	struct SavedFocusContinuity {
		CaptureOwner Owner;
		bool Alive = false, HasEntry = false, ClearTextOnFocus = false, HasLabel = false;
		std::string Text;
	};
	struct SavedSelectionContinuity {
		CaptureOwner Owner;
		bool Alive = false, HasElement = false, Visible = false, Selectable = false, Interactable = false;
	};
	// Values read by canvas dragging, elastic eligibility and event-local coordinates.
	struct CanvasCaptureData {
		bool HasScrolling = false, HasState = false, HasResolved = false;
		bool Enabled = false;
		ScrollingDirection Direction = ScrollingDirection::Y;
		ElasticBehavior Elastic = ElasticBehavior::WhenScrollable;
		std::array<uint32_t, 8> Bits{};
		bool operator==(const CanvasCaptureData &) const = default;
	};

	inline CanvasCaptureData CaptureCanvasData(const ecs::Store &store, ecs::Entity canvas) {
		CanvasCaptureData result;
		if (const auto *scrolling = store.Get<Scrolling>(canvas)) {
			result.HasScrolling = true;
			result.Enabled = scrolling->Enabled;
			result.Direction = scrolling->Direction;
			result.Elastic = scrolling->Elastic;
			result.Bits[0] = std::bit_cast<uint32_t>(scrolling->CanvasPosition.X);
			result.Bits[1] = std::bit_cast<uint32_t>(scrolling->CanvasPosition.Y);
		}
		if (const auto *state = store.Get<ScrollState>(canvas)) {
			result.HasState = true;
			result.Bits[2] = std::bit_cast<uint32_t>(state->CanvasSize.X);
			result.Bits[3] = std::bit_cast<uint32_t>(state->CanvasSize.Y);
			result.Bits[4] = std::bit_cast<uint32_t>(state->WindowSize.X);
			result.Bits[5] = std::bit_cast<uint32_t>(state->WindowSize.Y);
		}
		if (const auto *resolved = store.Get<Resolved>(canvas)) {
			result.HasResolved = true;
			result.Bits[6] = std::bit_cast<uint32_t>(resolved->AbsolutePosition.X);
			result.Bits[7] = std::bit_cast<uint32_t>(resolved->AbsolutePosition.Y);
		}
		return result;
	}

	// Inputs read while continuing a detector drag, refreshed after ordered input.
	struct DetectorCaptureData {
		ecs::Entity Parent, Bound;
		bool HasDetector = false, HasElement = false, HasResolved = false;
		bool HasParentResolved = false, HasBoundResolved = false, IsDescendant = false;
		DragStyle Style = DragStyle::TranslatePlane;
		DragResponse Response = DragResponse::Offset;
		std::array<uint32_t, 23> Bits{};
		bool operator==(const DetectorCaptureData &) const = default;
	};

	inline DetectorCaptureData
	CaptureDetectorData(const ecs::Store &store, ecs::Entity detectorEntity, ecs::Entity dragged) {
		DetectorCaptureData result;
		if (detectorEntity == ecs::NULL_ENTITY) return result;
		result.Parent = store.ParentOf(dragged);
		result.IsDescendant = store.IsDescendantOf(detectorEntity, dragged);
		const auto bits = [](float value) { return std::bit_cast<uint32_t>(value); };
		if (const auto *detector = store.Get<DragDetector>(detectorEntity)) {
			result.HasDetector = true;
			result.Bound = detector->BoundingUI;
			result.Style = detector->Style;
			result.Response = detector->Response;
			result.Bits[0] = bits(detector->Axis.X);
			result.Bits[1] = bits(detector->Axis.Y);
			result.Bits[2] = bits(detector->MinTranslation.X);
			result.Bits[3] = bits(detector->MinTranslation.Y);
			result.Bits[4] = bits(detector->MaxTranslation.X);
			result.Bits[5] = bits(detector->MaxTranslation.Y);
		}
		if (const auto *element = store.Get<Element>(dragged)) {
			result.HasElement = true;
			result.Bits[6] = bits(element->Position.X.Scale);
			result.Bits[7] = bits(element->Position.X.Offset);
			result.Bits[8] = bits(element->Position.Y.Scale);
			result.Bits[9] = bits(element->Position.Y.Offset);
			result.Bits[10] = bits(element->Rotation);
		}
		const auto copyRect = [&](ecs::Entity entity, size_t index, bool &present) {
			if (const auto *resolved = store.Get<Resolved>(entity)) {
				present = true;
				result.Bits[index] = bits(resolved->AbsolutePosition.X);
				result.Bits[index + 1] = bits(resolved->AbsolutePosition.Y);
				result.Bits[index + 2] = bits(resolved->AbsoluteSize.X);
				result.Bits[index + 3] = bits(resolved->AbsoluteSize.Y);
			}
		};
		copyRect(dragged, 11, result.HasResolved);
		copyRect(result.Parent, 15, result.HasParentResolved);
		copyRect(result.Bound, 19, result.HasBoundResolved);
		return result;
	}

	struct CaptureContinuity {
		uint64_t StoreIdentity = 0;
		uint64_t RouterIncarnation = 0;
		size_t RetainedBytes = 0;
		CaptureOwner Holding, Dragging, Detector, Dragged, Canvas;
		ecs::Entity Over;
		std::array<uint32_t, 2> LastBits{};
		std::array<uint32_t, 7> DragBits{};
		bool Started = false, WasDown = false;
		CanvasCaptureData CanvasData;
		DetectorCaptureData DetectorData;
		ecs::Entity HoldingCollection;
		std::string HoldingKey;
		uint32_t HoldingIndex = UINT32_MAX;
		bool HasCanvasMotion = false, CanvasHeld = false;
		uint64_t CanvasReleasedAtBits = 0;
		std::array<uint32_t, 8> CanvasMotionBits{};
		bool ModalOverflow = false;
		std::vector<CaptureOwner> Modals;
		std::vector<SavedFocusContinuity> SavedFocus;
		std::vector<SavedSelectionContinuity> SavedSelection;
	};

	inline std::array<uint32_t, 8> CaptureCanvasMotionBits(const ScrollMotion &motion) {
		return {
			std::bit_cast<uint32_t>(motion.Pull.X),
			std::bit_cast<uint32_t>(motion.Pull.Y),
			std::bit_cast<uint32_t>(motion.Released.X),
			std::bit_cast<uint32_t>(motion.Released.Y),
			std::bit_cast<uint32_t>(motion.LastPoint.X),
			std::bit_cast<uint32_t>(motion.LastPoint.Y),
			std::bit_cast<uint32_t>(motion.Overshoot.X),
			std::bit_cast<uint32_t>(motion.Overshoot.Y)
		};
	}

	// Caller keeps the Router and Store unchanged throughout bounded capture.
	inline std::optional<CaptureContinuity> CaptureRouterContinuity(
		const ecs::Store &store, const CaptureState &state, size_t byteBudget, uint64_t routerIncarnation
	) {
		constexpr size_t MAX_MODAL_CONTEXTS = 64, ALLOCATION_OVERHEAD = 64;
		if (!routerIncarnation || state.HoldingKey.size() > MAX_POINTER_KEY_BYTES ||
			state.Modals.size() > MAX_MODAL_CONTEXTS || state.SavedFocus.size() != state.Modals.size() ||
			state.SavedSelection.size() != state.Modals.size() || byteBudget < sizeof(CaptureContinuity))
			return {};
		size_t remaining = byteBudget - sizeof(CaptureContinuity);
		if (state.Modals.size() > remaining / sizeof(CaptureOwner)) return {};
		remaining -= state.Modals.size() * sizeof(CaptureOwner);
		if (state.SavedFocus.size() > remaining / sizeof(SavedFocusContinuity)) return {};
		remaining -= state.SavedFocus.size() * sizeof(SavedFocusContinuity);
		if (state.SavedSelection.size() > remaining / sizeof(SavedSelectionContinuity)) return {};
		remaining -= state.SavedSelection.size() * sizeof(SavedSelectionContinuity);
		if (state.HoldingKey.size() > remaining) return {};
		remaining -= state.HoldingKey.size();
		for (const auto &focus : state.SavedFocus) {
			const auto *label = store.Get<Label>(focus.Target);
			if (label == nullptr) continue;
			if (label->Text.size() > remaining) return {};
			remaining -= label->Text.size();
		}
		try {
			CaptureContinuity result;
			result.StoreIdentity = store.Identity();
			result.RouterIncarnation = routerIncarnation;
			result.Holding = state.Holding;
			result.Dragging = state.Dragging;
			result.Detector = state.Detector;
			result.Dragged = state.Dragged;
			result.Canvas = state.Canvas;
			result.Over = state.Over;
			result.LastBits = {std::bit_cast<uint32_t>(state.Last.X), std::bit_cast<uint32_t>(state.Last.Y)};
			result.DragBits = {
				std::bit_cast<uint32_t>(state.DragFrom.X),
				std::bit_cast<uint32_t>(state.DragFrom.Y),
				std::bit_cast<uint32_t>(state.DragStart.X.Scale),
				std::bit_cast<uint32_t>(state.DragStart.X.Offset),
				std::bit_cast<uint32_t>(state.DragStart.Y.Scale),
				std::bit_cast<uint32_t>(state.DragStart.Y.Offset),
				std::bit_cast<uint32_t>(state.DragAngle)
			};
			result.Started = state.Started;
			result.WasDown = state.WasDown;
			result.CanvasData = CaptureCanvasData(store, state.Canvas.Target);
			result.DetectorData = CaptureDetectorData(store, state.Detector.Target, state.Dragged.Target);
			result.HoldingCollection = state.HoldingCollection;
			result.HoldingKey.assign(state.HoldingKey);
			result.HoldingIndex = state.HoldingIndex;
			result.ModalOverflow = state.ModalOverflow;
			result.Modals.assign(state.Modals.begin(), state.Modals.end());
			if (const auto *motion = store.Get<ScrollMotion>(state.Canvas.Target)) {
				result.HasCanvasMotion = true;
				result.CanvasHeld = motion->Held;
				result.CanvasReleasedAtBits = std::bit_cast<uint64_t>(motion->ReleasedAt);
				result.CanvasMotionBits = CaptureCanvasMotionBits(*motion);
			}
			result.SavedFocus.reserve(state.SavedFocus.size());
			for (const auto &focus : state.SavedFocus) {
				SavedFocusContinuity item;
				item.Owner = focus;
				item.Alive = store.Alive(focus.Target);
				if (const auto *entry = store.Get<Entry>(focus.Target)) {
					item.HasEntry = true;
					item.ClearTextOnFocus = entry->ClearTextOnFocus;
				}
				if (const auto *label = store.Get<Label>(focus.Target)) {
					item.HasLabel = true;
					item.Text = label->Text;
				}
				result.SavedFocus.push_back(std::move(item));
			}
			result.SavedSelection.reserve(state.SavedSelection.size());
			for (const auto &selection : state.SavedSelection) {
				SavedSelectionContinuity item;
				item.Owner = selection;
				item.Alive = store.Alive(selection.Target);
				if (const auto *element = store.Get<Element>(selection.Target)) {
					item.HasElement = true;
					item.Visible = element->Visible;
					item.Selectable = element->Selectable;
					item.Interactable = element->Interactable;
				}
				result.SavedSelection.push_back(item);
			}
			remaining = byteBudget - sizeof(CaptureContinuity);
			const auto charge = [&](size_t count) {
				if (count > remaining) return false;
				remaining -= count;
				return true;
			};
			const auto chargeAllocation = [&](size_t bytes) {
				return bytes == 0 || (charge(bytes) && charge(ALLOCATION_OVERHEAD));
			};
			if (result.Modals.capacity() > remaining / sizeof(CaptureOwner) ||
				!chargeAllocation(result.Modals.capacity() * sizeof(CaptureOwner)) ||
				result.SavedFocus.capacity() > remaining / sizeof(SavedFocusContinuity) ||
				!chargeAllocation(result.SavedFocus.capacity() * sizeof(SavedFocusContinuity)) ||
				result.SavedSelection.capacity() > remaining / sizeof(SavedSelectionContinuity) ||
				!chargeAllocation(result.SavedSelection.capacity() * sizeof(SavedSelectionContinuity)) ||
				result.HoldingKey.capacity() >= remaining || !charge(result.HoldingKey.capacity() + 1) ||
				!charge(ALLOCATION_OVERHEAD))
				return {};
			for (const auto &focus : result.SavedFocus)
				if (focus.Text.capacity() >= remaining || !charge(focus.Text.capacity() + 1) ||
					!charge(ALLOCATION_OVERHEAD))
					return {};
			if (!charge(ALLOCATION_OVERHEAD)) return {};
			result.RetainedBytes = byteBudget - remaining;
			return result;
		} catch (const std::bad_alloc &) {
			return {};
		}
	}

	// Allocation-free comparison; collector IDs must be freshly resolved.
	inline bool MatchesRouterContinuity(
		const ecs::Store &store,
		const CaptureContinuity &witness,
		const CaptureState &state,
		uint64_t routerIncarnation
	) {
		if (!routerIncarnation || routerIncarnation != witness.RouterIncarnation ||
			store.Identity() != witness.StoreIdentity || state.Holding != witness.Holding ||
			state.Dragging != witness.Dragging || state.Detector != witness.Detector ||
			state.Dragged != witness.Dragged || state.Canvas != witness.Canvas ||
			state.Over != witness.Over || state.Started != witness.Started ||
			std::array<uint32_t, 7>{
				std::bit_cast<uint32_t>(state.DragFrom.X),
				std::bit_cast<uint32_t>(state.DragFrom.Y),
				std::bit_cast<uint32_t>(state.DragStart.X.Scale),
				std::bit_cast<uint32_t>(state.DragStart.X.Offset),
				std::bit_cast<uint32_t>(state.DragStart.Y.Scale),
				std::bit_cast<uint32_t>(state.DragStart.Y.Offset),
				std::bit_cast<uint32_t>(state.DragAngle)
			} != witness.DragBits ||
			state.WasDown != witness.WasDown ||
			std::bit_cast<uint32_t>(state.Last.X) != witness.LastBits[0] ||
			std::bit_cast<uint32_t>(state.Last.Y) != witness.LastBits[1] ||
			CaptureCanvasData(store, state.Canvas.Target) != witness.CanvasData ||
			CaptureDetectorData(store, state.Detector.Target, state.Dragged.Target) != witness.DetectorData ||
			state.HoldingCollection != witness.HoldingCollection || state.HoldingKey != witness.HoldingKey ||
			state.HoldingIndex != witness.HoldingIndex || state.ModalOverflow != witness.ModalOverflow ||
			state.Modals.size() != witness.Modals.size() ||
			state.SavedFocus.size() != witness.SavedFocus.size() ||
			state.SavedSelection.size() != witness.SavedSelection.size())
			return false;
		for (size_t i = 0; i < state.Modals.size(); ++i)
			if (state.Modals[i] != witness.Modals[i]) return false;
		const auto *motion = store.Get<ScrollMotion>(state.Canvas.Target);
		if ((motion != nullptr) != witness.HasCanvasMotion ||
			(motion && (motion->Held != witness.CanvasHeld ||
						std::bit_cast<uint64_t>(motion->ReleasedAt) != witness.CanvasReleasedAtBits ||
						CaptureCanvasMotionBits(*motion) != witness.CanvasMotionBits)))
			return false;
		for (size_t i = 0; i < state.SavedFocus.size(); ++i) {
			const auto &saved = witness.SavedFocus[i];
			const auto &current = state.SavedFocus[i];
			if (saved.Owner != current || saved.Alive != store.Alive(current.Target)) return false;
			const auto *entry = store.Get<Entry>(current.Target);
			const auto *label = store.Get<Label>(current.Target);
			if ((entry != nullptr) != saved.HasEntry || (label != nullptr) != saved.HasLabel ||
				(entry && entry->ClearTextOnFocus != saved.ClearTextOnFocus) ||
				(label && label->Text != saved.Text))
				return false;
		}
		for (size_t i = 0; i < state.SavedSelection.size(); ++i) {
			const auto &saved = witness.SavedSelection[i];
			const auto &current = state.SavedSelection[i];
			if (saved.Owner != current || saved.Alive != store.Alive(current.Target)) return false;
			const auto *element = store.Get<Element>(current.Target);
			if ((element != nullptr) != saved.HasElement ||
				(element && (element->Visible != saved.Visible || element->Selectable != saved.Selectable ||
							 element->Interactable != saved.Interactable)))
				return false;
		}
		return true;
	}
}
