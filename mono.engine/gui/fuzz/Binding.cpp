#include <engine/core/Name.hpp>
#include <engine/ecs/Attributes.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/gui/Binding.hpp>
#include <engine/gui/Registration.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace {
	void Require(bool condition) {
		if (!condition) std::abort();
	}
}

extern "C" int LLVMFuzzerInitialize(int *count, char ***arguments) {
	engine::gui::RegisterGuiClasses();
	if (*count != 3 || std::string_view((*arguments)[1]) != "--write-seeds") return 0;
	const std::filesystem::path directory((*arguments)[2]);
	std::filesystem::create_directories(directory);
	std::ofstream file(directory / "source-path.binding", std::ios::binary);
	file.write("\0Source", 7);
	Require(file.good());
	file.close();
	std::exit(0);
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *input, size_t size) {
	using namespace engine;
	using namespace gui;
	RegisterGuiClasses();
	ecs::Store store("gui.fuzz.binding");
	const ecs::Entity source = store.CreateInstance(ecs::Classes::Find(core::Name("Instance")), "Source");
	const ecs::Entity label = store.CreateInstance(GuiClass("TextLabel"), "Label");
	const ecs::Entity bindingEntity = store.CreateInstance(GuiClass("UIBinding"), "Binding");
	Require(store.SetParent(bindingEntity, label));

	ecs::AttributeValue value;
	value.Type = ecs::PropertyType::String;
	value.String = "value";
	Require(ecs::SetAttribute(store, source, core::Name("Value"), value));
	Binding binding;
	if (size > 0) {
		binding.Target = input[0] == 0 ? core::Name("Text") : core::Name("Position");
		binding.SourcePath.assign(reinterpret_cast<const char *>(input + 1), size - 1);
	}
	binding.Attribute = core::Name("Value");
	binding.Fallback = "fallback";
	store.Set(bindingEntity, std::move(binding));
	EvaluateBindings(store);
	const BindingOutput *output = store.Get<BindingOutput>(bindingEntity);
	Require(output != nullptr && output->Value.size() <= 4096);
	EvaluateBindings(store);
	Require(BoundText(store, label, "authored").size() <= 4096);
	return 0;
}
