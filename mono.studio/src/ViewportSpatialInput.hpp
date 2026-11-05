#pragma once

#include <engine/gui/Input.hpp>
#include <engine/render/AdornmentGeometry.hpp>
#include <engine/render/SpatialInputQueue.hpp>

#include <client/Accessibility.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

namespace studio {
	enum class ViewportInputKind : uint8_t {
		FrameStart,
		Accessibility,
		Adornment,
		Keyboard,
		AuthoringCommit
	};
	enum ViewportInputKey : uint32_t {
		KEY_UP = 1u << 0,
		KEY_DOWN = 1u << 1,
		KEY_LEFT = 1u << 2,
		KEY_RIGHT = 1u << 3,
		KEY_ENTER = 1u << 4,
		KEY_SPACE = 1u << 5,
		KEY_ESCAPE = 1u << 6,
		PAD_UP = 1u << 7,
		PAD_DOWN = 1u << 8,
		PAD_LEFT = 1u << 9,
		PAD_RIGHT = 1u << 10,
		PAD_ACTIVATE = 1u << 11,
		PAD_CANCEL = 1u << 12,
		KEY_BACKSPACE = 1u << 13,
		KEY_CARET_LEFT = 1u << 14,
		KEY_CARET_RIGHT = 1u << 15,
		KEY_SHIFT = 1u << 16,
	};

	// Distinguishes external selection changes from earlier picks in this queue.
	class ViewportSelectionFence {
	  public:
		uint64_t Capture(uint64_t revision) {
			if (!Initialised) {
				Initialised = true;
				LastRevision = revision;
			} else if (LastRevision != revision)
				Advance(revision);
			return Epoch;
		}
		bool Accept(uint64_t epoch, uint64_t revision) {
			if (!Initialised) return false;
			if (LastRevision != revision) {
				Advance(revision);
				return false;
			}
			return Epoch != 0 && epoch == Epoch;
		}
		void Commit(uint64_t revision) {
			LastRevision = revision;
		}

	  private:
		void Advance(uint64_t revision) {
			LastRevision = revision;
			if (Epoch != 0) ++Epoch;
		}
		uint64_t Epoch = 1;
		uint64_t LastRevision = 0;
		bool Initialised = false;
	};

	// Only value fields cross the asynchronous input boundary. The trailing
	// bytes of a keyboard record are the owned UTF-8 input for that batch.
	struct ViewportInputRecord {
		ViewportInputKind Kind = ViewportInputKind::FrameStart;
		engine::ecs::Entity Target;
		engine::ecs::Entity Collector;
		uint64_t WorldEpoch = 0;
		uint64_t SelectionEpoch = 0;
		uint64_t NativeNodeId = 0;
		uint32_t NativeAction = 0;
		uint8_t Tool = 0;
		bool AddSelection = false;
		engine::gui::SemanticAction Action = engine::gui::SemanticAction::Up;
		engine::render::AdornmentPointer Adornment;
		uint32_t Keys = 0;
	};

	inline std::vector<std::byte>
	EncodeViewportInput(const ViewportInputRecord &record, std::string_view text = {}) {
		constexpr size_t fixed = 2 * sizeof(uint8_t) + 5 * sizeof(uint64_t) + 2 * sizeof(uint32_t) +
								 2 * sizeof(float) + 2 * sizeof(uint8_t);
		constexpr size_t limit = engine::render::SpatialInputQueue::MAX_PAYLOAD_BYTES;
		if (text.size() > limit - fixed || (record.Kind != ViewportInputKind::Keyboard && !text.empty()) ||
			!std::isfinite(record.Adornment.Position.X) || !std::isfinite(record.Adornment.Position.Y))
			return {};
		try {
			std::vector<std::byte> bytes(fixed + text.size());
			size_t at = 0;
			const auto write = [&](const auto &value) {
				std::memcpy(bytes.data() + at, &value, sizeof(value));
				at += sizeof(value);
			};
			const uint8_t kind = static_cast<uint8_t>(record.Kind),
						  action = static_cast<uint8_t>(record.Action);
			const uint8_t flags = static_cast<uint8_t>(
				(record.Adornment.PrimaryDown ? 1 : 0) | (record.Adornment.SecondaryDown ? 2 : 0) |
				(record.Adornment.Moved ? 4 : 0) | (record.Adornment.Inside ? 8 : 0) |
				(record.AddSelection ? 16 : 0)
			);
			write(kind);
			write(action);
			write(record.Target.Id);
			write(record.Collector.Id);
			write(record.WorldEpoch);
			write(record.SelectionEpoch);
			write(record.NativeNodeId);
			write(record.NativeAction);
			write(record.Tool);
			write(record.Keys);
			write(record.Adornment.Position.X);
			write(record.Adornment.Position.Y);
			write(flags);
			if (!text.empty()) std::memcpy(bytes.data() + at, text.data(), text.size());
			return bytes;
		} catch (const std::bad_alloc &) {
			return {};
		}
	}
	inline bool DecodeViewportInput(
		std::span<const std::byte> bytes, ViewportInputRecord &record, std::string_view &text
	) {
		if (bytes.size() > engine::render::SpatialInputQueue::MAX_PAYLOAD_BYTES) return false;
		size_t at = 0;
		const auto read = [&](auto &value) {
			if (sizeof(value) > bytes.size() - at) return false;
			std::memcpy(&value, bytes.data() + at, sizeof(value));
			at += sizeof(value);
			return true;
		};
		ViewportInputRecord decoded;
		uint8_t kind = 0, action = 0, flags = 0;
		if (!read(kind) || !read(action) || !read(decoded.Target.Id) || !read(decoded.Collector.Id) ||
			!read(decoded.WorldEpoch) || !read(decoded.SelectionEpoch) || !read(decoded.NativeNodeId) ||
			!read(decoded.NativeAction) || !read(decoded.Tool) || !read(decoded.Keys) ||
			!read(decoded.Adornment.Position.X) || !read(decoded.Adornment.Position.Y) || !read(flags))
			return false;
		if (kind > static_cast<uint8_t>(ViewportInputKind::AuthoringCommit) ||
			action > static_cast<uint8_t>(engine::gui::SemanticAction::Cancel) || (flags & ~uint8_t{31}) ||
			(decoded.Keys & ~((1u << 17) - 1)) || !std::isfinite(decoded.Adornment.Position.X) ||
			!std::isfinite(decoded.Adornment.Position.Y))
			return false;
		decoded.Kind = static_cast<ViewportInputKind>(kind);
		decoded.Action = static_cast<engine::gui::SemanticAction>(action);
		decoded.Adornment.PrimaryDown = flags & 1;
		decoded.Adornment.SecondaryDown = flags & 2;
		decoded.Adornment.Moved = flags & 4;
		decoded.Adornment.Inside = flags & 8;
		decoded.AddSelection = flags & 16;
		if (decoded.Kind != ViewportInputKind::Keyboard && at != bytes.size()) return false;
		text = std::string_view(reinterpret_cast<const char *>(bytes.data() + at), bytes.size() - at);
		record = decoded;
		return true;
	}

	// Preserve the native origin and attach its rejection observer when queuing.
	inline bool EnqueueViewportAccessibility(
		engine::render::SpatialInputQueue &queue,
		uint64_t hostToken,
		const client::AccessibilityAction &action,
		engine::render::SpatialInputQueue::OpaqueDiscardObserver observer
	) {
		if (action.Status != client::AccessibilityActionStatus::Ready || action.FrameSequence == 0)
			return false;
		ViewportInputRecord record;
		record.Kind = ViewportInputKind::Accessibility;
		record.Target = action.Target;
		record.Collector = action.Collector;
		record.WorldEpoch = action.WorldEpoch;
		record.NativeNodeId = action.NativeNodeId;
		record.NativeAction = action.NativeAction;
		record.Action = action.Action;
		const auto payload = EncodeViewportInput(record);
		return !payload.empty() &&
			   queue.EnqueueOpaque(action.FrameSequence, hostToken, payload, false, 0, observer);
	}

} // namespace studio
