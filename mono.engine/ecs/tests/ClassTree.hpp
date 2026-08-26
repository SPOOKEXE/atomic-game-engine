#pragma once

// The one class tree both object-model suites are written against.
//
// **Shared because a class tree cannot be built twice.** `Classes` and
// `Components` are process-wide and never unregister, so two files each
// declaring their own `test.Transform` would be two C++ types asking for one
// component name, and `Components::Adopt` aborts on that. One header, one
// registration, and `engine.ecs.classes` and `engine.ecs.instance` both build
// against it.
//
// @tier L3 · shared

#include <engine/core/Name.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/ecs/Property.hpp>

namespace ecs_test {

	struct Transform {
		float X = 0.0f;
		float Y = 0.0f;
	};
	struct Bounds {
		float HalfExtent = 0.5f;
	};
	struct Visual {
		engine::core::Name Mesh;
		bool Visible = true;
	};
	struct Motion {
		float Speed = 0.0f;
	};

	// A handle to another instance, for the one thing `Clone` has to reason
	// about that no other component does. A weld naming the two parts it joins
	// is this shape.
	struct Link {
		engine::ecs::Entity Target;
	};

	// The five classes, as a tree: Instance, PVInstance, BasePart, Part, Model.
	struct Tree {
		engine::ecs::ClassId Instance;
		engine::ecs::ClassId PVInstance;
		engine::ecs::ClassId BasePart;
		engine::ecs::ClassId Part;
		engine::ecs::ClassId Model;
	};

	// Registers the tree once for the whole process and hands it back.
	//
	// @return The registered class ids.
	inline const Tree &ClassTree() {
		using engine::core::Name;
		using engine::ecs::Classes;
		using engine::ecs::ComponentId;
		using engine::ecs::Components;

		static const Tree tree = [] {
			Tree built;
			built.Instance = Classes::Register("test.Instance", {});

			const ComponentId transform = Components::Register<Transform>("test.Transform");
			const ComponentId bounds = Components::Register<Bounds>("test.Bounds");
			const ComponentId visual = Components::Register<Visual>("test.Visual");
			const ComponentId motion = Components::Register<Motion>("test.Motion");
			const ComponentId link = Components::Register<Link>("test.Link");

			const ComponentId pv[] = {transform};
			built.PVInstance = Classes::Register("test.PVInstance", built.Instance, pv);

			const ComponentId base[] = {bounds};
			built.BasePart = Classes::Register("test.BasePart", built.PVInstance, base);

			const ComponentId part[] = {visual, link};
			built.Part = Classes::Register("test.Part", built.BasePart, part);

			const ComponentId model[] = {motion};
			built.Model = Classes::Register("test.Model", built.PVInstance, model);

			// The prototype rows. A default declared on a base applies to
			// everything registered under it afterwards.
			Classes::Default<Bounds>(built.BasePart, Bounds{2.5f});
			Classes::Default<Visual>(built.Part, Visual{Name("test.mesh.cube"), true});

			Classes::Property<&Transform::X>(built.PVInstance, "X");
			Classes::Property<&Transform::Y>(built.PVInstance, "Y");
			Classes::Property<&Bounds::HalfExtent>(built.BasePart, "HalfExtent");
			Classes::Property<&Visual::Visible>(built.Part, "Visible");
			Classes::Property<&Visual::Mesh>(built.Part, "Mesh");
			Classes::Property<&Link::Target>(built.Part, "Target");

			return built;
		}();
		return tree;
	}
}
