#include "PresentationEventCodec.hpp"

#include <bit>
#include <cmath>
#include <cstring>
#include <new>
#include <type_traits>

namespace client {
	namespace {
		using Bytes = std::vector<std::byte>;
		template <class T> void Put(Bytes &bytes, T value) {
			static_assert(std::is_integral_v<T>);
			const size_t offset = bytes.size();
			bytes.resize(offset + sizeof(T));
			std::memcpy(bytes.data() + offset, &value, sizeof(T));
		}
		void PutFloat(Bytes &bytes, float value) {
			Put(bytes, std::bit_cast<uint32_t>(value));
		}
		void PutText(Bytes &bytes, std::string_view text) {
			Put(bytes, static_cast<uint32_t>(text.size()));
			const auto *start = reinterpret_cast<const std::byte *>(text.data());
			if (!text.empty()) bytes.insert(bytes.end(), start, start + text.size());
		}
		struct Reader {
			std::span<const std::byte> Bytes;
			template <class T> bool Get(T &value) {
				static_assert(std::is_integral_v<T>);
				if (Bytes.size() < sizeof(T)) return false;
				std::memcpy(&value, Bytes.data(), sizeof(T));
				Bytes = Bytes.subspan(sizeof(T));
				return true;
			}
			bool Float(float &value) {
				uint32_t bits = 0;
				if (!Get(bits)) return false;
				value = std::bit_cast<float>(bits);
				return std::isfinite(value);
			}
			bool Bool(bool &value) {
				uint8_t encoded = 0;
				if (!Get(encoded) || encoded > 1) return false;
				value = encoded != 0;
				return true;
			}
			bool Text(std::string_view &text, size_t limit) {
				uint32_t length = 0;
				if (!Get(length) || length > limit || length > Bytes.size()) return false;
				text = {reinterpret_cast<const char *>(Bytes.data()), length};
				Bytes = Bytes.subspan(length);
				return true;
			}
		};
	}

	std::optional<Bytes> EncodePresentationEvent(const PresentationEvent &event) try {
		Bytes bytes;
		Put(bytes, static_cast<uint8_t>(event.index()));
		bool valid = true;
		std::visit(
			[&](const auto &value) {
				using T = std::decay_t<decltype(value)>;
				if constexpr (std::is_same_v<T, AccessibilityAction>) {
					Put(bytes, value.Target.Id);
					Put(bytes, value.Collector.Id);
					Put(bytes, value.WorldEpoch);
					Put(bytes, value.FrameSequence);
					Put(bytes, value.NativeNodeId);
					Put(bytes, value.NativeAction);
					Put(bytes, static_cast<uint8_t>(value.Status));
					Put(bytes, value.RejectedCount);
					Put(bytes, static_cast<uint8_t>(value.Action));
				} else if constexpr (std::is_same_v<T, engine::scene::KeyCode>) {
					Put(bytes, static_cast<uint16_t>(value));
				} else if constexpr (std::is_same_v<T, engine::scene::ControllerState>) {
					for (const auto &slot : value.Slots) {
						Put(bytes, slot.Buttons);
						Put(bytes, slot.PreviousButtons);
						Put(bytes, slot.PressedButtons);
						for (float axis : slot.Axes)
							PutFloat(bytes, axis);
						for (float axis : slot.PreviousAxes)
							PutFloat(bytes, axis);
						Put(bytes, uint8_t(slot.Connected));
						Put(bytes, uint8_t(slot.PreviousConnected));
						Put(bytes, uint8_t(slot.Mapped));
						Put(bytes, slot.Reserved);
					}
				} else if constexpr (std::is_same_v<T, DeferredEdit>) {
					if (value.Text.size() > engine::input::PresentationInput::MAXIMUM_TEXT_BYTES) {
						valid = false;
						return;
					}
					Put(bytes, static_cast<uint8_t>(value.Kind));
					Put(bytes, uint8_t(value.Extend));
					PutText(bytes, value.Text);
				} else if constexpr (std::is_same_v<T, DeferredComposition>) {
					if (value.Text.size() > engine::input::TextComposition::MAXIMUM_BYTES) {
						valid = false;
						return;
					}
					Put(bytes, value.Start);
					Put(bytes, value.Length);
					Put(bytes, value.Revision);
					Put(bytes, uint8_t(value.Apply));
					PutText(bytes, value.Text);
				} else {
					const auto &pointer = value.Pointer;
					PutFloat(bytes, pointer.Ray.Origin.X);
					PutFloat(bytes, pointer.Ray.Origin.Y);
					PutFloat(bytes, pointer.Ray.Origin.Z);
					PutFloat(bytes, pointer.Ray.Direction.X);
					PutFloat(bytes, pointer.Ray.Direction.Y);
					PutFloat(bytes, pointer.Ray.Direction.Z);
					PutFloat(bytes, pointer.Position.X);
					PutFloat(bytes, pointer.Position.Y);
					Put(bytes, uint8_t(pointer.PrimaryDown));
					Put(bytes, uint8_t(pointer.SecondaryDown));
					Put(bytes, uint8_t(pointer.Moved));
					Put(bytes, uint8_t(pointer.Inside));
					PutFloat(bytes, value.Width);
					PutFloat(bytes, value.Height);
				}
			},
			event
		);
		if (!valid) return {};
		return bytes;
	} catch (const std::bad_alloc &) {
		return {};
	}

	std::optional<PresentationEvent> DecodePresentationEvent(std::span<const std::byte> bytes) {
		Reader reader{bytes};
		uint8_t kind = 0;
		if (!reader.Get(kind)) return {};
		PresentationEvent event;
		switch (kind) {
		case 0: {
			AccessibilityAction value;
			uint64_t target = 0, collector = 0;
			uint8_t action = 0, status = 0;
			if (!reader.Get(target) || !reader.Get(collector) || !reader.Get(value.WorldEpoch) ||
				!reader.Get(value.FrameSequence) || !reader.Get(value.NativeNodeId) ||
				!reader.Get(value.NativeAction) || !reader.Get(status) ||
				status > uint8_t(AccessibilityActionStatus::Unsupported) ||
				!reader.Get(value.RejectedCount) || !reader.Get(action) ||
				action > uint8_t(engine::gui::SemanticAction::Cancel))
				return {};
			value.Target = engine::ecs::Entity(target);
			value.Collector = engine::ecs::Entity(collector);
			value.Status = static_cast<AccessibilityActionStatus>(status);
			value.Action = static_cast<engine::gui::SemanticAction>(action);
			event = value;
			break;
		}
		case 1: {
			uint16_t key = 0;
			if (!reader.Get(key) || key >= uint16_t(engine::scene::KeyCode::Count)) return {};
			event = static_cast<engine::scene::KeyCode>(key);
			break;
		}
		case 2: {
			engine::scene::ControllerState value;
			for (auto &slot : value.Slots) {
				if (!reader.Get(slot.Buttons) || !reader.Get(slot.PreviousButtons) ||
					!reader.Get(slot.PressedButtons))
					return {};
				for (float &axis : slot.Axes)
					if (!reader.Float(axis)) return {};
				for (float &axis : slot.PreviousAxes)
					if (!reader.Float(axis)) return {};
				if (!reader.Bool(slot.Connected) || !reader.Bool(slot.PreviousConnected) ||
					!reader.Bool(slot.Mapped) || !reader.Get(slot.Reserved))
					return {};
			}
			event = value;
			break;
		}
		case 3: {
			DeferredEdit value;
			uint8_t edit = 0;
			if (!reader.Get(edit) || edit > uint8_t(engine::input::PresentationEditKind::CaretRight) ||
				!reader.Bool(value.Extend) ||
				!reader.Text(value.Text, engine::input::PresentationInput::MAXIMUM_TEXT_BYTES))
				return {};
			value.Kind = static_cast<engine::input::PresentationEditKind>(edit);
			event = value;
			break;
		}
		case 4: {
			DeferredComposition value;
			if (!reader.Get(value.Start) || !reader.Get(value.Length) || !reader.Get(value.Revision) ||
				!reader.Bool(value.Apply) ||
				!reader.Text(value.Text, engine::input::TextComposition::MAXIMUM_BYTES))
				return {};
			event = value;
			break;
		}
		case 5: {
			DeferredAdornment value;
			auto &pointer = value.Pointer;
			if (!reader.Float(pointer.Ray.Origin.X) || !reader.Float(pointer.Ray.Origin.Y) ||
				!reader.Float(pointer.Ray.Origin.Z) || !reader.Float(pointer.Ray.Direction.X) ||
				!reader.Float(pointer.Ray.Direction.Y) || !reader.Float(pointer.Ray.Direction.Z) ||
				!reader.Float(pointer.Position.X) || !reader.Float(pointer.Position.Y) ||
				!reader.Bool(pointer.PrimaryDown) || !reader.Bool(pointer.SecondaryDown) ||
				!reader.Bool(pointer.Moved) || !reader.Bool(pointer.Inside) || !reader.Float(value.Width) ||
				!reader.Float(value.Height))
				return {};
			event = value;
			break;
		}
		default:
			return {};
		}
		if (!reader.Bytes.empty()) return {};
		return event;
	}
}
