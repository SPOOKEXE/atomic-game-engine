// engine/brdf: lobes an authored Light function composes. Each returns the
// response to one light before its colour; none of them knows what it shades.

#ifndef ENGINE_BRDF
#define ENGINE_BRDF

float BrdfGgx(vec3 normal, vec3 view, vec3 toLight, float roughness) {
	vec3 halfway = normalize(view + toLight);
	float lambert = max(dot(normal, toLight), 0.0);
	float facing = max(dot(normal, view), 0.0);
	float shape = DistributionGGX(normal, halfway, roughness);
	float geometry = GeometrySchlickGGX(facing, roughness) * GeometrySchlickGGX(lambert, roughness);
	return shape * geometry / max(4.0 * facing * lambert, 1e-4) * lambert;
}

vec3 BrdfFresnel(vec3 view, vec3 toLight, vec3 reflectance) {
	return FresnelSchlick(max(dot(normalize(view + toLight), view), 0.0), reflectance);
}

// Lambert widened past the terminator by `wrap` (0 none, 1 half the sphere).
float BrdfWrap(float cosine, float wrap) {
	return clamp((cosine + wrap) / ((1.0 + wrap) * (1.0 + wrap)), 0.0, 1.0);
}

// Clamped cosine convolved with a Gaussian per channel. `spread` is an angle:
// scatter distance times surface curvature.
vec3 BrdfPreIntegrated(float cosine, vec3 spread) {
	vec3 s = clamp(spread, vec3(0.005), vec3(0.75));
	vec3 t = vec3(cosine) / s;
	vec3 density = 0.39894228 * exp(-0.5 * t * t);
	vec3 cumulative = 1.0 / (1.0 + exp(-1.702 * t));
	return max(cosine * cumulative + s * density, vec3(0.0));
}

// Light through a thin part, per channel. `thickness` and `falloff` share units.
vec3 BrdfTransmission(vec3 normal, vec3 toLight, float thickness, vec3 falloff) {
	float behind = clamp((dot(-normal, toLight) + 0.3) / 1.3, 0.0, 1.0);
	return behind * exp(-thickness / max(falloff, vec3(1e-5)));
}

// Sheen at grazing angles, for cloth and fuzz.
float BrdfSheen(vec3 normal, vec3 view, vec3 toLight, float roughness) {
	vec3 halfway = normalize(view + toLight);
	float inverse = 1.0 / max(roughness, 0.05);
	float sine = sqrt(max(1.0 - pow(max(dot(normal, halfway), 0.0), 2.0), 1e-4));
	float shape = (2.0 + inverse) * pow(sine, inverse) / 6.2831853;
	float lambert = max(dot(normal, toLight), 0.0);
	float facing = max(dot(normal, view), 0.0);
	return shape * lambert / max(4.0 * (lambert + facing - lambert * facing), 1e-3);
}

// Highlight along `tangent`, for hair and brushed metal (Kajiya-Kay).
float BrdfStrand(vec3 tangent, vec3 view, vec3 toLight, float exponent) {
	float along = dot(tangent, normalize(view + toLight));
	return pow(sqrt(max(1.0 - along * along, 0.0)), exponent);
}

// Curvature of the interpolated surface, in inverse world units.
float SurfaceCurvature() {
	vec3 across = fwidth(normalize(inNormal));
	return length(across) / max(length(fwidth(inWorldPosition)), 1e-5);
}

#endif
