#pragma once

#include <engine/input/Translate.hpp>
#include <engine/render/AdornmentGeometry.hpp>

#include <client/Accessibility.hpp>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

namespace client {
	// Text views borrow the caller's bytes. Encoded records own those bytes.
	struct DeferredEdit {
		engine::input::PresentationEditKind Kind = engine::input::PresentationEditKind::Text;
		std::string_view Text;
		bool Extend = false;
	};
	struct DeferredComposition {
		std::string_view Text;
		int32_t Start = -1;
		int32_t Length = -1;
		uint64_t Revision = 0;
		bool Apply = false;
	};
	struct DeferredAdornment {
		engine::render::AdornmentPointer Pointer;
		float Width = 0.0f;
		float Height = 0.0f;
	};
	using PresentationEvent = std::variant<
		AccessibilityAction,
		engine::scene::KeyCode,
		engine::scene::ControllerState,
		DeferredEdit,
		DeferredComposition,
		DeferredAdornment>;

	// The bytes are process-local, bounded and owned by the queue after admission.
	std::optional<std::vector<std::byte>> EncodePresentationEvent(const PresentationEvent &event);
	// Text views in the result remain valid only while bytes remains alive.
	std::optional<PresentationEvent> DecodePresentationEvent(std::span<const std::byte> bytes);
}
