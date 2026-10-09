#include <engine/scene/ImageGraph.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Services.hpp>

#include <algorithm>
#include <chrono>
#include <imgui.h>
#include <studio/Assets.hpp>
#include <studio/Commands.hpp>
#include <studio/Editor.hpp>
#include <studio/ImageComposer.hpp>
#include <type_traits>

namespace studio {
	bool ApplyImageComposerSelection(
		engine::ecs::Store &store,
		std::span<const engine::ecs::Entity> instances,
		engine::core::Name property,
		engine::core::Name graph,
		engine::core::Name output,
		engine::core::Name key,
		bool authoritative,
		engine::ecs::Entity &created,
		std::vector<ImageComposerWrite> &writes,
		std::string &failure
	) {
		using namespace engine;
		if (!authoritative || instances.empty()) {
			failure = "select images in an editable scene";
			return false;
		}
		std::vector<ImageComposerWrite> candidate;
		for (const auto instance : instances) {
			if (!store.Alive(instance)) {
				failure = "selection no longer exists";
				return false;
			}
			const auto klass = store.ClassOf(instance);
			if (!klass.IsValid()) {
				failure = "selection has no image class";
				return false;
			}
			const auto &properties = ecs::Classes::Describe(klass).Properties;
			const auto descriptor =
				std::find_if(properties.begin(), properties.end(), [&](const auto &field) {
					return field.Name == property;
				});
			const auto *visual = store.Get<scene::Visual>(instance);
			if (descriptor == properties.end() || !descriptor->Writable ||
				descriptor->Type != ecs::PropertyType::Name ||
				ContentKindOfProperty(klass, descriptor->Spelling) != assets::AssetKind::Texture ||
				(visual != nullptr && visual->Locked)) {
				failure = "selection has no editable image slot";
				return false;
			}
			ImageComposerWrite write{.Instance = instance};
			if (!game::ReadProperty(store, instance, *descriptor, write.Before)) {
				failure = "could not read the selected image";
				return false;
			}
			candidate.push_back(std::move(write));
		}
		const auto controller = store.CreateInstance(scene::ImageGraphClass(), "ImageGraph");
		if (controller == ecs::NULL_ENTITY) {
			failure = "could not create an ImageGraph instance";
			return false;
		}
		if (!scene::SetImageGraphInstanceKey(store, controller, key) ||
			!scene::SetImageGraphAsset(store, controller, graph) ||
			!scene::SetImageGraphOutput(store, controller, output)) {
			store.DestroyInstance(controller);
			failure = "invalid ImageGraph reference";
			return false;
		}
		store.SetParent(controller, scene::WorkspaceOf(store));
		const auto reference = scene::ImageGraphContentName(store, controller);
		if (!reference.IsValid()) {
			store.DestroyInstance(controller);
			failure = "invalid ImageGraph identity";
			return false;
		}
		size_t completed = 0;
		for (auto &write : candidate) {
			write.After = ChosenContentValue(ecs::PropertyType::Name, reference.Text());
			const auto &properties = ecs::Classes::Describe(store.ClassOf(write.Instance)).Properties;
			const auto descriptor =
				std::find_if(properties.begin(), properties.end(), [&](const auto &field) {
					return field.Name == property;
				});
			if (!game::WriteAuthoredProperty(store, write.Instance, *descriptor, write.After)) {
				for (size_t prior = 0; prior < completed; ++prior) {
					const auto &priorProperties =
						ecs::Classes::Describe(store.ClassOf(candidate[prior].Instance)).Properties;
					const auto priorDescriptor =
						std::find_if(priorProperties.begin(), priorProperties.end(), [&](const auto &field) {
							return field.Name == property;
						});
					(void)game::WriteAuthoredProperty(
						store, candidate[prior].Instance, *priorDescriptor, candidate[prior].Before
					);
				}
				store.DestroyInstance(controller);
				failure = "could not assign the live image";
				return false;
			}
			++completed;
		}
		created = controller;
		writes = std::move(candidate);
		failure.clear();
		return true;
	}

	bool EditImageGraphInstanceInput(
		engine::ecs::Store &store,
		engine::ecs::Entity instance,
		const engine::imagegraph::InputOverride &input,
		bool reset,
		bool authoritative,
		const engine::imagegraph::Document *document,
		engine::game::PropertyValue &before,
		engine::game::PropertyValue &after,
		std::string &failure
	) {
		using namespace engine;
		if (!authoritative || store.Get<scene::ImageGraph>(instance) == nullptr) {
			failure = "select an editable ImageGraph instance";
			return false;
		}
		if (document != nullptr && !reset) {
			imagegraph::Document resolved;
			imagegraph::Diagnostic diagnostic;
			if (!imagegraph::ResolveInputs(*document, std::span(&input, 1), resolved, diagnostic)) {
				failure = diagnostic.Message;
				return false;
			}
		}
		const auto &properties = ecs::Classes::Describe(store.ClassOf(instance)).Properties;
		const auto descriptor = std::find_if(properties.begin(), properties.end(), [](const auto &field) {
			return field.Name == core::Name("Inputs");
		});
		game::PropertyValue prior, accepted;
		if (descriptor == properties.end() || !game::ReadProperty(store, instance, *descriptor, prior)) {
			failure = "could not read ImageGraph inputs";
			return false;
		}
		scene::ImageGraphInput value;
		value.Name = core::Name(input.Name);
		value.Kind = static_cast<scene::ImageGraphInputKind>(input.Value.index());
		std::visit(
			[&](const auto &item) {
				using T = std::decay_t<decltype(item)>;
				if constexpr (std::is_same_v<T, double>)
					value.Number = item;
				else if constexpr (std::is_same_v<T, bool>)
					value.Boolean = item;
				else if constexpr (std::is_same_v<T, std::string>)
					value.String = item;
				else
					value.Colour = item;
			},
			input.Value
		);
		const bool applied = reset ? scene::ResetImageGraphInput(store, instance, value.Name)
								   : scene::SetImageGraphInput(store, instance, value);
		if (!applied || !game::ReadProperty(store, instance, *descriptor, accepted)) {
			failure = "ImageGraph input was refused";
			return false;
		}
		before = std::move(prior);
		after = std::move(accepted);
		failure.clear();
		return true;
	}

	void Editor::DrawImageGraphInstanceInputs(engine::ecs::Store &store, Entity instance) {
		const auto *component = store.Get<engine::scene::ImageGraph>(instance);
		if (component == nullptr || !ImGui::CollapsingHeader("Named inputs", ImGuiTreeNodeFlags_DefaultOpen))
			return;
		const auto *document = FindLiveImageGraph(component->Graph);
		std::vector<engine::imagegraph::Parameter> controls;
		if (document != nullptr) controls = document->Parameters;
		auto inputValue = [](const engine::scene::ImageGraphInput &input) -> engine::imagegraph::InputValue {
			switch (input.Kind) {
			case engine::scene::ImageGraphInputKind::Number:
				return input.Number;
			case engine::scene::ImageGraphInputKind::Boolean:
				return input.Boolean;
			case engine::scene::ImageGraphInputKind::Colour:
				return input.Colour;
			case engine::scene::ImageGraphInputKind::String:
				return input.String;
			}
			return 0.;
		};
		if (document == nullptr)
			for (const auto &input : component->Inputs)
				controls.push_back({std::string(input.Name.Text()), inputValue(input)});
		if (controls.empty()) {
			ImGui::TextUnformatted(document == nullptr ? "Graph content is loading" : "No named inputs");
			return;
		}
		ImGui::BeginDisabled(AuthorityOf(SelectionWorld) != EditAuthority::Authoritative);
		for (const auto &control : controls) {
			ImGui::PushID(control.Name.c_str());
			auto value = control.Default;
			engine::scene::ImageGraphInput override;
			const bool explicitValue = engine::scene::GetImageGraphInput(
				store, instance, engine::core::Name(control.Name), override
			);
			if (explicitValue) value = inputValue(override);
			ImGui::SetNextItemWidth(std::max(80.f, ImGui::GetContentRegionAvail().x - 70.f));
			if (DrawImageComposerInput(control.Name.c_str(), value))
				ImageGraphInputEdits.push_back({SelectionWorld, instance, {control.Name, value}, false});
			if (explicitValue) {
				ImGui::SameLine();
				if (ImGui::SmallButton("Reset"))
					ImageGraphInputEdits.push_back({SelectionWorld, instance, {control.Name, value}, true});
			}
			ImGui::PopID();
		}
		ImGui::EndDisabled();
	}

	void Editor::ApplyImageComposerPending() {
		const auto inputEdits = std::move(ImageGraphInputEdits);
		ImageGraphInputEdits.clear();
		for (const auto &edit : inputEdits) {
			if (Universe == nullptr || AuthorityOf(edit.World) != EditAuthority::Authoritative) continue;
			bool changed = false;
			Universe->Enter(edit.World, [&](engine::ecs::Store &store) {
				const auto *component = store.Get<engine::scene::ImageGraph>(edit.Instance);
				if (component == nullptr) return;
				engine::game::PropertyValue before, after;
				std::string failure;
				if (!EditImageGraphInstanceInput(
						store,
						edit.Instance,
						edit.Input,
						edit.Reset,
						true,
						FindLiveImageGraph(component->Graph),
						before,
						after,
						failure
					)) {
					ENGINE_WARN("ImageGraph input: {}", failure);
					return;
				}
				changed = !engine::game::ValuesEqual(before, after);
				if (changed && Commands != nullptr)
					Commands->RecordProperty(
						edit.World,
						edit.Instance,
						engine::core::Name("Inputs"),
						before,
						after,
						"Set ImageGraph input"
					);
			});
			if (changed) MarkModified();
		}
		if (!ComposerApply.Graph.IsValid()) return;
		const auto request = std::move(ComposerApply);
		ComposerApply = {};
		if (Universe == nullptr || AuthorityOf(request.World) != EditAuthority::Authoritative) return;
		std::optional<std::string> recording;
		if (Commands != nullptr) {
			recording = Commands->TryBeginRecording("composer.apply", "Apply live image");
			if (!recording) return;
		}
		bool accepted = false;
		std::string failure;
		Universe->Enter(request.World, [&](engine::ecs::Store &store) {
			const auto key = engine::core::Name(
				"composer-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
				"-" + std::to_string(++ComposerInstanceSerial)
			);
			engine::ecs::Entity created = engine::ecs::NULL_ENTITY;
			std::vector<ImageComposerWrite> writes;
			accepted = ApplyImageComposerSelection(
				store,
				request.Instances,
				request.Property,
				request.Graph,
				request.Output,
				key,
				true,
				created,
				writes,
				failure
			);
			if (!accepted || Commands == nullptr) return;
			Commands->RecordCreate(store, request.World, created, "Create ImageGraph");
			for (const auto &write : writes)
				Commands->RecordProperty(
					request.World,
					write.Instance,
					request.Property,
					write.Before,
					write.After,
					"Apply live image"
				);
		});
		if (recording)
			Commands->FinishRecording(
				*recording, accepted ? FinishOperation::Commit : FinishOperation::Cancel
			);
		if (ImageComposer != nullptr) {
			ImageComposer->Error = failure;
			if (accepted) ImageComposer->Notice = "Applied live image";
		}
		if (accepted) {
			MarkModified();
			GalleryScanned = false;
		}
	}
}
