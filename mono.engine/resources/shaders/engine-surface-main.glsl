// The generated main around an authored Surface and Light.
//   SURFACE_LIT      the source defines Light
//   SURFACE_GBUFFER  write the G-buffer instead of one colour
//   SURFACE_BAKE     write the surface itself in UV space, for a baked layer prefix:
//                    albedo and roughness, tangent-space normal and metalness,
//                    emissive and occlusion, then Custom[0] and Custom[1]

#ifdef SURFACE_BAKE
layout(location = 0) out vec4 outBake0;
layout(location = 1) out vec4 outBake1;
layout(location = 2) out vec4 outBake2;
layout(location = 3) out vec4 outBake3;
layout(location = 4) out vec4 outBake4;
#elif defined(SURFACE_GBUFFER)
layout(location = 0) out vec4 outAlbedo;
layout(location = 1) out vec4 outNormal;
layout(location = 2) out vec4 outMaterial;
layout(location = 3) out vec4 outEmissive;
layout(location = 4) out uint outObjectId;
layout(location = 5) out uint outSemanticId;
layout(location = 6) out uint outPartId;
layout(location = 7) out vec2 outMeshUv;
layout(location = 10) flat in uvec3 inLabels;
#else
layout(location = 0) out vec4 outColour;
#endif

#ifndef SURFACE_LIT
vec3 Light(SurfaceOutput s, LightSample l) {
	vec3 view = normalize(lighting.Eye.xyz - inWorldPosition);
	vec3 base = mix(vec3(0.04), s.Albedo, s.Metalness) * clamp(s.Specular, 0.0, 1.0);
	vec3 fresnel = BrdfFresnel(view, l.Direction, base);
	vec3 diffuse = s.Albedo * (1.0 - fresnel) * (1.0 - s.Metalness) * max(dot(s.Normal, l.Direction), 0.0);
	vec3 specular = l.IsSun ? fresnel * BrdfGgx(s.Normal, view, l.Direction, s.Roughness) : vec3(0.0);
	return (diffuse + specular) * l.Colour * l.Shadow;
}
#endif

vec3 ShadeAuthored(SurfaceOutput s) {
	vec3 base = mix(vec3(0.04), s.Albedo, s.Metalness) * clamp(s.Specular, 0.0, 1.0);
	float sky = max(s.Normal.y, 0.0);
	vec3 lit = mix(s.Albedo, base, s.Metalness) *
			   (lighting.Ambient.rgb + lighting.OutdoorAmbient.rgb * sky) * s.Occlusion + s.Emissive;
	LightSample sun;
	sun.Direction = -normalize(lighting.Direction.xyz);
	sun.Colour = lighting.Direct.rgb * WeatherShadowFactor(inWorldPosition);
	sun.Shadow = min(ShadowFactor(normalize(inNormal), sun.Direction), PortalBeamFactor(inWorldPosition));
	sun.IsSun = true;
	lit += Light(s, sun);
	for (int index = 0; index < MAX_LIGHTS; index++) {
		if (index >= int(lights.Count.x)) break;
		vec3 offset = lights.Position[index].xyz - inWorldPosition;
		float range = lights.Position[index].w;
		float distance = length(offset);
		if (distance > range || range <= 0.0) continue;
		LightSample l;
		l.Direction = offset / max(distance, 1e-4);
		float ratio = distance / range;
		float window = max(1.0 - ratio * ratio, 0.0);
		float falloff = window * window / (1.0 + distance * distance);
		float cosine = lights.Direction[index].w;
		if (cosine > -1.0) {
			float aligned = dot(-l.Direction, normalize(lights.Direction[index].xyz));
			if (aligned < cosine) continue;
			falloff *= smoothstep(cosine, mix(cosine, 1.0, 0.1), aligned);
		}
		l.Colour = lights.Colour[index].rgb * falloff;
		l.Shadow = 1.0;
		l.IsSun = false;
		lit += Light(s, l);
	}
	return mix(lit, lighting.FogColour.rgb, FogFactor());
}

void main() {
	if (OutsideUdimTile(inTexCoord, lighting.Flipbook.w)) discard;
	if (!KeepSeamSample(inWorldPosition, lighting.SeamPlane, lighting.SeamFirst, lighting.SeamSecond, lighting.SeamCentre))
		discard;
	SurfaceInput i;
	i.MeshUV = inTexCoord;
	i.UV = fract(inTexCoord) * lighting.Flipbook.x + lighting.Flipbook.yz;
	i.WorldPosition = inWorldPosition;
	i.Normal = normalize(inNormal);
	i.View = normalize(lighting.Eye.xyz - inWorldPosition);
	i.Tint = inColour;
	i.Attributes[0] = inAttribute0;
	i.Attributes[1] = inAttribute1;
	i.Attributes[2] = inAttribute2;
	i.Attributes[3] = inAttribute3;
	i.Time = lighting.Mirror.y;
	SurfaceOutput o;
	o.Albedo = inColour.rgb * lighting.BaseColour.rgb * inSurfaceColour;
	o.Normal = i.Normal;
	o.Roughness = 0.65;
	o.Metalness = 0.0;
	o.Occlusion = 1.0;
	o.Specular = lighting.MaterialExtra.z;
	o.Emissive = (ResolvedSurfaceFeatures() & FEATURE_EMISSION) != 0u ? inEmission.rgb * inEmission.a : vec3(0.0);
	o.Alpha = inColour.a;
	o.Height = 0.5;
	o.User = 0.0;
	for (int index = 0; index < 4; index++) o.Custom[index] = vec4(0.0);
	Surface(i, o);
	o.Roughness = clamp(o.Roughness, 0.045, 1.0);
	o.Metalness = clamp(o.Metalness, 0.0, 1.0);
#ifdef SURFACE_BAKE
	// Tangent space, so the baked normal follows the surface through any pose.
	vec3 tangent = inverse(CotangentFrame(i.Normal, inWorldPosition, inTexCoord)) * o.Normal;
	outBake0 = vec4(o.Albedo, o.Roughness);
	outBake1 = vec4(normalize(tangent) * 0.5 + 0.5, o.Metalness);
	outBake2 = vec4(o.Emissive, o.Occlusion);
	outBake3 = o.Custom[0];
	outBake4 = o.Custom[1];
#elif defined(SURFACE_GBUFFER)
	outAlbedo = vec4(o.Albedo, o.Alpha);
	outMaterial = vec4(o.Roughness, o.Metalness, o.Occlusion, clamp(o.Specular, 0.0, 1.0));
	#ifdef SURFACE_LIT
	// Normal alpha 1/3: a real normal on a pixel deferred lighting leaves alone.
	outNormal = vec4(o.Normal * 0.5 + 0.5, 1.0 / 3.0);
	outEmissive = vec4(ShadeAuthored(o), clamp(o.User, 0.0, 1.0));
	#else
	outNormal = vec4(o.Normal * 0.5 + 0.5, 1.0);
	outEmissive = vec4(o.Emissive, 0.0);
	#endif
	outObjectId = inLabels.x;
	outSemanticId = inLabels.y;
	outPartId = inLabels.z;
	outMeshUv = inTexCoord;
#else
	outColour = vec4(Encode(ShadeAuthored(o)), o.Alpha);
#endif
}
