// Private resident instance and joint layouts, paired with InstancePacking.hpp.
#ifndef GPU_INSTANCE_WORDS
#define GPU_INSTANCE_WORDS 16
#endif
#if GPU_INSTANCE_WORDS != 16
#error GPU instance row declarations must change together
#endif
#ifndef GPU_JOINT_WORDS
#define GPU_JOINT_WORDS 7
#endif
#if GPU_JOINT_WORDS != 7
#error GPU joint row declarations must change together
#endif

struct InstanceRow {
	uvec4 PositionColour;
	uvec4 Rotation;
	uvec4 ScaleAppearance;
	uvec4 SurfaceEmission;
};
layout(set = 0, binding = 0) readonly buffer InstanceRows {
	InstanceRow rows[];
}
residentInstances;
layout(set = 0, binding = 1) readonly buffer InstanceIndices {
	uint slots[];
}
drawInstances;
layout(set = 0, binding = 2) readonly buffer SkinOffsets {
	uint first[];
}
skinOffsets;
layout(set = 0, binding = 3) readonly buffer JointWords {
	uint words[];
}
jointRows;

uint InstanceSlot() {
	return drawInstances.slots[gl_InstanceIndex];
}

InstanceRow LoadInstance() {
	return residentInstances.rows[InstanceSlot()];
}

vec3 RotateByQuaternion(vec4 quaternion, vec3 point);

// The CPU normalizes before upload; avoid another rounding step here.
vec4 DecodePackedRotation(uvec4 words) {
	return uintBitsToFloat(words);
}

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
		uint word = (first + joints[influence]) * GPU_JOINT_WORDS;
		vec3 translation = vec3(
			uintBitsToFloat(jointRows.words[word]),
			uintBitsToFloat(jointRows.words[word + 1u]),
			uintBitsToFloat(jointRows.words[word + 2u])
		);
		vec4 rotation = DecodePackedRotation(uvec4(
			jointRows.words[word + 3u],
			jointRows.words[word + 4u],
			jointRows.words[word + 5u],
			jointRows.words[word + 6u]
		));
		skinnedPosition += (RotateByQuaternion(rotation, position) + translation) * weights[influence];
		skinnedNormal += RotateByQuaternion(rotation, normal) * weights[influence];
	}
	position = skinnedPosition / total;
	normal = skinnedNormal / total;
}

vec3 InstancePosition(InstanceRow instance) {
	return vec3(
		uintBitsToFloat(instance.PositionColour.x),
		uintBitsToFloat(instance.PositionColour.y),
		uintBitsToFloat(instance.PositionColour.z)
	);
}

vec3 InstanceScale(InstanceRow instance) {
	return vec3(
		uintBitsToFloat(instance.ScaleAppearance.x),
		uintBitsToFloat(instance.ScaleAppearance.y),
		uintBitsToFloat(instance.ScaleAppearance.z)
	);
}

// The rotation as a unit quaternion, xyz vector part and w scalar.
vec4 InstanceRotation(InstanceRow instance) {
	return DecodePackedRotation(instance.Rotation);
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
	return unpackUnorm4x8(instance.PositionColour.w);
}

uint InstanceAppearance(InstanceRow instance) {
	return instance.ScaleAppearance.w;
}

vec3 InstanceSurfaceColour(InstanceRow instance) {
	return unpackUnorm4x8(instance.SurfaceEmission.x).rgb;
}

vec4 InstanceEmission(InstanceRow instance) {
	vec4 packed = unpackUnorm4x8(instance.SurfaceEmission.y);
	packed.a *= 16.0;
	return packed;
}
