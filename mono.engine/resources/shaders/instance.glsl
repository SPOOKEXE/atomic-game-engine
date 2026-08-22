// Decoding the packed per-instance row.
//
// **Included by every vertex stage that binds the instance stream**, so the
// arithmetic exists once rather than once per pass. `opaque.vert` and
// `shadow.vert` read the same thirty-six bytes and drew different pictures from
// them the day one of them was edited and the other was not.
//
// The C++ side is `render/src/InstancePacking.hpp`. The two decodes have to
// agree and no compiler can check that they do, so both are written to be read
// side by side: `render/tests/InstancePacking.cpp` pins the C++ half against the
// matrix the old layout uploaded, and this half is the same three steps in the
// same order.

// SDL assigns vertex storage buffers to set zero for SPIR-V. The first is the
// stable thirty-six-byte row pool; the second is this draw order's uint slots.
layout(set = 0, binding = 0) readonly buffer InstanceRows { uint words[]; } residentInstances;
layout(set = 0, binding = 1) readonly buffer InstanceIndices { uint slots[]; } drawInstances;

#ifndef GPU_INSTANCE_WORDS
#define GPU_INSTANCE_WORDS 9
#endif

uint InstanceWord(uint part) {
	uint resident = drawInstances.slots[gl_InstanceIndex];
	return residentInstances.words[resident * uint(GPU_INSTANCE_WORDS) + part];
}

vec3 InstancePosition() {
	return vec3(
		uintBitsToFloat(InstanceWord(0)),
		uintBitsToFloat(InstanceWord(1)),
		uintBitsToFloat(InstanceWord(2)));
}

vec3 InstanceScale() {
	return vec3(
		uintBitsToFloat(InstanceWord(5)),
		uintBitsToFloat(InstanceWord(6)),
		uintBitsToFloat(InstanceWord(7)));
}

// The rotation as a unit quaternion, `xyz` vector part and `w` scalar.
//
// **Renormalised, and that is most of the accuracy.** Four components rounded
// independently land off the unit sphere, and a quaternion off the unit sphere
// is a rotation *and* a scale - so skipping this would show up as geometry
// breathing rather than as a rotation being slightly wrong.
vec4 InstanceRotation() {
	vec4 raw = vec4(
		unpackSnorm2x16(InstanceWord(3)),
		unpackSnorm2x16(InstanceWord(4)));
	float square = dot(raw, raw);
	return square > 1e-12 ? raw * inversesqrt(square) : vec4(0.0, 0.0, 0.0, 1.0);
}

// Rotates a vector by a unit quaternion.
//
// The two-cross-product form: eighteen multiplies against the twenty-seven a
// matrix build plus a matrix multiply would cost, and it never materialises the
// 3x3 that the old layout was uploading.
vec3 RotateByQuaternion(vec4 quaternion, vec3 point) {
	vec3 twice = 2.0 * cross(quaternion.xyz, point);
	return point + quaternion.w * twice + cross(quaternion.xyz, twice);
}

// The world position of one mesh vertex under this instance.
vec3 InstanceWorldPosition(vec4 quaternion, vec3 meshPosition) {
	return RotateByQuaternion(quaternion, meshPosition * InstanceScale()) + InstancePosition();
}

// The world-space normal, corrected for the instance's non-uniform scale.
//
// **A normal transforms by the inverse transpose**, which for `R * S` with `R`
// orthonormal is `R * S^-1` up to a length nobody here cares about - so this is
// one divide and the same rotation the position took. The old layout uploaded
// `1 / scale^2` as a whole float4 to reach the same place through a matrix.
//
// A zero axis keeps the old rule rather than dividing: the degenerate scale
// multiplies instead, which drives that component to zero exactly as
// `InverseScaleSquared`'s guard used to.
vec3 InstanceWorldNormal(vec4 quaternion, vec3 meshNormal) {
	vec3 scale = InstanceScale();
	bvec3 usable = greaterThan(scale * scale, vec3(1e-12));
	vec3 divisor = mix(vec3(1.0), scale, usable);
	vec3 factor = mix(scale, 1.0 / divisor, usable);
	return RotateByQuaternion(quaternion, meshNormal * factor);
}

// The instance's colour and alpha.
vec4 InstanceColour() {
	return unpackUnorm4x8(InstanceWord(8));
}
