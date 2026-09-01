// Decoding the packed per-instance row.
//
// **Included by every vertex stage that binds the instance stream**, so the
// arithmetic exists once rather than once per pass. `opaque.vert` and
// `shadow.vert` read the same forty-eight bytes and drew different pictures from
// them the day one of them was edited and the other was not.
//
// The C++ side is `render/src/InstancePacking.hpp`. The two decodes have to
// agree and no compiler can check that they do, so both are written to be read
// side by side: `render/tests/InstancePacking.cpp` pins the C++ half against the
// matrix the old layout uploaded, and this half is the same three steps in the
// same order.

#ifndef GPU_INSTANCE_WORDS
#define GPU_INSTANCE_WORDS 12
#endif

#if GPU_INSTANCE_WORDS != 12
#error GPU instance row declarations must change together
#endif

// SDL assigns vertex storage buffers to set zero for SPIR-V. The first is the
// stable forty-eight-byte row pool; the second is this draw order's uint slots.
// Three aligned vectors make the generated program issue three row loads. A
// scalar word helper issued fifteen row loads and re-read the indirection for
// every call in the unoptimised SPIR-V the build actually stages.
struct InstanceRow {
	uvec4 Transform0;
	uvec4 Transform1;
	uvec4 Appearance;
};
layout(set = 0, binding = 0) readonly buffer InstanceRows { InstanceRow rows[]; } residentInstances;
layout(set = 0, binding = 1) readonly buffer InstanceIndices { uint slots[]; } drawInstances;
layout(set = 0, binding = 2) readonly buffer SkinOffsets { uint first[]; } skinOffsets;
layout(set = 0, binding = 3) readonly buffer JointWords { uint words[]; } jointRows;

uint InstanceSlot() {
	return drawInstances.slots[gl_InstanceIndex];
}

InstanceRow LoadInstance() {
	return residentInstances.rows[InstanceSlot()];
}

vec3 RotateByQuaternion(vec4 quaternion, vec3 point);

void ApplySkin(uvec4 joints, vec4 weights, inout vec3 position, inout vec3 normal) {
	uint first = skinOffsets.first[InstanceSlot()];
	float total = dot(weights, vec4(1.0));
	if (first == 0xFFFFFFFFu || total <= 0.0) {
		return;
	}

	vec3 skinnedPosition = vec3(0.0);
	vec3 skinnedNormal = vec3(0.0);
	for (uint influence = 0; influence < 4; influence++) {
		if (weights[influence] <= 0.0) {
			continue;
		}
		uint word = (first + joints[influence]) * 5u;
		vec3 translation = vec3(
			uintBitsToFloat(jointRows.words[word]),
			uintBitsToFloat(jointRows.words[word + 1u]),
			uintBitsToFloat(jointRows.words[word + 2u]));
		vec4 raw = vec4(
			unpackSnorm2x16(jointRows.words[word + 3u]),
			unpackSnorm2x16(jointRows.words[word + 4u]));
		float square = dot(raw, raw);
		vec4 rotation = square > 1e-12 ? raw * inversesqrt(square) : vec4(0.0, 0.0, 0.0, 1.0);
		skinnedPosition += (RotateByQuaternion(rotation, position) + translation) * weights[influence];
		skinnedNormal += RotateByQuaternion(rotation, normal) * weights[influence];
	}
	position = skinnedPosition / total;
	normal = skinnedNormal / total;
}

vec3 InstancePosition(InstanceRow instance) {
	return vec3(
		uintBitsToFloat(instance.Transform0.x),
		uintBitsToFloat(instance.Transform0.y),
		uintBitsToFloat(instance.Transform0.z));
}

vec3 InstanceScale(InstanceRow instance) {
	return vec3(
		uintBitsToFloat(instance.Transform1.y),
		uintBitsToFloat(instance.Transform1.z),
		uintBitsToFloat(instance.Transform1.w));
}

// The rotation as a unit quaternion, `xyz` vector part and `w` scalar.
//
// **Renormalised, and that is most of the accuracy.** Four components rounded
// independently land off the unit sphere, and a quaternion off the unit sphere
// is a rotation *and* a scale - so skipping this would show up as geometry
// breathing rather than as a rotation being slightly wrong.
vec4 InstanceRotation(InstanceRow instance) {
	vec4 raw = vec4(
		unpackSnorm2x16(instance.Transform0.w),
		unpackSnorm2x16(instance.Transform1.x));
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
vec3 InstanceWorldPosition(vec4 quaternion, vec3 scale, vec3 position, vec3 meshPosition) {
	return RotateByQuaternion(quaternion, meshPosition * scale) + position;
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
vec3 InstanceWorldNormal(vec4 quaternion, vec3 scale, vec3 meshNormal) {
	bvec3 usable = greaterThan(scale * scale, vec3(1e-12));
	vec3 divisor = mix(vec3(1.0), scale, usable);
	vec3 factor = mix(scale, 1.0 / divisor, usable);
	return RotateByQuaternion(quaternion, meshNormal * factor);
}

// The instance's colour and alpha.
vec4 InstanceColour(InstanceRow instance) {
	return unpackUnorm4x8(instance.Appearance.x);
}

uint InstanceAppearance(InstanceRow instance) {
	return instance.Appearance.y;
}

vec3 InstanceSurfaceColour(InstanceRow instance) {
	return unpackUnorm4x8(instance.Appearance.z).rgb;
}

vec4 InstanceEmission(InstanceRow instance) {
	vec4 packed = unpackUnorm4x8(instance.Appearance.w);
	packed.a *= 16.0;
	return packed;
}
