# Source Tile

`pc.tile` follows `Node_Tile` and `sh_tile_ext` at Pixel Composer commit `b69eca232217360cf1502ef0223523d818606652`. grug checks pinned source equations and native graph behavior. licensed runtime pixels remain unverified.

fixed output dimensions use Pixel or Project units. linked dimensions already carry pixels. relative output dimensions multiply the source size by Amount. allocation rounds half to even and clamps each axis to at least one, with the native maximum dimension enforced before allocation. shader uniforms and the stretched rectangle retain raw dimensions. pixels outside the raw rectangle stay transparent.

reference Spacing and the source-spelled `posiiton` use one raw preview-row output size across the selected batch. native `SourceProperties` can carry Integer `preview_index`, default zero, to select that reference row. the existing source-property codec retains this native state through save/load. this does not claim a source PXC JSON preview key or a Studio preview selector. linked vector values already carry pixels. selected output dimensions still vary by execution row.

UV maps sample nearest clamped pixels. red becomes X, flipped green becomes Y, and UV Mix interpolates coordinates. map alpha multiplies output alpha even at zero UV Mix. coordinate mapping subtracts position, divides scale, rotates, applies odd-row or odd-column shift, then recomputes tile indices. negative tile indices use floor-based modulo. Flip Grid mirrors odd tiles; 90 Polar Rotation uses the source quarter-turn equation. spacing and pattern coordinates outside the source tile are transparent.

Tile uses raw `texture2D` and never enables interpolation. the native executor follows the normal source reset state with nearest main sampling. ambient GPU filter changes outside that source processing path are unverified. raw source and UV Atlas bindings are explicitly refused because Tile draws with `draw_surface_stretched`, not its Atlas-safe wrapper.

requested surface depth uses the shared typed format resolver. finite HDR survives float outputs. Tile has no Active, Mask, Mix, Channel, interpolation or oversample input, so no processor finish is added. arrays use the shared Loop, Hold and Expand selection. every selected row validates controls, format and derived coordinates before row execution or output allocation. the complete batch allows 64 million conservative work units. byte allocations use the existing evaluation ledger.

zero scale, zero repeat size, nonfinite derived coordinates and invalid preview rows return named diagnostics. unsupported selector leaves and Mask dimension units return explicit refusals. native source-derived fixtures do not establish licensed pixel parity or source driver allocation behavior when changing surface format.
