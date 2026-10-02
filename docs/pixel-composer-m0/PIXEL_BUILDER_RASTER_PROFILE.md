# Pixel Builder CPU raster profile

The native Pixel Builder primitive executors use the authored geometry from pinned
Pixel Composer commit `b69eca232217360cf1502ef0223523d818606652`. Their CPU reference
profile makes the raster rules explicit:

1. Triangles sample pixel centers, orient vertices consistently, and include only
   top and left edges when a sample lies exactly on an edge. Degenerate triangles
   draw nothing. Source quadrilateral triangulation and primitive submission order
   are preserved.
2. Lines use the source endpoints and a quadrilateral of the requested width.
   Coincident endpoints draw a square. Round curve caps use eight polygon segments,
   matching the source helper's precision transition. This line coverage is the
   native reference contract.
3. Ellipses use polygon fans and the captured circle precision. Pie clipping uses
   the source bounding scissor. Rounded rectangle arcs always use the source's
   explicit 32 segments for each quarter. Source corner offsets are preserved.
4. Circle precision is scoped to each native builder. The evaluation request
   supplies the initial value, default 24, restricted to multiples of 4 from 4 through 64.
   Curve round caps change it to 8 for later shapes in evaluation order. Dynamic
   recipes retain the initial captured precision and repeat those transitions.
5. Shape scratch and effect passes use RGBA8. Raster work is bounded to 64 million
   triangle sample checks. Source effect passes and final composition retain their
   distinct blend formulas.

An evaluation requiring `RequireSourceGpuRasterCoverage` refuses these CPU
primitive passes until a captured source renderer is available. Owned dynamic
recipes preserve that requirement.

Exact GameMaker GPU builtin edge coverage, floating vertex conversion, global
renderer-state sharing between different builders, and differences between source
OpenGL and Direct3D backends remain an explicit parity acceptance gate. Native
primitive functionality does not establish that gate. A licensed source renderer
fixture must record backend, circle precision, primitive order and pixel output to
resolve it. Rectangle and diamond source shader cases have dedicated fixtures;
primitive topology and ordered precision transitions have headless tests.
