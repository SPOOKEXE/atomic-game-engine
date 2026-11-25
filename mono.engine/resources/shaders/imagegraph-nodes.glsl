// Every imagegraph node kind as one compute dispatch. The including shader declares
// targetImage with its storage format and defines IMAGEGRAPH_WIDE for 16-bit storage.
// Values are in the CPU evaluator's 0..255 scale. Byte storage rounds every read to
// whole bytes so results match the CPU document's numeric contract; wide storage
// keeps the unorm16 steps. Hashing mirrors imagegraph/src/Patterns.hpp in float.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
layout(set = 0, binding = 0) uniform sampler2D firstImage;
layout(set = 0, binding = 1) uniform sampler2D secondImage;
layout(set = 0, binding = 2) uniform sampler2D thirdImage;
layout(set = 0, binding = 3) uniform sampler2D fourthImage;
layout(set = 2, binding = 0, std140) uniform Node {
	uvec4 Control;	// kind, bilinear, source flags, kind options
	ivec4 Extents;	// output size, first input size
	ivec4 Region;	// crop/flip offsets, seeds, input presence, vector size or channel picks
	vec4 Colour;
	vec4 Affine;
	vec4 Parameters;	// transform offset, blend opacity, first row of this dispatch
	vec4 Values[2];
	vec4 StopPositions[4];
	vec4 StopColours[16];
}
node;

const float PI = 3.14159265358979;

vec3 Encode(vec3 linear) {
	linear = clamp(linear, vec3(0.0), vec3(1.0));
	return mix(
		12.92 * linear, 1.055 * pow(linear, vec3(1.0 / 2.4)) - 0.055, greaterThan(linear, vec3(0.0031308))
	);
}
vec4 Quantize(vec4 bytes) {
#ifdef IMAGEGRAPH_WIDE
	return floor(clamp(bytes, vec4(0.0), vec4(255.0)) * 257.0 + 0.5) / 257.0;
#else
	return floor(clamp(bytes, vec4(0.0), vec4(255.0)) + 0.5);
#endif
}
vec4 Read(sampler2D image, ivec2 pixel) {
	return Quantize(texelFetch(image, pixel, 0) * 255.0);
}
vec4 At(ivec2 pixel, bool clampEdges) {
	if (clampEdges) pixel = clamp(pixel, ivec2(0), node.Extents.zw - 1);
	if (any(lessThan(pixel, ivec2(0))) || any(greaterThanEqual(pixel, node.Extents.zw))) return vec4(0.0);
	return Read(firstImage, pixel);
}
vec4 Sample(vec2 position, bool clampEdges) {
	if (any(isnan(position)) || any(isinf(position))) return vec4(0.0);
	if (node.Control.y == 0u) {
		if (any(lessThan(position, vec2(0.0))) || any(greaterThanEqual(position, vec2(node.Extents.zw))))
			return vec4(0.0);
		return At(ivec2(floor(position)), false);
	}
	if (any(lessThan(position, vec2(-0.5))) || any(greaterThan(position, vec2(node.Extents.zw) + 0.5)))
		return vec4(0.0);
	position -= 0.5;
	ivec2 corner = ivec2(floor(position));
	vec2 fraction = position - vec2(corner);
	vec4 total = vec4(0.0);
	for (int row = 0; row < 2; row++) {
		for (int column = 0; column < 2; column++) {
			vec4 pixel = At(corner + ivec2(column, row), clampEdges);
			float weight =
				(column == 0 ? 1.0 - fraction.x : fraction.x) * (row == 0 ? 1.0 - fraction.y : fraction.y);
			total.rgb += pixel.rgb * (pixel.a / 255.0) * weight;
			total.a += pixel.a * weight;
		}
	}
	if (total.a > 0.0) total.rgb *= 255.0 / total.a;
	return total;
}

// image.lookup sampling at normalized coordinates. node.Control.w: 0 clamp, 1 repeat,
// 2 transparent outside. Mirrors detail::Lookup in imagegraph/src/Pixels.hpp.
vec4 LookupTexel(ivec2 pixel) {
	ivec2 size = node.Extents.zw;
	if (node.Control.w == 1u) pixel = ((pixel % size) + size) % size;
	return At(pixel, true);
}
vec4 Lookup(vec2 uv) {
	if (any(isnan(uv)) || any(isinf(uv))) return vec4(0.0);
	if (node.Control.w == 2u && (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))))
		return vec4(0.0);
	vec2 position = uv * vec2(node.Extents.zw);
	if (node.Control.y == 0u) return LookupTexel(ivec2(floor(position)));
	position -= 0.5;
	ivec2 corner = ivec2(floor(position));
	vec2 fraction = position - vec2(corner);
	vec4 total = vec4(0.0);
	for (int row = 0; row < 2; row++) {
		for (int column = 0; column < 2; column++) {
			vec4 pixel = LookupTexel(corner + ivec2(column, row));
			float weight =
				(column == 0 ? 1.0 - fraction.x : fraction.x) * (row == 0 ? 1.0 - fraction.y : fraction.y);
			total.rgb += pixel.rgb * (pixel.a / 255.0) * weight;
			total.a += pixel.a * weight;
		}
	}
	if (total.a > 0.0) total.rgb *= 255.0 / total.a;
	return total;
}

uint Mix(uint x) {
	x ^= x >> 16;
	x *= 0x7feb352du;
	x ^= x >> 15;
	x *= 0x846ca68bu;
	x ^= x >> 16;
	return x;
}
uint Lattice(ivec2 cell, uint seed) {
	return Mix(uint(cell.x) ^ Mix(uint(cell.y) ^ Mix(seed)));
}
float Unit(uint hash) {
	return float(hash) * (1.0 / 4294967296.0);
}
float Fade(float t) {
	return t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
}
float LatticeNoise(uint type, vec2 position, uint seed) {
	vec2 floored = floor(position);
	ivec2 cell = ivec2(floored);
	vec2 t = position - floored;
	float corners[4];
	for (int index = 0; index < 4; index++) {
		ivec2 offset = ivec2(index & 1, index >> 1);
		uint hash = Lattice(cell + offset, seed);
		if (type == 0u)
			corners[index] = Unit(hash);
		else {
			float angle = Unit(hash) * 2.0 * PI;
			corners[index] = dot(vec2(cos(angle), sin(angle)), t - vec2(offset));
		}
	}
	vec2 fade = vec2(Fade(t.x), Fade(t.y));
	float value = mix(mix(corners[0], corners[1], fade.x), mix(corners[2], corners[3], fade.x), fade.y);
	return type == 0u ? value : 0.5 + value * 0.70710678;
}
float Fractal(uint type, vec2 position, uint octaves, float lacunarity, float gain, uint seed) {
	float sum = 0.0, amplitude = 1.0, norm = 0.0, frequency = 1.0;
	for (uint octave = 0u; octave < octaves; octave++) {
		sum += amplitude * LatticeNoise(type, position * frequency, seed + octave * 0x9e3779b9u);
		norm += amplitude;
		amplitude *= gain;
		frequency *= lacunarity;
	}
	return norm > 0.0 ? sum / norm : 0.0;
}
vec2 CellPoint(ivec2 cell, float randomness, uint seed) {
	uint hash = Lattice(cell, seed);
	return vec2(0.5) + (vec2(Unit(hash), Unit(Mix(hash))) - 0.5) * randomness;
}
// x F1, y F2, z edge distance, w cell value.
vec4 Cells(vec2 position, float randomness, uint seed, bool edge) {
	vec2 floored = floor(position);
	ivec2 cell = ivec2(floored);
	vec2 offset = position - floored;
	float f1 = 1e30, f2 = 1e30;
	ivec2 best = ivec2(0);
	vec2 near = vec2(0.0);
	for (int j = -1; j <= 1; j++)
		for (int i = -1; i <= 1; i++) {
			vec2 delta = vec2(i, j) + CellPoint(cell + ivec2(i, j), randomness, seed) - offset;
			float distance = length(delta);
			if (distance < f1) {
				f2 = f1;
				f1 = distance;
				best = ivec2(i, j);
				near = delta;
			} else if (distance < f2)
				f2 = distance;
		}
	float edgeDistance = 0.0;
	if (edge) {
		edgeDistance = 1e30;
		for (int j = -2; j <= 2; j++)
			for (int i = -2; i <= 2; i++) {
				if (ivec2(i, j) == best) continue;
				vec2 delta = vec2(i, j) + CellPoint(cell + ivec2(i, j), randomness, seed) - offset;
				vec2 across = delta - near;
				float span = length(across);
				if (span < 1e-9) continue;
				edgeDistance = min(edgeDistance, dot(0.5 * (near + delta), across) / span);
			}
	}
	return vec4(f1, f2, edgeDistance, Unit(Mix(Lattice(cell + best, seed) ^ 0x68bc21ebu)));
}
// A generator's position: normalized pixel centre, or the Vector input's red and green.
vec2 Position(ivec2 pixel) {
	vec2 uv = (vec2(pixel) + 0.5) / vec2(node.Extents.xy);
	if (node.Region.y == 0) return uv;
	ivec2 size = node.Region.zw;
	ivec2 read = clamp(ivec2(floor(uv * vec2(size))), ivec2(0), size - 1);
	return Read(firstImage, read).rg / 255.0;
}
vec4 Grey(float value) {
	return vec4(vec3(clamp(value, 0.0, 1.0) * 255.0), 255.0);
}
float Factor(vec4 pixel, uint channel) {
	if (channel == 4u) return dot(pixel.rgb, vec3(0.2126, 0.7152, 0.0722)) / 255.0;
	return pixel[channel] / 255.0;
}
float BlendChannel(uint mode, float back, float front) {
	if (mode == 1u) return back * front;
	if (mode == 2u) return back + front - back * front;
	if (mode == 3u) return back <= 0.5 ? 2.0 * back * front : 1.0 - 2.0 * (1.0 - back) * (1.0 - front);
	if (mode == 4u) return min(back + front, 1.0);
	if (mode == 5u) return max(back - front, 0.0);
	if (mode == 6u) {
		if (front <= 0.5) return back - (1.0 - 2.0 * front) * back * (1.0 - back);
		float d = back <= 0.25 ? ((16.0 * back - 12.0) * back + 4.0) * back : sqrt(back);
		return back + (2.0 * front - 1.0) * (d - back);
	}
	if (mode == 7u) return abs(back - front);
	return front;
}
float Arithmetic(uint operation, float a, float b) {
	if (operation == 0u) return a + b;
	if (operation == 1u) return a - b;
	if (operation == 2u) return a * b;
	if (operation == 3u) return b == 0.0 ? 0.0 : a / b;
	if (operation == 4u) return min(a, b);
	if (operation == 5u) return max(a, b);
	return pow(max(a, 0.0), b);
}
vec4 Ramp(float factor) {
	int count = node.Region.x;
	float first = node.StopPositions[0].x;
	if (factor <= first) return node.StopColours[0];
	float last = node.StopPositions[(count - 1) / 4][(count - 1) % 4];
	if (factor >= last) return node.StopColours[count - 1];
	int upper = 1;
	while (upper < count && node.StopPositions[upper / 4][upper % 4] <= factor)
		upper++;
	float low = node.StopPositions[(upper - 1) / 4][(upper - 1) % 4];
	float high = node.StopPositions[upper / 4][upper % 4];
	uint interpolation = node.Control.w >> 4;
	if (interpolation == 1u) return node.StopColours[upper - 1];
	float t = (factor - low) / (high - low);
	if (interpolation == 2u) t = t * t * (3.0 - 2.0 * t);
	return mix(node.StopColours[upper - 1], node.StopColours[upper], t);
}
vec4 Input(int index, ivec2 pixel) {
	if (index == 1) return Read(secondImage, pixel);
	if (index == 2) return Read(thirdImage, pixel);
	if (index == 3) return Read(fourthImage, pixel);
	return Read(firstImage, pixel);
}
float WaveAt(vec2 uv) {
	uint options = node.Control.w;
	float d = (options & 15u) == 0u ? dot(uv, vec2(cos(node.Values[0].y), sin(node.Values[0].y))) * node.Values[0].x
									 : length(uv - 0.5) * node.Values[0].x;
	if (node.Values[0].w != 0.0)
		d += node.Values[0].w *
			 (Fractal(0u, uv * node.Values[1].x, uint(node.Values[1].y), 2.0, 0.5, uint(node.Region.x)) * 2.0 - 1.0);
	d += node.Values[0].z;
	float fraction = d - floor(d);
	uint profile = options >> 4;
	if (profile == 0u) return 0.5 - 0.5 * cos(2.0 * PI * d);
	if (profile == 1u) return fraction;
	return 1.0 - abs(2.0 * fraction - 1.0);
}

void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy) + ivec2(0, int(node.Parameters.w));
	if (any(greaterThanEqual(pixel, node.Extents.xy))) return;
	vec4 result = vec4(0.0);
	uint kind = node.Control.x;
	if (kind == 0u) {
		vec4 sampled = texelFetch(firstImage, pixel, 0);
		if ((node.Control.z & 2u) != 0u) sampled = vec4(sampled.rrr, 1.0);
		if ((node.Control.z & 4u) != 0u) sampled = vec4(sampled.r, 0.0, 0.0, 1.0);
		if ((node.Control.z & 1u) != 0u) sampled.rgb = Encode(sampled.rgb);
		result = sampled * 255.0;
	} else if (kind == 1u) {
		result = node.Colour;
	} else if (kind == 2u) {
		vec2 position = (vec2(pixel) + 0.5) * vec2(node.Extents.zw) / vec2(node.Extents.xy);
		result = Sample(position, true);
	} else if (kind == 3u) {
		result = At(node.Region.xy + pixel, false);
	} else if (kind == 4u) {
		vec2 centre = vec2(pixel) + 0.5;
		vec2 position = vec2(dot(node.Affine.xy, centre), dot(node.Affine.zw, centre)) + node.Parameters.xy;
		result = Sample(position, false);
	} else if (kind == 5u) {
		ivec2 source = pixel;
		if (node.Region.x != 0) source.x = node.Extents.z - 1 - pixel.x;
		if (node.Region.y != 0) source.y = node.Extents.w - 1 - pixel.y;
		result = At(source, false);
	} else if (kind == 6u) {
		vec4 background = At(pixel, false);
		vec4 foreground = Read(secondImage, pixel);
		float opacity = node.Parameters.z;
		if (node.Region.y != 0) opacity *= Read(thirdImage, pixel).r / 255.0;
		uint mode = node.Control.w;
		if (mode == 0u) {
			float front = foreground.a / 255.0 * opacity;
			float back = background.a / 255.0 * (1.0 - front);
			float alpha = front + back;
			if (alpha > 0.0)
				result = vec4((foreground.rgb * front + background.rgb * back) / alpha, alpha * 255.0);
		} else {
			float source = foreground.a / 255.0 * opacity;
			float backdrop = background.a / 255.0;
			float alpha = source + backdrop * (1.0 - source);
			if (alpha > 0.0) {
				for (int channel = 0; channel < 3; channel++) {
					float cb = background[channel] / 255.0, cs = foreground[channel] / 255.0;
					float mixed = (1.0 - source) * backdrop * cb + (1.0 - backdrop) * source * cs +
								  source * backdrop * BlendChannel(mode, cb, cs);
					result[channel] = mixed / alpha * 255.0;
				}
				result.a = alpha * 255.0;
			}
		}
	} else if (kind == 7u) {
		vec2 uv = Position(pixel);
		result = Grey(Fractal(
			node.Control.w,
			uv * node.Values[0].x,
			uint(node.Values[0].y),
			node.Values[0].z,
			node.Values[0].w,
			uint(node.Region.x)
		));
	} else if (kind == 8u) {
		vec2 uv = Position(pixel);
		uint feature = node.Control.w;
		vec4 cells = Cells(uv * node.Values[0].x, node.Values[0].y, uint(node.Region.x), feature == 3u);
		float value = feature == 0u   ? cells.x
					  : feature == 1u ? cells.y
					  : feature == 2u ? cells.y - cells.x
					  : feature == 3u ? cells.z
									  : cells.w;
		result = Grey(value);
	} else if (kind == 9u) {
		vec2 uv = Position(pixel);
		vec2 start = node.Values[0].xy, along = node.Values[0].zw - start;
		float t = node.Control.w == 0u ? dot(uv - start, along) / dot(along, along) : length(uv - start) / length(along);
		result = Grey(t);
	} else if (kind == 10u) {
		result = Grey(WaveAt(Position(pixel)));
	} else if (kind == 11u) {
		result = Ramp(Factor(At(pixel, false), node.Control.w & 15u));
	} else if (kind == 12u) {
		result = At(pixel, false);
		for (int channel = 0; channel < 4; channel++) {
			if ((node.Control.w & (1u << channel)) == 0u) continue;
			float t = (result[channel] / 255.0 - node.Values[0].x) / (node.Values[0].y - node.Values[0].x);
			if (node.Values[1].y != 0.0) t = clamp(t, 0.0, 1.0);
			if (t > 0.0) t = pow(t, 1.0 / node.Values[1].x);
			result[channel] = (node.Values[0].z + t * (node.Values[0].w - node.Values[0].z)) * 255.0;
		}
	} else if (kind == 13u) {
		result = At(pixel, false);
		vec4 other = node.Region.y != 0 ? Read(secondImage, pixel) / 255.0 : node.Values[0];
		uint operation = node.Control.w & 15u, mask = node.Control.w >> 4;
		for (int channel = 0; channel < 4; channel++)
			if ((mask & (1u << channel)) != 0u)
				result[channel] = Arithmetic(operation, result[channel] / 255.0, other[channel]) * 255.0;
	} else if (kind == 14u) {
		for (int channel = 0; channel < 4; channel++) {
			int pick = node.Region[channel];
			if (pick == -1)
				result[channel] = 0.0;
			else if (pick == -2)
				result[channel] = 255.0;
			else
				result[channel] = Input(pick >> 2, pixel)[pick & 3];
		}
	} else if (kind == 15u) {
		result = At(pixel, false);
		for (int channel = 0; channel < 4; channel++)
			if ((node.Control.w & (1u << channel)) != 0u) result[channel] = 255.0 - result[channel];
	} else if (kind == 16u) {
		// Pass 0 premultiplies and blurs rows into wide scratch; pass 1 blurs columns of
		// the scratch and unpremultiplies.
		int reach = int(ceil(node.Values[0].x));
		float sigma = node.Values[0].x / 3.0;
		bool vertical = node.Control.w == 1u;
		vec4 sum = vec4(0.0);
		float total = 0.0;
		for (int offset = -reach; offset <= reach; offset++) {
			float weight = exp(-float(offset * offset) / (2.0 * sigma * sigma));
			total += weight;
			ivec2 at = clamp(pixel + (vertical ? ivec2(0, offset) : ivec2(offset, 0)), ivec2(0), node.Extents.zw - 1);
			vec4 texel = texelFetch(firstImage, at, 0);
			if (!vertical) {
				texel = Quantize(texel * 255.0) / 255.0;
				texel.rgb *= texel.a;
			}
			sum += texel * weight;
		}
		sum /= total;
		if (!vertical)
			result = sum * 255.0;
		else {
			result.a = sum.a * 255.0;
			if (sum.a > 0.0) result.rgb = sum.rgb / sum.a * 255.0;
		}
	} else if (kind == 17u) {
		uint channel = node.Control.w & 15u;
		float strength = node.Values[0].x;
		float dx = (Factor(At(pixel + ivec2(1, 0), true), channel) - Factor(At(pixel - ivec2(1, 0), true), channel)) *
				   0.5 * strength;
		float dy = (Factor(At(pixel + ivec2(0, 1), true), channel) - Factor(At(pixel - ivec2(0, 1), true), channel)) *
				   0.5 * strength;
		vec3 normal = normalize(vec3(-dx, (node.Control.w >> 4) != 0u ? -dy : dy, 1.0));
		result = vec4((normal * 0.5 + 0.5) * 255.0, 255.0);
	} else if (kind == 18u) {
		vec2 vector = Read(secondImage, pixel).rg / 255.0;
		vec2 position = vec2(pixel) + 0.5 + (vector * 2.0 - 1.0) * node.Values[0].x;
		position = clamp(position, vec2(0.5), vec2(node.Extents.zw) - 0.5);
		result = Sample(position, true);
	} else if (kind == 20u) {
		vec4 vector = Read(secondImage, pixel);
		if (vector.a > 0.0) result = Lookup(vector.rg / 255.0 * node.Values[0].xy + node.Values[0].zw);
	}
	imageStore(targetImage, pixel, Quantize(result) / 255.0);
}
