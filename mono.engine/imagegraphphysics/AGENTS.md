# Source rigid-body host invariants

This shared L10 host consumes copied `imagegraph` commands. It does not extend
the native 3D `physics` module or change that module's solver policy.

- Box2D headers and IDs remain private. Public records contain owned values.
- Source seeks rebuild the complete recorded history, including contact steps.
  Recreating bodies from final poses loses contact history and is forbidden.
- World work is synchronous and single-threaded. Frames never overlap.
- Validate bounded inputs before creating a vendor world. Failures preserve output.
- The native profile pins Box2D 3.1.0. The source bundle's exact Box2D version is
  unknown. Native contact tests do not establish source executable parity.

`RigidReplay` tests enforce copying, reset, contact replay, budgets, and playing
gates. No filesystem, device, clock, or ECS ownership belongs here.
