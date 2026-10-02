#pragma once
#include "RigidGraphFixture.hpp"
namespace engine::imagegraphphysics::testing {
	inline engine::imagegraph::Document MixedRigidSimulationFixture() {
		using namespace engine::imagegraph;
		auto document = RigidGraphFixture();
		document.Nodes.push_back({"observe", "pc.rigid_variable", "rigid", {}, {}});
		document.Nodes.push_back({"gravity", "pc.array_get", "", {}, {{"index", int64_t{0}}}});
		document.Nodes.push_back(
			{"fluid",
			 "pc.flip_domain",
			 "",
			 {},
			 {{"dimension_unit", EnumValue{0}},
			  {"dimension", Vector2{16, 16}},
			  {"particle_size", int64_t{2}},
			  {"attribute_max_particles", 16.},
			  {"attribute_iteration", 2.},
			  {"attribute_iteration_particle", 0.},
			  {"attribute_skip_incompressible", true},
			  {"time_step", .1}}}
		);
		document.Nodes.push_back(
			{"fill",
			 "pc.flip_fill",
			 "",
			 {},
			 {{"spawn_area_unit", EnumValue{0}}, {"spawn_area", Area{8, 8, 4, 4}}, {"density", .5}}}
		);
		document.Nodes.push_back({"step", "pc.flip_update", "", {}, {}});
		document.Links.insert(
			document.Links.end(),
			{{"body", "object", "observe", "objects"},
			 {"observe", "velocity_magnitude", "gravity", "array"},
			 {"gravity", "value", "fluid", "gravity"},
			 {"fluid", "domain", "fill", "domain"},
			 {"fill", "domain", "step", "domain"}}
		);
		document.Outputs.push_back({"fluid_state", "step", "domain"});
		return document;
	}
}
