# Draw Shape 3D stages

`SourceShape3D.hpp` prepares source controls and the three fragment outputs.
`SourceShape3DGeometry.hpp` builds local triangles for all nine shape selectors,
including source VB order, UVs and normals. The caller retains its allocation
reservation through rendering. Nonpositive Cut Sphere ratios refuse because the
source retains a previous cached model.

`SourceShape3DRaster.hpp` accepts explicitly projected orthographic triangles.
It admits texture validation, triangle work, depth storage and three attachments
before allocation. Failure preserves the previous result. Vertex normals must
already be normalized or zero; geometry preserves source zero normals, and
the native CPU profile explicitly leaves zero normals at zero. The raster result contains raw shader attachments and optional
straight-alpha background composition on the surface attachment alone.

`SourceShape3DProjection.hpp` applies the source world stack and fixed camera
with an explicit framebuffer profile. `SourceShape3DExecutor.cpp` registers
`pc.shape_3_d`, binds processor-selected texture lists, and publishes the three
computed attachments. Its engine conventions and remaining reference unknowns
are documented in [the native execution profile](source-shape-3d-native-profile.md).

These stages and their focused suites are subject to joined verification.
Registration does not assert equivalence to the pinned Windows renderer or
licensed reference parity.
