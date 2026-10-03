# Source Look At value node

Pinned Pixel Composer commit `b69eca232217360cf1502ef0223523d818606652`.
Public type is `pc.quarternion_lookat`, preserving the source spelling.

Origin defaults to `(0,0,0)`, Target to `(1,0,0)`, Up to `(0,0,-1)`.
Unit defaults to Quaternion (0), with Euler (1) as its second choice.
Physical input slots replace the inherited Position, Rotation, Scale and Anchor;
those inherited controls do not participate in this node. The inherited array
processing controls retain their existing source schedules.

The forward vector is `(target.x-origin.x, origin.y-target.y,
target.z-origin.z)`. BBMOD normalizes forward and up, then repeats forward
normalization during Gram-Schmidt, computes `up cross forward`, and converts the
basis using the source largest-diagonal 180-degree branches. There is no final
quaternion normalization. Euler output uses BBMOD roll, pitch and yaw, degrees,
and half-even rounding to three decimal places. The source contains a zero-forward branch returning the four-component identity
before testing Unit. In the native finite-double comparison profile, coincident
points take that branch. Zero or parallel Up yield identity, then convert to a
three-component zero Euler value when selected. These degenerate outcomes do not
establish how licensed runner epsilon comparisons evaluate the same expressions.

The source Vec3 getter repeats scalar numbers, pads short numeric tuples with
zero and truncates long ones. Linked surfaces return `(width,height,0)`.
A whole surface array is a nonsurface to `surface_get_dimension`, hence returns
`(1,1,0)` before processor batching. This getter projection is limited to the
three verified controls on this exact node. Batched output retains each row's
actual three- or four-component value without padding Euler output to four.

The deterministic native arithmetic profile uses finite double arithmetic,
ordinary comparisons and the source normalization threshold `0.00001`.
The [GameMaker manual](https://manual.gamemaker.io/monthly/en/GameMaker_Language/GML_Reference/Maths_And_Numbers/Number_Functions/math_get_epsilon.htm)
confirms the default epsilon, but ambient runtime epsilon and generated runner
comparison operators are not established by this CPU implementation. In
particular, tiny vector behavior is a named native profile, not licensed runner
parity. Small Up vectors can retain nonunit magnitude under the literal BBMOD
threshold. Overflow or nonfinite intermediate coordinates produce a diagnostic
before publishing an output. The existing camera and instancer Look helpers are
unchanged because their up direction and normalization contracts differ.

The kernel has constant stack storage and constant arithmetic work per selected
row. Existing source scheduler element, depth and byte admission bounds all
rows. Direct entry additionally checks the maximum row count. Output containers
and names use the evaluator's reservation and atomic publication machinery.
There is no retained state; existing value keyframes, expressions, replay times,
and native document codecs remain responsible for control resolution.

CPU graph fixtures cover handedness, Euler rounding, all 180-degree branches,
coincident points, zero/parallel/small Up, origin translation, scalar and tuple
getters, surface getters, mixed Unit rows, animation, serialization and refusal
atomicity. These fixtures are prepared for the parent's fresh joined runtime.
The outside direct numeric probe passed eight checks; strict syntax passed.
No compiled graph, licensed GameMaker, GPU or visual parity is claimed here.
