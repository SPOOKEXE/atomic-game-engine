# Native planar path modifiers

The pinned source revision is `b69eca232217360cf1502ef0223523d818606652`.

| Route | Source script SHA-256 | Native behavior |
| --- | --- | --- |
| `pc.path_to_curve` | `4804812f056e400c2b00e5966f2cedea785877ba4a442a741407221d5975ed22` | Samples line zero into the source six-number curve header and six-number anchors. Bounds normalization uses minimum width and height one. Endpoint samples retain the child path's wrapping or clamping behavior. |
| `pc.path_redistribute` | `168da5716b846420c72d0c1db4eeebda417350b8e483d2d8b713f0f6959a49bb` | Stores the source `curveMap` table of 33 samples at tolerance `0.00001`. Samples clamp the caller ratio, interpolate adjacent table entries, then sample the child path. NaN ratios map directly to child ratio zero as the source getter does. |
| `pc.path_skew` | `3c4e417b0b8a2b9f50fe1950806ea3f6ec5888a093a6ed2f26105964c09730ae` | Applies X or Y shear around the unit-resolved center. Preserves child lengths, accumulated lengths, bounds and weights. Source distance samples always use child line zero, including calls that request another line. |

Redistribute uses the existing processor scheduler for outer path and curve
arrays. These routes retain their source no-write behavior through the bounded
latest-value DataReplay entry. Path to Curve starts with `CURVE_DEF_01`; Skew
starts with source noone, represented by `UndefinedValue`. Redistribute's
initial output is the source processor object itself, which has no path sampling
methods. Evaluating that initial branch receives an explicit diagnostic until a
valid path has produced a native path value. A subsequent missing input preserves
that owned path. This does not invent a native path for an unusable source object.

Curve output resolution is restricted to positive integers below the native
256-anchor limit. All path trees obey existing depth, slot and payload limits.
The whole processor batch shares a 16,777,216 work ceiling, admitted using division
before output tree or sample-table growth. Candidate outputs and replay state
remain unchanged on failure. Redistribute tables occupy fixed payload storage;
Skew controls and tables participate in native validation and persistence.

The native profile stores immutable path values. Source memoization and source
handle aliases remain outside the executable parity claim. Skew's source cache
uses distance without a line key; the native line-zero sampling preserves the
observable geometric result without retaining source handle identity. Spatial
Path3D adapters remain explicitly refused by these planar kernels.
