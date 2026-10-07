# Source Gaussian Noise

grug keeps pinned `pc.noise_gaussian` equations in binary32. source pin is
`b69eca232217360cf1502ef0223523d818606652`. constructor is
`scripts/node_noise_gaussian/node_noise_gaussian.gml`; shader is
`shaders/sh_noise_gaussian/sh_noise_gaussian.fsh` in the official source archive.

raw Dimension sets sprite coverage and texture coordinates. allocation uses
source half-even rounding and a minimum of one pixel. uncovered pixels stay
transparent. wholly uncovered rows do not consume noise controls.

Position is a direct normalized offset. source does not give this input a
Reference unit. shader rotates normalized texture coordinates as a row vector,
multiplies Scale, then subtracts Position. there is no aspect correction.
source declares twelve physical input slots and one Surface Out. it declares
no UV Map, Mask, or mapped numeric controls.

random branch uses `fract(sin(dot(p, vec2(78.233,128.852))) *
(43758.5453 + mod(seed,100000)/10))`. first sample adds `(3.9613,1.6452)`
and clamps to at least `.001`; second adds `(.1654,2.9873)`. native random
branch requires an explicit resolved Seed.

conversion branch samples nearest red from both conversion surfaces at the
transformed coordinates. native sampling clamps at texture edges. Seed and
unused conversion bindings in random mode do not affect output. missing or
raw Atlas conversion bindings in active conversion mode get named refusal.
first conversion red must lie in `(0,1]` for the consumed logarithm and square
root. second red must be finite, and its cosine phase must stay finite.

normal sample is `sqrt(-2*log(first))*cos(2*PI*second)`. result becomes
`mean + normal*varience`, keeping the source spelling. RGB remaps through
Level In and Level Out. alpha is one. source computes a second sine sample
but never uses it; native does not evaluate that unused result.

Use Conversion follows source Bool getter. typed booleans keep their value.
finite numeric values become true only above `.5`, per the English
[GameMaker bool manual](https://manual.gamemaker.io/lts/en/GameMaker_Language/GML_Reference/Variable_Functions/bool.htm).
other unresolved value shapes get a named refusal. surface-to-boolean conversion
needs the source surface handle; native does not invent that handle.

all seven source storage formats remain available. native rejects consumed
nonfinite arithmetic, equal Level In endpoints, and samples outside finite
half-float storage. it prescans every selected processor row before output
allocation or observers. retained byte quote includes the typed raster and
an optional raster field copy.

work quote is `allocatedPixels * 4096` for rows with any covered fragment,
including their uncovered edge pixels. wholly uncovered rows cost
`allocatedPixels * 512`. whole selected batch must fit 64 million work units.
these are conservative admission units, not measured speed claims.

CPU float math and nearest sampling form the native source-equation profile.
grug makes no licensed application pixel parity claim.
