// The bound-action stack, which both VMs write and both pumps walk.
//
// **Nothing here names a VM**, which is the point: `Actions.hpp` carries the
// argument, and what is left is the ordering rules — replace by name, sort
// stably by priority, first claim wins.
//
// @tier L9 · shared

#include "Actions.hpp"

#include <algorithm>
#include <utility>

namespace engine::script {

	InputReport KeyReport(scene::KeyCode key, bool began) {
		InputReport report;
		report.Key = key;
		report.Source = scene::InputSource::Keyboard;
		report.State = core::Name(began ? "Begin" : "End");
		return report;
	}

	InputReport ButtonReport(const scene::InputState &input, scene::MouseButton button, bool began) {
		InputReport report;

		// `MouseButton` and `InputSource` share their first members by
		// construction — see `scene/Input.hpp` — which is what makes a button
		// ordinal an `Enum.UserInputType` member without a table in between.
		report.Source = static_cast<scene::InputSource>(button);
		report.State = core::Name(began ? "Begin" : "End");
		report.Position = core::Vector3{input.MousePosition.X, input.MousePosition.Y, 0.0f};
		return report;
	}

	InputReport MotionReport(const scene::InputState &input) {
		InputReport report;
		report.Source = scene::InputSource::MouseMovement;
		report.State = core::Name("Change");
		report.Position = core::Vector3{input.MousePosition.X, input.MousePosition.Y, 0.0f};
		report.Delta = core::Vector3{input.MouseDelta.X, input.MouseDelta.Y, 0.0f};
		return report;
	}

	InputReport WheelReport(const scene::InputState &input) {
		InputReport report;
		report.Source = scene::InputSource::MouseWheel;
		report.State = core::Name("Change");

		// **The notch count goes in `Z` of both**, which is Roblox's odd-looking
		// placement and the one a migrated script reads: a wheel has nowhere else
		// to go in a `Vector3`.
		report.Position = core::Vector3{input.MousePosition.X, input.MousePosition.Y, input.WheelDelta};
		report.Delta = core::Vector3{0.0f, 0.0f, input.WheelDelta};
		return report;
	}

	bool ActionStack::Bind(BoundAction action, CallbackRef &released) {
		const auto existing = std::find_if(Bound.begin(), Bound.end(), [&action](const BoundAction &bound) {
			return bound.Name == action.Name;
		});

		bool replaced = false;
		if (existing != Bound.end()) {
			released = existing->Callback;
			replaced = true;
			*existing = std::move(action);
		} else {
			Bound.push_back(std::move(action));
		}

		// Stable, so two actions at one priority stay in bind order — see the
		// declaration for why that is the only reproducible tie-break.
		std::stable_sort(Bound.begin(), Bound.end(), [](const BoundAction &left, const BoundAction &right) {
			return left.Priority > right.Priority;
		});
		return replaced;
	}

	bool ActionStack::Unbind(std::string_view name, CallbackRef &released) {
		const auto found = std::find_if(Bound.begin(), Bound.end(), [name](const BoundAction &bound) {
			return bound.Name == name;
		});
		if (found == Bound.end()) {
			return false;
		}

		released = found->Callback;
		Bound.erase(found);
		return true;
	}

	void ActionStack::UnbindAll(std::vector<CallbackRef> &released) {
		for (const BoundAction &action : Bound) {
			released.push_back(action.Callback);
		}
		Bound.clear();
	}

	const BoundAction *ActionStack::Claiming(uint16_t key) const {
		for (const BoundAction &action : Bound) {
			if (std::find(action.Keys.begin(), action.Keys.end(), key) != action.Keys.end()) {
				return &action;
			}
		}
		return nullptr;
	}

	const BoundAction *ActionStack::Find(std::string_view name) const {
		for (const BoundAction &action : Bound) {
			if (action.Name == name) {
				return &action;
			}
		}
		return nullptr;
	}

	std::span<const BoundAction> ActionStack::Entries() const {
		return Bound;
	}
}
