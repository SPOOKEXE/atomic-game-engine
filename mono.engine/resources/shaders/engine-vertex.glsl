// engine/vertex: what an authored `void Vertex(inout VertexInput v)` is written
// against. It runs in every pass that draws the surface, shadows included, after
// skinning and in world space, so wind, inflation and contact move the shadow
// with the body.

struct VertexInput {
	vec3 Position;      // world
	vec3 Normal;        // world, unit length
	vec2 UV;
	vec4 Attribute[4];  // the editable mesh channels, zero on a packed mesh
	vec3 Origin;        // the instance's world position
	float Time;         // seconds
};

// The material's values, under the name a surface's parameters read.
layout(set = 1, binding = VERTEX_VALUES_BINDING) uniform VertexValues {
	vec4 Parameter0;
	vec4 Parameter1;
	vec4 Parameter2;
	vec4 Parameter3;
	vec4 ParameterState;
	// x seconds.
	vec4 View;
} lighting;

void Vertex(inout VertexInput v);

// Runs the author's Vertex over one engine vertex.
void EngineAuthoredVertex(inout vec3 position, inout vec3 normal, inout vec2 uv, vec4 attributes[4], vec3 origin) {
	VertexInput v;
	v.Position = position;
	v.Normal = normal;
	v.UV = uv;
	v.Attribute = attributes;
	v.Origin = origin;
	v.Time = lighting.View.x;
	Vertex(v);
	position = v.Position;
	normal = normalize(v.Normal);
	uv = v.UV;
}
