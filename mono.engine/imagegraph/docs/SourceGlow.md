# Source Glow

`pc.glow` follows `Node_Glow` and `sh_glow` at pinned Pixel Composer commit `b69eca232217360cf1502ef0223523d818606652`. grug checks shader arithmetic, not licensed runtime captures.

Greyscale compares alpha-weighted Rec. 709 brightness. Alpha compares coverage. Outer searches for a larger sample; Inner searches for a smaller one. each ring starts at the positive X axis, then visits angles through the inclusive endpoint. rings begin at one pixel and stop before Size. the angular count is `max(64, Size * 4)`. first hit wins. plain nearest texture reads clamp to the image edge.

Pixel Distance uses the ring radius. turning it off measures the distance between floored pixel coordinates, including coordinates outside the image before texture clamping. Size and Strength maps interpolate their ranges with mean RGB. the optional Strength curve shapes `1 - distance / size` before multiplication by mapped Strength. the shared shader curve evaluator uses float values and the source's strict step comparison.

blend slots remain source slots: Normal 0, Replace 1, Lighten 3, Screen 4, Darken 6, Multiply 7. Replace clamps strength. Darken uses strength in both its target color and final interpolation. Greyscale preserves source alpha. outer Alpha uses glow RGB and interpolates alpha from zero to one, regardless of the ordinary blend target. Texture multiplies the glow color; Blend Color mixes it with the winning sample. Draw Original and no-hit defaults follow the shader. Border is uploaded but unused by the pinned shader.

output retains the requested processor surface format. normalized formats clamp; float formats retain finite HDR. mask, feather, and Mix finish through the shared processor path. inactive nodes copy the original surface. main SurfaceAtlas inputs unwrap their surface, matching `draw_surface_safe`. raw auxiliary Atlas bindings and undefined blend separators return named diagnostics.

all selected processor rows are quoted before the first output allocation. the complete batch has 64 million work units: 256 per active pixel, 16 per possible radial tap, bounded curve work, and both feather passes. conservative search quoting includes pixels whose shader might exit early. byte allocations use the existing evaluation ledger. nonfinite uniforms and mapped values are rejected. curves beyond the portable GLSL nine-anchor layout are explicitly refused.
