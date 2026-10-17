#include <engine/ecs/Store.hpp>
#include <engine/gui/Binding.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/Localization.hpp>
#include <engine/gui/TextResolution.hpp>
#include <engine/gui/Typing.hpp>

namespace engine::gui {

	std::string ResolveText(
		const ecs::Store &store,
		ecs::Entity instance,
		const Label &label,
		const TextResolutionRequest &request
	) {
		std::string text = label.Text;
		const LabelPresentation *presentation = store.Get<LabelPresentation>(instance);
		if (presentation != nullptr && presentation->LocalizationKey.IsValid() &&
			request.Catalogue != nullptr) {
			LocalizedMessage message;
			message.Key = presentation->LocalizationKey;
			message.Source = label.Text;
			if (const auto *arguments = store.Get<LabelLocalizationArguments>(instance);
				arguments != nullptr) {
				for (size_t index = 0; index < arguments->Count && index < arguments->Values.size();
					 index++) {
					const LabelLocalizationArgument &argument = arguments->Values[index];
					if (!argument.Name.IsValid()) continue;
					LocalizedArgument resolved;
					resolved.Name = argument.Name;
					resolved.Type = argument.Type;
					resolved.Value = argument.String;
					resolved.Number = argument.Number;
					resolved.UnixSeconds = argument.UnixSeconds;
					if (!message.AddArgument(std::move(resolved))) break;
				}
			}
			text =
				request.Catalogue->Resolve(message, request.Locale, request.StudioMissingLocalizationMarker);
		}
		return TextWithComposition(store, instance, BoundText(store, instance, text));
	}
}
