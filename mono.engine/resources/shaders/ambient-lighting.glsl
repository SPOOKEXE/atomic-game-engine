// Shared ambient arithmetic for deferred shading and retained AO response.
vec3 AmbientRadiance(vec3 albedo, float metalness, vec3 normal,
                     vec3 ambient, vec3 outdoorAmbient, float occlusion) {
    vec3 baseReflectance = mix(vec3(0.04), albedo, metalness);
    vec3 ambientAlbedo = mix(albedo, baseReflectance, metalness);
    return ambientAlbedo * (ambient + outdoorAmbient * max(normal.y, 0.0)) * occlusion;
}
