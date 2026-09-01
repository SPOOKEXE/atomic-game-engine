#include <engine/gui/Registration.hpp>
#include <engine/gui/SettingsMenu.hpp>

#include <algorithm>

namespace engine::gui {

	bool SettingsMenuExtensions::Set(core::Name id, std::string_view label) {
		if (!id.IsValid() || label.empty() || label.size() > MAXIMUM_LABEL_BYTES) {
			return false;
		}

		const auto found = std::find_if(Entries.begin(), Entries.end(), [&](const SettingsMenuAction &entry) {
			return entry.Id == id;
		});
		if (found != Entries.end()) {
			found->Label = label;
			return true;
		}
		if (Entries.size() >= MAXIMUM_ACTIONS) {
			return false;
		}

		Entries.push_back(SettingsMenuAction{id, std::string(label)});
		return true;
	}

	bool SettingsMenuExtensions::Remove(core::Name id) {
		const auto found = std::find_if(Entries.begin(), Entries.end(), [&](const SettingsMenuAction &entry) {
			return entry.Id == id;
		});
		if (found == Entries.end()) {
			return false;
		}
		Entries.erase(found);
		return true;
	}

	void SettingsMenuExtensions::Clear() {
		Entries.clear();
	}

	SettingsMenuExtensions &ReachSettingsMenuExtensions(ecs::Store &store) {
		RegisterGuiComponents();
		SettingsMenuExtensions *extensions = store.ResourceMutable<SettingsMenuExtensions>();
		if (extensions == nullptr) {
			store.SetResource(SettingsMenuExtensions{});
			extensions = store.ResourceMutable<SettingsMenuExtensions>();
		}
		return *extensions;
	}

	std::span<const SettingsMenuAction> SettingsMenuActionsOf(const ecs::Store &store) {
		RegisterGuiComponents();
		const SettingsMenuExtensions *extensions = store.Resource<SettingsMenuExtensions>();
		return extensions == nullptr ? std::span<const SettingsMenuAction>{} : extensions->Actions();
	}
}
