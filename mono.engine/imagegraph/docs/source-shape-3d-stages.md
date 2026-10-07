# Draw Shape 3D stages

`SourceShape3D.hpp` prepares source controls and the three fragment outputs.
`SourceShape3DGeometry.hpp` builds local triangles for all nine shape selectors,
including source VB order, UVs and normals. The caller retains its allocation
reservation through rendering. Nonpositive Cut Sphere ratios refuse because the
source retains a previous cached model.

`SourceShape3DRaster.hpp` accepts explicitly projected orthographic triangles.
It admits texture validation, triangle work, depth storage and three attachments
before allocation. Failure preserves the previous result. Vertex normals must
already be normalized; geometry preserves source zero normals rather than
inventing them. The raster result contains raw shader attachments and optional
straight-alpha background composition on the surface attachment alone.

These stages do not register successful `pc.shape_3_d` execution. Integration
still needs verified native world/view/projection matrices, framebuffer row
mapping, zero-normal behavior, texture-array binding and source GPU blending.
The geometry and raster suites passed the third joined Composer check batch;
that acceptance covers these stages, not complete node execution or licensed
reference parity.
