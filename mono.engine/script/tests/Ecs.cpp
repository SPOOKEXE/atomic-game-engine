// The storage surface, exercised through both VMs.
//
// `engine.ecs.schema` covers what a described component *is* - the layout, the
// hooks, the serialisation - because that is pure C++ and testable without a
// VM. This file covers what a script can reach, which is the half a binding can
// get wrong while every piece underneath it is correct.
//
// **Both languages, and mostly the same assertions.** The two surfaces are two
// object models over one store, and the thing worth pinning is that they agree:
// a component one declares is the same component the other queries.

#include <engine/ecs/Schema.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Services.hpp>
#include <engine/script/Runtime.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

TEST_SUITE_ID("engine.script.ecs")
TEST_DEPENDS("engine.ecs.schema")

using engine::ecs::ComponentId;
using engine::ecs::Components;
using engine::ecs::Entity;
using engine::ecs::Schemas;
using engine::ecs::Store;
using engine::script::Language;
using engine::script::MakeRuntime;
using engine::script::Runtime;

namespace {

	void MustRun(Runtime &runtime, const std::string &source) {
		INFO(source);
		const bool ok = runtime.Run(source.c_str());
		INFO(runtime.LastError());
		REQUIRE(ok);
	}

	void MustFail(Runtime &runtime, const std::string &source) {
		INFO(source);
		REQUIRE_FALSE(runtime.Run(source.c_str()));
	}

	// The component table is process-wide and nothing unregisters, so a case
	// that declares `Health` twice in one binary is declaring it once and then
	// agreeing with itself - which is a different test from the one intended.
	std::string Unique(const char *what) {
		static int counter = 0;
		return std::string("engine.script.ecs.test.") + what + "." + std::to_string(counter++);
	}
}

// --- declaring ---------------------------------------------------------------

TEST_CASE("a script declares a component and the storage has it", "[script-ecs]") {
	engine::scene::EnsureClassTree();
	Store store("script_ecs");
	const auto runtime = MakeRuntime(store, Language::Luau);

	const std::string name = Unique("health");

	MustRun(
		*runtime,
		"local created = World:DefineComponent('" + name +
			"', { Current = 'number', Max = 'number' })\n"
			"assert(created, 'the first declaration should have created it')\n"
			"assert(World:HasComponentType('" +
			name + "'), 'the component is not registered')\n"
	);

	// The id came from the same counter every C++ component uses, which is the
	// property that lets a system iterate what a script declared.
	const ComponentId id = Components::Find(engine::core::Name(name));
	REQUIRE(id.IsValid());
	REQUIRE(Schemas::Of(id) != nullptr);
	REQUIRE(Schemas::Of(id)->Fields().size() == 2);
}

TEST_CASE("declaring the same component twice agrees rather than fails", "[script-ecs]") {
	engine::scene::EnsureClassTree();
	Store store("script_ecs");
	const auto runtime = MakeRuntime(store, Language::Luau);

	const std::string name = Unique("agree");

	MustRun(
		*runtime,
		"assert(World:DefineComponent('" + name +
			"', { A = 'number' }), 'the first should create')\n"
			"assert(not World:DefineComponent('" +
			name + "', { A = 'number' }), 'the second should agree')\n"
	);

	MustFail(*runtime, "World:DefineComponent('" + name + "', { A = 'number', B = 'number' })");
	MustFail(*runtime, "World:DefineComponent('" + name + "', { A = 'Vector3' })");
}

TEST_CASE("a field type nothing maps is refused where it was written", "[script-ecs]") {
	engine::scene::EnsureClassTree();
	Store store("script_ecs");
	const auto runtime = MakeRuntime(store, Language::Luau);

	MustFail(*runtime, "World:DefineComponent('" + Unique("bad") + "', { A = 'sparkly' })");
	MustFail(*runtime, "World:DefineComponent('" + Unique("bad") + "', { A = 5 })");
	MustFail(*runtime, "World:DefineComponent('" + Unique("bad") + "', { [1] = 'number' })");
	MustFail(*runtime, "World:DefineComponent('" + Unique("bad") + "', { A = 'Enum.' })");
}

TEST_CASE("a component the engine declares is not writable through this surface", "[script-ecs]") {
	engine::scene::EnsureClassTree();
	Store store("script_ecs");
	const auto runtime = MakeRuntime(store, Language::Luau);

	// `scene.Visual` is a C++ struct with no field list at run time, and it
	// already has a property surface. Refusing here is what keeps one way to
	// write it rather than two.
	MustFail(*runtime, "Instance.new('Part'):GetComponent('scene.Visual')");
	MustFail(*runtime, "Instance.new('Part'):SetComponent('scene.Visual', {})");

	// Asking whether it is carried is a different question, and it has a
	// correct answer either way.
	MustRun(
		*runtime,
		"local part = Instance.new('Part')\n"
		"assert(part:HasComponent('scene.Visual'), 'a Part carries a Visual')\n"
		"assert(not part:HasComponent('nothing.declared.this'), 'an unknown component is not carried')\n"
	);
}

// --- entities and values -----------------------------------------------------

TEST_CASE("an entity takes a component, reads it back and gives it up", "[script-ecs]") {
	engine::scene::EnsureClassTree();
	Store store("script_ecs");
	const auto runtime = MakeRuntime(store, Language::Luau);

	const std::string name = Unique("health");

	MustRun(
		*runtime,
		"World:DefineComponent('" + name +
			"', { Current = 'number', Max = 'number' })\n"
			"local entity = World:CreateEntity()\n"
			"assert(typeof(entity) == 'Instance', 'an entity is an Instance handle')\n"
			"assert(not entity:HasComponent('" +
			name +
			"'), 'a fresh entity carries nothing')\n"
			"assert(entity:GetComponent('" +
			name +
			"') == nil, 'a component nobody attached reads as nil')\n"
			"entity:SetComponent('" +
			name +
			"', { Current = 30, Max = 100 })\n"
			"assert(entity:HasComponent('" +
			name +
			"'), 'the component did not attach')\n"
			"local health = entity:GetComponent('" +
			name +
			"')\n"
			"assert(health.Current == 30 and health.Max == 100, 'the values did not survive')\n"
			"entity:RemoveComponent('" +
			name +
			"')\n"
			"assert(not entity:HasComponent('" +
			name + "'), 'the component did not come off')\n"
	);
}

TEST_CASE("a field a write left out keeps the value it had", "[script-ecs]") {
	engine::scene::EnsureClassTree();
	Store store("script_ecs");
	const auto runtime = MakeRuntime(store, Language::Luau);

	const std::string name = Unique("partial");

	MustRun(
		*runtime,
		"World:DefineComponent('" + name +
			"', { Current = 'number', Max = 'number' })\n"
			"local entity = World:CreateEntity()\n"
			"entity:SetComponent('" +
			name +
			"', { Current = 30, Max = 100 })\n"
			"entity:SetComponent('" +
			name +
			"', { Current = 12 })\n"
			"local health = entity:GetComponent('" +
			name +
			"')\n"
			"assert(health.Current == 12, 'the write did not land, got ' .. tostring(health.Current))\n"
			"assert(health.Max == 100, 'an unmentioned field was reset to ' .. tostring(health.Max))\n"
	);
}

TEST_CASE("a field the component does not have is refused", "[script-ecs]") {
	engine::scene::EnsureClassTree();
	Store store("script_ecs");
	const auto runtime = MakeRuntime(store, Language::Luau);

	const std::string name = Unique("strict");
	MustRun(*runtime, "World:DefineComponent('" + name + "', { A = 'number' })");
	MustFail(*runtime, "World:CreateEntity():SetComponent('" + name + "', { B = 1 })");
}

TEST_CASE("every value type crosses and comes back", "[script-ecs]") {
	engine::scene::EnsureClassTree();
	Store store("script_ecs");
	const auto runtime = MakeRuntime(store, Language::Luau);

	const std::string name = Unique("values");

	MustRun(
		*runtime,
		"World:DefineComponent('" + name +
			"', {\n"
			"  Flag = 'boolean',\n"
			"  Count = 'int32',\n"
			"  Ratio = 'float',\n"
			"  Exact = 'number',\n"
			"  Label = 'string',\n"
			"  Kind = 'Name',\n"
			"  Where = 'Vector3',\n"
			"  Tint = 'Color3',\n"
			"  Placement = 'CFrame',\n"
			"  Target = 'Instance',\n"
			"})\n"
			"local part = Instance.new('Part')\n"
			"local entity = World:CreateEntity()\n"
			"entity:SetComponent('" +
			name +
			"', {\n"
			"  Flag = true,\n"
			"  Count = 7,\n"
			"  Ratio = 0.5,\n"
			"  Exact = 1.25,\n"
			"  Label = 'a score that changes',\n"
			"  Kind = 'Counter',\n"
			"  Where = Vector3.new(1, 2, 3),\n"
			"  Tint = Color3.new(1, 0, 0),\n"
			"  Placement = CFrame.new(4, 5, 6),\n"
			"  Target = part,\n"
			"})\n"
			"local held = entity:GetComponent('" +
			name +
			"')\n"
			"assert(held.Flag == true, 'boolean')\n"
			"assert(held.Count == 7, 'int32')\n"
			"assert(math.abs(held.Ratio - 0.5) < 1e-6, 'float')\n"
			"assert(held.Exact == 1.25, 'double')\n"
			"assert(held.Label == 'a score that changes', 'string, got ' .. tostring(held.Label))\n"
			"assert(held.Kind == 'Counter', 'Name')\n"
			"assert(held.Where == Vector3.new(1, 2, 3), 'Vector3')\n"
			"assert(held.Tint.R == 1, 'Color3')\n"
			"assert(held.Placement.Position == Vector3.new(4, 5, 6), 'CFrame')\n"
			"assert(held.Target == part, 'Instance reference')\n"
	);
}

TEST_CASE("a string field can be rewritten without leaking the last one", "[script-ecs]") {
	engine::scene::EnsureClassTree();
	Store store("script_ecs");
	const auto runtime = MakeRuntime(store, Language::Luau);

	const std::string name = Unique("text");

	// A `String` field is what makes a described component non-trivial. The
	// storage destroys and re-copies rather than memcpying, and this is the loop
	// that would surface a missing hook as a crash or a corrupted read.
	MustRun(
		*runtime,
		"World:DefineComponent('" + name +
			"', { Label = 'string' })\n"
			"local entity = World:CreateEntity()\n"
			"for step = 1, 200 do\n"
			"  entity:SetComponent('" +
			name +
			"', { Label = string.rep('x', step) })\n"
			"end\n"
			"local held = entity:GetComponent('" +
			name +
			"')\n"
			"assert(#held.Label == 200, 'the last write did not land, got ' .. tostring(#held.Label))\n"
	);
}

// --- querying ----------------------------------------------------------------

TEST_CASE("a query names components and hands back the entities carrying them", "[script-ecs]") {
	engine::scene::EnsureClassTree();
	Store store("script_ecs");
	const auto runtime = MakeRuntime(store, Language::Luau);

	const std::string health = Unique("health");
	const std::string poison = Unique("poison");

	MustRun(
		*runtime,
		"World:DefineComponent('" + health +
			"', { Current = 'number' })\n"
			"World:DefineComponent('" +
			poison +
			"', { Stacks = 'int32' })\n"
			"local both = World:CreateEntity()\n"
			"local one = World:CreateEntity()\n"
			"both:SetComponent('" +
			health +
			"', { Current = 10 })\n"
			"both:SetComponent('" +
			poison +
			"', { Stacks = 2 })\n"
			"one:SetComponent('" +
			health +
			"', { Current = 20 })\n"
			"assert(World:Count('" +
			health +
			"') == 2, 'one term')\n"
			"assert(World:Count('" +
			health + "', '" + poison +
			"') == 1, 'two terms')\n"
			"local found = World:Query('" +
			health + "', '" + poison +
			"')\n"
			"assert(#found == 1, 'the query returned ' .. #found)\n"
			"assert(found[1] == both, 'the wrong entity matched')\n"
			"assert(found[1]:GetComponent('" +
			poison + "').Stacks == 2, 'the match cannot be read')\n"
	);
}

TEST_CASE("a query naming nothing, or naming a typo, is refused", "[script-ecs]") {
	engine::scene::EnsureClassTree();
	Store store("script_ecs");
	const auto runtime = MakeRuntime(store, Language::Luau);

	// A typo would otherwise be a loop that never runs, which reads exactly
	// like a world with nothing in it.
	MustFail(*runtime, "World:Query()");
	MustFail(*runtime, "World:Query('nothing.declared.this')");
	MustFail(*runtime, "World:Count('nothing.declared.this')");
}

TEST_CASE("a script's component is iterated from C++ like any other", "[script-ecs]") {
	engine::scene::EnsureClassTree();
	Store store("script_ecs");
	const auto runtime = MakeRuntime(store, Language::Luau);

	const std::string name = Unique("shared");

	MustRun(
		*runtime,
		"World:DefineComponent('" + name +
			"', { Current = 'number' })\n"
			"for index = 1, 3 do\n"
			"  World:CreateEntity():SetComponent('" +
			name +
			"', { Current = index })\n"
			"end\n"
	);

	// **This is the whole point of the surface.** A system that never heard of
	// a script walks the rows a script created, through the same storage.
	const ComponentId id = Components::Find(engine::core::Name(name));
	REQUIRE(id.IsValid());

	const ComponentId terms[] = {id};
	double total = 0.0;
	const auto *schema = Schemas::Of(id);
	const uint32_t offset = schema->Find("Current")->Offset;

	store.EachMatching(terms, [&](Entity entity) {
		const auto *bytes = static_cast<const std::byte *>(store.GetComponent(entity, id));
		total += *reinterpret_cast<const double *>(bytes + offset);
	});

	REQUIRE(store.CountMatching(terms) == 3);
	REQUIRE(total == 6.0);
}

TEST_CASE("an instance lists every component it carries", "[script-ecs]") {
	engine::scene::EnsureClassTree();
	Store store("script_ecs");
	const auto runtime = MakeRuntime(store, Language::Luau);

	const std::string name = Unique("listed");

	MustRun(
		*runtime,
		"World:DefineComponent('" + name +
			"', { A = 'number' })\n"
			"local part = Instance.new('Part')\n"
			"part:SetComponent('" +
			name +
			"', { A = 1 })\n"
			"local held = part:GetComponents()\n"
			"local found = false\n"
			"for _, component in held do\n"
			"  if component == '" +
			name +
			"' then found = true end\n"
			"end\n"
			"assert(found, 'the script component is not listed')\n"
			"assert(#held > 1, 'a Part carries the engine components too')\n"
			"local sorted = true\n"
			"for index = 2, #held do\n"
			"  if held[index] < held[index - 1] then sorted = false end\n"
			"end\n"
			"assert(sorted, 'the list is not sorted')\n"
	);
}

TEST_CASE("a schema reads back in a shape the declaration accepts", "[script-ecs]") {
	engine::scene::EnsureClassTree();
	Store store("script_ecs");
	const auto runtime = MakeRuntime(store, Language::Luau);

	const std::string first = Unique("roundtrip");
	const std::string second = Unique("roundtrip");

	// The round trip is the point: what a schema says about itself has to be
	// something a declaration can be built from, or tooling has to keep its own
	// translation table.
	MustRun(
		*runtime,
		"World:DefineComponent('" + first +
			"', { Label = 'string', Where = 'Vector3', Count = 'int32' })\n"
			"local schema = World:GetComponentSchema('" +
			first +
			"')\n"
			"assert(schema ~= nil, 'a declared component has a schema')\n"
			"assert(World:DefineComponent('" +
			second +
			"', schema), 'the schema did not describe a component')\n"
			"assert(World:GetComponentSchema('nothing.declared.this') == nil, 'an unknown one is nil')\n"
	);

	REQUIRE(Schemas::Find(engine::core::Name(second)) != nullptr);
	REQUIRE(Schemas::Find(engine::core::Name(second))->Fields().size() == 3);
}

// --- the other language ------------------------------------------------------

TEST_CASE("the JavaScript surface reaches the same storage", "[script-ecs]") {
	engine::scene::EnsureClassTree();
	Store store("script_ecs");

	const std::string name = Unique("shared.js");

	{
		const auto luau = MakeRuntime(store, Language::Luau);
		MustRun(
			*luau,
			"World:DefineComponent('" + name +
				"', { Current = 'number' })\n"
				"World:CreateEntity():SetComponent('" +
				name + "', { Current = 5 })\n"
		);
	}

	// A second VM over the same world. The component is already declared, so
	// this one agrees with it rather than creating a second - which is the
	// property that makes two languages in one game possible at all.
	const auto js = MakeRuntime(store, Language::JavaScript);
	MustRun(
		*js,
		"if (World.DefineComponent('" + name +
			"', { Current: 'number' })) { throw new Error('should have agreed'); }\n"
			"const entity = World.CreateEntity();\n"
			"entity.SetComponent('" +
			name +
			"', { Current: 9 });\n"
			"if (entity.GetComponent('" +
			name +
			"').Current !== 9) { throw new Error('the value did not survive'); }\n"
			"if (World.Count('" +
			name + "') !== 2) { throw new Error('the query missed the other VM\\'s row'); }\n"
	);
}
