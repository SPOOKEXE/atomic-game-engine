# Source Polar

`pc.polar` follows `Node_Polar` and `sh_polar` at Pixel Composer commit `b69eca232217360cf1502ef0223523d818606652`. grug checks source equations and native graph behavior. licensed runtime pixels remain unverified.

forward coordinates use normalized distance from Center and `atan(y, -x) + pi`. inverse coordinates start with half the horizontal UV and a full turn from vertical UV. Radius Mode selects linear, square root, or logarithm. the angle is shifted and divided by the authored range, then compared against the original range endpoints in radians. grug preserves this unusual order. clipped pixels are transparent. Angle subtracts radians; Twist adds distance times its value. Tile applies before `fract`, with both tile components and final coordinates exchanged by Swap Axis. Blend interpolates the original UV with the mapped UV and can extrapolate.

Center in reference units multiplies dimensions in host double precision, then uploads float pixel coordinates. linked values and pixel units already carry pixels. shader division normalizes the uploaded center. source coordinates and control arithmetic use floats.

Angle, Blend and Twist use the shared mapped numeric getter and array selection. default controls project the native map range. explicitly supplied scalar endpoints repeat the scalar. maps interpolate mean RGB between the endpoints. missing maps use the low endpoint. map texture stages force nearest filtering, while bicubic, Lanczos and CleanEdge use the main source dimensions. main texture filtering uses the inherited or selected interpolation. both paths call `texture2Dintp` directly, so Oversample has no effect and raw texture addressing clamps to the edge.

output uses the requested typed surface format, preserving finite HDR in float formats. mask, feather, Mix and Channel run through the shared processor finish. inactive nodes copy the source. main SurfaceAtlas inputs unwrap valid backing pixels. raw auxiliary Atlas bindings return named diagnostics.

all selected rows are admitted before output allocation. the complete batch allows 64 million work units, with conservative per-pixel sampler costs and both feather passes. admission also checks every derived coordinate before drawing. byte allocations use the existing evaluation ledger. zero angular range, nonfinite controls, logarithmic zero radius and unsupported raw array choices return node and port diagnostics. native source-derived fixtures cannot prove licensed pixel parity or platform-specific texture-filter behavior.
