bool KeepSeamSample(vec3 position, vec4 plane, vec4 firstMode, vec4 second, vec4 centre) {
	if (dot(plane.xyz, plane.xyz) == 0.0) return true;
	const float signedDistance = dot(position, plane.xyz) - plane.w;
	const uint mode = uint(firstMode.w + 0.5);
	if (mode == 0u) return signedDistance >= 0.0;
	const vec3 relative = position - centre.xyz;
	const float firstLength = dot(firstMode.xyz, firstMode.xyz);
	const float secondLength = dot(second.xyz, second.xyz);
	const bool inside = firstLength > 0.0 && secondLength > 0.0 &&
		abs(dot(relative, firstMode.xyz)) <= firstLength && abs(dot(relative, second.xyz)) <= secondLength;
	if (mode == 1u) return signedDistance >= 0.0 || !inside;
	return signedDistance >= 0.0 && inside;
}
