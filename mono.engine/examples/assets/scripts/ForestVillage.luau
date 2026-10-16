-- Paired with ForestVillage.js. Serial generation and explicit same-tick publication.
local procedural = game:GetService("ProceduralService")
local scopes = {{X = 0, Z = 0, Cells = 16}, {X = 20, Z = 0, Cells = 16}}
procedural:ConfigureForestVillage({
    Name = "forest-village", Seed = "42", Regions = scopes,
    Sources = {
        {Kind = "Village", Name = "oakstead", X = 8, Z = 8},
        {Kind = "Entrance", Name = "west-gate", X = 12, Z = 8, TargetVillage = "oakstead"},
    },
})
procedural:SetSpline({Name = "village-road", EntranceName = "west-gate", ControlX = 10, ControlZ = 9})
local floor = Instance.new("Part")
floor.Name = "VillageGround"
floor.Anchored = true
floor.Size = Vector3.new(20, 1, 20)
floor.Position = Vector3.new(8, -0.5, 8)
floor.Color = Color3.fromRGB(55, 62, 42)
floor.Parent = workspace
local meshes, parts, tickets = {}, {}, {}
for index, scope in scopes do
    local mesh = Instance.new("EditableMesh")
    mesh.Name = "GeneratedRegion" .. index
    mesh.Parent = workspace
    local part = Instance.new("MeshPart")
    part.Name = "GeneratedRegionView" .. index
    part.Anchored = true
    part.CanCollide = false
    part.CanQuery = false
    part.Color = Color3.new(1, 1, 1)
    part.MeshId = mesh.ContentId
    part.Parent = workspace
    meshes[index], parts[index] = mesh, part
    tickets[index] = procedural:Generate(scope, mesh, part)
    local progress = procedural:Progress(tickets[index])
    assert(progress.Status == "Ready", progress.Diagnostic)
    print("Generated region", index, progress.Placements, progress.Roads, progress.ArtifactDigest)
end
procedural:Publish(tickets)
for _, ticket in tickets do procedural:Release(ticket) end
-- One source edit rebuilds only its affected visible region. The distant region stays accepted.
procedural:SetSource({Kind = "Village", Name = "oakstead", X = 9, Z = 8})
local replacement = procedural:Generate(scopes[1], meshes[1], parts[1])
procedural:Publish({replacement})
procedural:Release(replacement)
-- Cancellation preserves the accepted geometry and releases only this ticket.
local canceled = procedural:Generate(scopes[1], meshes[1], parts[1])
procedural:Cancel(canceled)
assert(procedural:Progress(canceled).Status == "Canceled")
procedural:Release(canceled)

local starterPlayerScripts = game:GetService("StarterPlayer"):FindFirstChild("StarterPlayerScripts")
local camera = starterPlayerScripts and starterPlayerScripts:FindFirstChild("DemoCamera")
if camera ~= nil then
	camera:SetAttribute("Name", "Forest Village Camera")
	camera:SetAttribute("CFrame", CFrame.lookAt(Vector3.new(10, 14, 22), Vector3.new(10, 0, 8)))
	camera:SetAttribute("FieldOfView", 58)
end
