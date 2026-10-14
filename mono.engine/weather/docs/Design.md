# Reference weather

The shared module evaluates small headless queries and verification fixtures.
It does not retain a CPU copy of GPU volume pages. Authored settings live in
`scene::WeatherCompute`; `Compile` validates and quantizes them once, producing
canonical integer inputs and a separately named physical encoding profile.
Persisted `LayerName` text supplies the default layer key through uint32 FNV-1a
(offset 2166136261, multiplier 16777619) followed by the reference avalanche
hash. Process-local name handles never enter field identity. Explicit key
overrides exist for reference fixtures and adapters carrying canonical keys.
Runtime queries and rendering use the same default compilation path.

## Algorithm version 1

Reference noise is periodic every 2^24 canonical cells on each axis. Coordinates
use their low 24 bits, independent of camera rebasing. Cumulative drift is a
modulo-2^32 Q8 sampling position plus twelve retained fractional bits.
Q20 per-tick wind preserves slow motion, and the combined position wraps modulo
2^44. The displacement period and spatial period
agree. Neighbor corner keys wrap at the lattice boundary. Negative wind and
coordinate wrap therefore preserve continuous sampling of the same torus.
The altitude envelope samples canonical cell centres, including one-cell layers.
It uses the full signed Y coordinate and finite layer
bounds. A remote coordinate never wraps into the authored vertical layer.

The hash operates on uint32 words with modular overflow:

1. XOR the word with itself shifted right 16.
2. Multiply by `0x7feb352d`.
3. XOR with itself shifted right 15.
4. Multiply by `0x846ca68b`.
5. XOR with itself shifted right 16.

Corner keys combine seed, hashed layer key, purpose, hashed time epoch and
coordinates multiplied by `0x9e3779b9`, `0x85ebca6b` and `0xc2b2ae35`.
The high eight hash bits provide the corner code. Spatial interpolation uses
Q8 smoothstep `(f*f*(768-2*f)+32768)>>16`, then rounded interpolation
`(a*(256-f)+b*f+128)>>8`. Integer divisions on signed scalar responses truncate
toward zero. No floating-point value enters reference evaluation.

The time epoch is the low 32 bits of `PhaseTick >> 8`. Its Q8 fraction is
`PhaseTick & 255`. Adjacent epochs blend with the same smoothstep; the seeded
noise has a declared 2^40-phase-tick period with matching value and smooth
underlying derivative at the wrap. Byte quantization has the expected discrete
steps. The fractional phase retained by anchors preserves timeline arithmetic;
version 1 reference cells sample whole phase ticks.

Front, moisture, body, erosion, temperature and each wind component use distinct
purpose keys. Fronts correlate moisture, temperature and pressure without
reshuffling unrelated purpose fields. Layer shape multiplies a bounded vertical
envelope, coverage response, growth envelope and moisture surplus. A named
linear cosmetic saturation proxy responds to the authored temperature/lapse
profile. This field is a designed weather approximation, not an atmospheric
forecast or a mass-conserving fluid simulation.

## Time and changes

The scene-owned accepted clock carries one cumulative anchor per authored layer.
`InitialAnchor`, `ResolveTime` and `Reanchor` are pure functions over copied
values. Tick is the base-rate weather clock; accepted time scale applies to
phase and displacement once. Reanchoring resolves the old wind/rate first and
copies cumulative phase/displacement before accepting the new parameters.

Resolution uses bounded word products instead of casting absolute time to a
float. Every uint64 tick, including UINT64_MAX, takes constant work. Phase
arithmetic wraps modulo 2^64; field periodicity uses only its declared phase bits.
Seeking before the retained anchor is refused. A paused anchor accepts later
clock ticks while retaining the same phase, displacement and fractional residue.
Wind changes retain that residue before replacing velocity. Quantized velocity
error is at most half a Q20 cell per clock tick, so position error after N ticks
is at most `N * CellSizeMetres / 2097152` metres relative to the authored scaled
trajectory. Q8 presentation sampling adds less than `CellSizeMetres / 256` metres
of position quantization, independently of the retained residue. Older historical
seeking requires the appropriate saved anchor rather than replay from origin.

## Eight-byte semantic codec

The codec order is condensate, temperature anomaly, vapour, pressure anomaly,
wind X, wind Y, wind Z and ice fraction. Unsigned quantities span 0 through 255.
Signed quantities span -127 through 127 and encode as value plus 127; code 255
is reserved and refused. Code order does not prescribe renderer uint32 word
layout or host byte order. Every legal byte is exercised by the owning suite.

The physical profile declares condensate kg water/m^3, vapour kg water/kg dry
air, signed temperature/pressure scales, and velocity m/s per axis. Conversions
are cosmetic interpretations. Vapour water uses dry-air density times volume;
condensate uses volume alone. Optics explicitly multiplies condensate by a
mass extinction coefficient in m^2/kg. These helpers make units visible and do
not claim conservation for procedural growth or random reconstruction.

## Checks and performance

`engine.weather.weather` covers canonical fixtures, large ticks, signed/world
boundaries, changes of wind, pause/resume, scalar/batch parity, generation order,
codec refusal and water/optics conversion. `engine.weather.bench.weather` reports
cost per scalar cell, per batch cell and per direct timeline seek to stdout.
The batch API writes caller-owned spans and allocates nothing. Optimized builds
evaluate integer hash/noise/interpolation in four uint32 lanes using SSE2 or
NEON, then apply the shared scalar semantic response. Tail cells and unsupported
architectures use the scalar reference. O0 also selects scalar evaluation:
its unoptimized intrinsic wrappers measured slower on the available x86 host.
Exact byte parity is checked across spatial/phase boundaries and every tail
length. The NEON branch has not been executed on this x86 host.
Stateful evolution and its complete checkpoints have separate owning APIs.


## CPU packing comparison

The owning benchmark suite compares two uint32 words, eight byte planes and
software E4M3 normalized values. These are offline CPU conversion fixtures,
not renderer layouts or hardware FP8 support claims. Words and byte planes
round-trip every semantic byte exactly. E4M3 is explicitly lossy: exhaustive
codec-byte checks bound the error to eight unsigned codes and four signed
anomaly/velocity codes. Signed fields normalize around zero rather than encoding
the codec's bias as a physical value. Software lookup tables implement nearest
even rounding and use only the finite normalized [-1,1] subset.

Every compared cell representation still occupies eight bytes. Software FP8
therefore provides no cell payload saving over the semantic byte codec; its
four conversion tables add 1024 bytes. The CPU comparisons do not choose a GPU
storage format, quantify optical image error or replace backend bandwidth and
shader measurements. The benchmark retains identical canonical input across
iterations, and consumes packed data before decoding to prevent folding the
round trip into a copy.


## Certified empty procedural intervals

`DensityUpperBound` bounds the condensate byte over every X/Z coordinate and
future phase in a closed Y interval. It refuses unsupported versions, invalid
canonical controls and reversed intervals. The caller includes the entire
sampling halo; testing eight corners at one tick cannot certify future absence.
A zero bound permits density payload omission. Vapour, temperature and wind
may still be nonzero, and stateful solver history is outside this certificate.

The proof treats each noise code as any integer from 0 through 255. The signed
positive extreme is therefore 128. It bounds vapour above, temperature and
saturation below, and moisture surplus above. The lapse response takes the
endpoint selected by its sign and preserves C++ signed truncation. A page's
cell-centred integer triangle reaches its maximum at an endpoint or one of the
two central layer cells clamped into the interval. Coverage and growth cannot
increase that envelope. The final bound is the integer floor of envelope maximum
times surplus maximum divided by 255. These are dimensionless encoded values,
not a claim of conserved physical water.

The owning suite covers signed coordinate extremes, thin layers, halo contact,
invalid canonical controls, and one million actual evaluator samples using
seeds 1, 7, 19, 101 and 1729. Independent brute force checks cover every interval
for thicknesses one through 64. The scratch mathematical check uses SymPy,
mpmath, NumPy and Z3 outside the project environment, with a fixed two-second
solver limit per lemma; no Python tooling is added to the repository.
