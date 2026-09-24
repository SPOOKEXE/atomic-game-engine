# Supplied media observations

The exact file and decoded-frame hashes are in `supplied-fixture-manifest.json`. These observations come from viewing the supplied media. They do not establish which PXC project produced a capture, the hidden graph values, or reference output for an isolated node.

| Capture | Visible reference |
|---|---|
| `005_nodes.png` | Pixel Composer title bar shows `v. 1.16.6.0`. The graph canvas contains a grid of 2D generator and filter examples. A left preview shows a grayscale image. The project settings panel visibly shows a 128 by 128 default surface. The timeline is at frame 11 of 30. |
| `008_3d.png` | Title bar shows `v. 1.16.6.0`. The canvas connects tile images through 3D cube, UV remap, repeat, light, scene, camera, and render nodes. The left preview shows a textured 3D arrangement. The timeline is at frame 1 of 30. The right inspector is on Directional Light. |
| `001_effect_2.gif` | Fifteen captured UI frames show a multicolor animated effect, a connected graph with compose/output nodes, an inspector, and timeline playback. |
| `006_effects.gif` | Thirty captured UI frames show a red 2D animated effect and a graph with gradient, grid, blur, edge, curve, colorize, scale, and export stages. The inspector shows Draw Gradient. |
| `007_simulation.gif` | Thirty captured UI frames show a white spiral or vortex evolving over time, a graph with emitter, vortex, render-domain, alpha-to-gray, level, and colorize stages, and an Add Emitter inspector. |

The PNGs and GIFs are captured editor screens, not clean node output images. Their hashes are useful for visual studies and traceability, but whole-screen equality is not a meaningful parity check for a native engine output. Controlled image, parameter, tick, and diagnostic captures remain unrecorded.
