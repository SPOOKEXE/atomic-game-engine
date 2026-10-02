#pragma once
#include <engine/imagegraph/Document.hpp>
namespace engine::imagegraph {
	// Position already includes the source object's matrix. Outer world matrices are applied afterward.
	Vector3 SourceInstancePosition3D(const MeshInstance3D &instance, Vector3 position);
	// Source instance normals use only Euler rotation, without the position look/scale transform.
	Vector3 SourceInstanceNormal3D(const MeshInstance3D &instance, Vector3 normal);
}
