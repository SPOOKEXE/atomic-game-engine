#include <engine/core/Name.hpp>
#include <engine/ecs/Attributes.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/gui/Binding.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.gui.binding")

TEST_CASE("a binding publishes an attribute string without changing label text", "[gui][binding]") {
	using namespace engine;
	using namespace engine::gui;

	ecs::Store store("gui_binding.output");
	RegisterGuiClasses();
	const ecs::Entity source = store.CreateInstance(ecs::Classes::Find(core::Name("Instance")), "Source");
	const ecs::Entity label = store.CreateInstance(GuiClass("TextLabel"), "Label");
	const ecs::Entity binding = store.CreateInstance(GuiClass("UIBinding"), "Binding");
	store.SetParent(binding, label);

	ecs::AttributeValue value;
	value.Type = ecs::PropertyType::String;
	value.String = "twelve";
	REQUIRE(ecs::SetAttribute(store, source, core::Name("Score"), value));

	Binding configured;
	configured.SourcePath = "Source";
	configured.Attribute = core::Name("Score");
	configured.Fallback = "zero";
	store.Set(binding, configured);

	REQUIRE(EvaluateBindings(store) == 1);
	CHECK(BoundText(store, label, "authored") == "twelve");
	CHECK(BoundText(store, label, "authored") != "zero");
	CHECK(EvaluateBindings(store) == 0);
	const BindingOutput *first = store.Get<BindingOutput>(binding);
	REQUIRE(first != nullptr);
	CHECK(first->Valid);
	CHECK(first->Failure == BindingFailure::None);
	const uint64_t firstRevision = first->SourceRevision;
	const uint64_t firstEvaluations = first->EvaluationCount;
	REQUIRE(firstRevision != 0);

	CHECK(EvaluateBindings(store) == 0);
	const BindingOutput *unchanged = store.Get<BindingOutput>(binding);
	REQUIRE(unchanged != nullptr);
	CHECK(unchanged->SourceRevision == firstRevision);
	CHECK(unchanged->EvaluationCount == firstEvaluations);

	value.Type = ecs::PropertyType::Int32;
	value.Int32 = 12;
	REQUIRE(ecs::SetAttribute(store, source, core::Name("Score"), value));
	REQUIRE(EvaluateBindings(store) == 1);
	CHECK(BoundText(store, label, "authored") == "zero");
	const BindingOutput *mismatched = store.Get<BindingOutput>(binding);
	REQUIRE(mismatched != nullptr);
	CHECK_FALSE(mismatched->Valid);
	CHECK(mismatched->Failure == BindingFailure::TypeMismatch);
	CHECK(mismatched->SourceRevision > firstRevision);
	CHECK(mismatched->EvaluationCount == firstEvaluations + 1);
}

TEST_CASE("a binding publishes source and target failures without changing its target", "[gui][binding]") {
	using namespace engine;
	using namespace engine::gui;

	ecs::Store store("gui_binding.failures");
	RegisterGuiClasses();
	const ecs::Entity label = store.CreateInstance(GuiClass("TextLabel"), "Label");
	(void)store.CreateInstance(ecs::Classes::Find(core::Name("Instance")), "Source");
	const ecs::Entity binding = store.CreateInstance(GuiClass("UIBinding"), "Binding");
	store.SetParent(binding, label);

	Binding configured;
	configured.SourcePath = "Missing";
	configured.Attribute = core::Name("Value");
	configured.Fallback = "fallback";
	store.Set(binding, configured);
	REQUIRE(EvaluateBindings(store) == 1);
	const BindingOutput *missing = store.Get<BindingOutput>(binding);
	REQUIRE(missing != nullptr);
	CHECK(missing->Failure == BindingFailure::MissingSource);
	CHECK_FALSE(missing->Valid);
	CHECK(BoundText(store, label, "authored") == "fallback");

	configured.SourcePath = "";
	store.Set(binding, configured);
	REQUIRE(EvaluateBindings(store) == 1);
	const BindingOutput *invalid = store.Get<BindingOutput>(binding);
	REQUIRE(invalid != nullptr);
	CHECK(invalid->Failure == BindingFailure::InvalidSourcePath);

	configured.SourcePath = "Source";
	configured.Target = core::Name("Position");
	store.Set(binding, configured);
	REQUIRE(EvaluateBindings(store) == 1);
	const BindingOutput *unsupported = store.Get<BindingOutput>(binding);
	REQUIRE(unsupported != nullptr);
	CHECK(unsupported->Failure == BindingFailure::UnsupportedTarget);
}

TEST_CASE("bindings bound paths and values without publishing partial output", "[gui][binding]") {
	using namespace engine;
	using namespace engine::gui;

	ecs::Store store("gui_binding.boundaries");
	RegisterGuiClasses();
	const ecs::Entity source = store.CreateInstance(ecs::Classes::Find(core::Name("Instance")), "Source");
	const ecs::Entity label = store.CreateInstance(GuiClass("TextLabel"), "Label");
	const ecs::Entity binding = store.CreateInstance(GuiClass("UIBinding"), "Binding");
	store.SetParent(binding, label);

	ecs::AttributeValue value;
	value.Type = ecs::PropertyType::String;
	value.String.assign(4096, 'v');
	REQUIRE(ecs::SetAttribute(store, source, core::Name("Value"), value));
	Binding configured;
	configured.SourcePath = "Source";
	configured.Attribute = core::Name("Value");
	configured.Fallback = "fallback";
	store.Set(binding, configured);
	REQUIRE(EvaluateBindings(store) == 1);
	CHECK(BoundText(store, label, "authored") == value.String);

	value.String.push_back('x');
	REQUIRE(ecs::SetAttribute(store, source, core::Name("Value"), value));
	REQUIRE(EvaluateBindings(store) == 1);
	const BindingOutput *oversized = store.Get<BindingOutput>(binding);
	REQUIRE(oversized != nullptr);
	CHECK_FALSE(oversized->Valid);
	CHECK(oversized->Failure == BindingFailure::OutputTooLong);
	CHECK(oversized->Value == "fallback");

	ecs::Entity nested = store.CreateInstance(ecs::Classes::Find(core::Name("Instance")), "x");
	std::string nestedPath = "x";
	for (size_t index = 1; index < 32; ++index) {
		const ecs::Entity child = store.CreateInstance(ecs::Classes::Find(core::Name("Instance")), "x");
		store.SetParent(child, nested);
		nested = child;
		nestedPath += "/x";
	}
	value.String = "deep";
	REQUIRE(ecs::SetAttribute(store, nested, core::Name("Value"), value));
	configured.SourcePath = nestedPath;
	store.Set(binding, configured);
	REQUIRE(EvaluateBindings(store) == 1);
	CHECK(BoundText(store, label, "authored") == "deep");

	configured.SourcePath += "/x";
	store.Set(binding, configured);
	REQUIRE(EvaluateBindings(store) == 1);
	const BindingOutput *tooDeep = store.Get<BindingOutput>(binding);
	REQUIRE(tooDeep != nullptr);
	CHECK(tooDeep->Failure == BindingFailure::InvalidSourcePath);
	CHECK(tooDeep->Value == "fallback");

	configured.SourcePath.assign(257, 'a');
	store.Set(binding, configured);
	CHECK(EvaluateBindings(store) == 0);
	const BindingOutput *invalid = store.Get<BindingOutput>(binding);
	REQUIRE(invalid != nullptr);
	CHECK(invalid->Failure == BindingFailure::InvalidSourcePath);
	CHECK(invalid->Value == "fallback");
}
