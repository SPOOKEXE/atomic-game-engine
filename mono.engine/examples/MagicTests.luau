--!strict
-- The ported libraries' own test suite, run by this engine's Luau.
--
-- **This is the parity check, and it is the reason the port is credible.**
-- `MagicCore` and `TerrainCore` came from a Rojo project with 183 data tests
-- that ran in Studio. Those tests came across with the libraries and are run
-- here, unmodified apart from one line: `Run.luau` uses `FindFirstChild` where
-- the Roblox original used `WaitForChild`. Everything else - the harness, every
-- spec, the inventory guard - is the file that shipped.
--
-- **That line stays changed now the engine has a `WaitForChild`**, and `Run.luau`
-- says why: the tree is fully mounted before any script runs, a wait suspends
-- until a barrier, and there is no beat inside a scene load - so waiting would
-- suspend this chunk and the 215 specs would silently never run. The engine's
-- form also takes a required timeout, so the line could not have gone back
-- verbatim in any case.
--
-- The count is 215 now. **The 183 are still the parity check and the 32 since
-- are not**: those cover `Noise`'s simplex, Worley and derivative samplers and
-- the `NoiseMap` grids, which were written in this tree and never ran in Studio.
-- The distinction matters only if the port is ever re-checked against its
-- origin, and it is cheap to record now and impossible to reconstruct later.
--
-- A failure raises, so `LoadScene` returns false with the first failure in its
-- error. `mono.engine/examples/tests/Scene.cpp` loads this scene, which is what
-- makes the suite part of `just test` rather than something somebody remembers
-- to run.
--
-- ## What is not here
--
-- The `live` suite, which built real `ParticleEmitter`, `Beam`, `Trail`,
-- `PointLight` and `Attachment` instances and asserted the particle-tail release
-- deadline. This engine has none of those classes, so the suite was **dropped
-- rather than stubbed** - a mock that asserted against a fake emitter would
-- report a pass for a pipeline that does not exist, which is worse than an
-- honest gap. `MagicRuntime` is absent for the same reason.
--
-- Everything the live suite covered is about turning compiled data into
-- Instances. Nothing it covered is about whether the data is right, and that is
-- the whole of what runs here.

-- The modules are children of this script rather than of `ReplicatedStorage` -
-- see `Magic.luau` for the whole of why.
local Modules = script :: any

local MagicCore = Modules.MagicCore
local TerrainCore = Modules.TerrainCore

local Run = require(Modules.MagicTests.Run)

local report = Run.data(MagicCore, TerrainCore)

print(report.summary)

for _, failure in report.failures do
	warn(`magic tests: {failure}`)
end

-- The world still has to build something, or `LoadScene` measures a scene one
-- unit across. One part, named after the outcome, so a run that reported nothing
-- is distinguishable from a run that did not happen.
local marker = Instance.new("Part")
marker.Name = if report.failed > 0 then "MagicTestsFailed" else "MagicTestsPassed"
marker.Anchored = true
marker.Size = Vector3.new(4, 4, 4)
marker.Color = if report.failed > 0 then Color3.new(0.8, 0.2, 0.2) else Color3.new(0.2, 0.75, 0.35)
marker.Parent = workspace

local eye = Instance.new("Camera")
eye.Name = "Eye"
eye.CFrame = CFrame.lookAt(Vector3.new(8, 6, 12), Vector3.new(0, 0, 0))
eye.Parent = workspace
workspace.CurrentCamera = eye

-- Raised last, so the marker and the log survive a failing run.
assert(
	report.failed == 0,
	`{report.failed} of {report.executed} magic test(s) failed; the first is: {report.failures[1]}`
)
