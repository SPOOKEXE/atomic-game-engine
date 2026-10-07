# Pixel Sort

`pc.pixel_sort` follows the pinned `Node_Pixel_Sort` host and `sh_pixel_sort` shader. each iteration compares one adjacent pair per pixel; successive passes change pairing parity. four directions come from the integer getter, then floor division by 90 and modulo four, including negative directions.

horizontal pass zero pairs columns zero and one. vertical pass zero leaves row zero at the edge and pairs rows one and two. out-of-bounds neighbours keep the current pixel. threshold uses RGB luminance weights 0.2126, 0.7152, and 0.0722, with no alpha multiplier.

the shift branch takes the neighbour when current brightness is strictly above Threshold and the neighbour is brighter. the other branch takes the neighbour when neighbour brightness is strictly above Threshold and current brightness is greater or equal. equal-brightness pixels can therefore copy one another's different alpha; the executor preserves this source behavior.

the initial source copy and both ping-pong scratch surfaces use RGBA8 storage. signed and HDR source values quantize before the first positive pass. selected output depth is separate; requesting float output does not make the scratch surfaces float.

nonpositive Iteration copies the original source directly to the selected output depth, before mask, Mix, and Channel processing. inactive nodes use the shared unchanged-source copy. shared executor input validation still rejects malformed surfaces before either early return. positive passes use shared source mask modifiers, Mix, and Channel finishing.

main input uses the source's safe draw and permits valid Atlas input. mask binding is raw and rejects Atlas. source arrays select rows through the shared processor policy, including Loop, Hold, Expand, and Expand inverse.

all selected rows are admitted against 64000000 work units before execution. admission includes iteration work and positive mask feather work; the two actual scratch allocations use the existing live byte budget. missing input, nonfinite controls, malformed surfaces, excessive work, and insufficient memory produce named diagnostics.

native source-derived fixtures do not establish licensed runtime pixel parity. the executor carries `ENGINE_PROFILE("imagegraph.source.pixel_sort")`; no dedicated performance result is claimed here.
