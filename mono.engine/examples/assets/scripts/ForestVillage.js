// Paired with ForestVillage.luau. Serial generation and explicit same-tick publication.
const procedural = game.GetService('ProceduralService');
const scopes = [{X:0,Z:0,Cells:16},{X:20,Z:0,Cells:16}];
procedural.ConfigureForestVillage({
  Name:'forest-village',Seed:'42',Regions:scopes,
  Sources:[
    {Kind:'Village',Name:'oakstead',X:8,Z:8},
    {Kind:'Entrance',Name:'west-gate',X:12,Z:8,TargetVillage:'oakstead'},
  ],
});
procedural.SetSpline({Name:'village-road',EntranceName:'west-gate',ControlX:10,ControlZ:9});
const floor = Instance.new('Part');
floor.Name = 'VillageGround';
floor.Anchored = true;
floor.Size = Vector3.new(20,1,20);
floor.Position = Vector3.new(8,-0.5,8);
floor.Color = Color3.fromRGB(55,62,42);
floor.Parent = workspace;
const meshes = [],parts = [],tickets = [];
scopes.forEach((scope,index) => {
  const mesh = Instance.new('EditableMesh');
  mesh.Name = 'GeneratedRegion'+(index+1);
  mesh.Parent = workspace;
  const part = Instance.new('MeshPart');
  part.Name = "GeneratedRegionView"+(index+1);
  part.Anchored = true;
  part.CanCollide = false;
  part.CanQuery = false;
  part.Color = Color3.new(1,1,1);
  part.MeshId = mesh.ContentId;
  part.Parent = workspace;
  meshes.push(mesh);parts.push(part);
  const ticket = procedural.Generate(scope,mesh,part);
  tickets.push(ticket);
  const progress = procedural.Progress(ticket);
  if (progress.Status !== 'Ready') throw new Error(progress.Diagnostic);
  print('Generated region',index+1,progress.Placements,progress.Roads,progress.ArtifactDigest);
});
procedural.Publish(tickets);
tickets.forEach(ticket => procedural.Release(ticket));
// One source edit rebuilds only its affected visible region. The distant region stays accepted.
procedural.SetSource({Kind:'Village',Name:'oakstead',X:9,Z:8});
const replacement = procedural.Generate(scopes[0],meshes[0],parts[0]);
procedural.Publish([replacement]);
procedural.Release(replacement);
// Cancellation preserves the accepted geometry and releases only this ticket.
const canceled = procedural.Generate(scopes[0],meshes[0],parts[0]);
procedural.Cancel(canceled);
if (procedural.Progress(canceled).Status !== 'Canceled') throw new Error('cancellation failed');
procedural.Release(canceled);

const starterPlayerScripts = game.GetService('StarterPlayer').FindFirstChild('StarterPlayerScripts');
if (starterPlayerScripts === null) throw new Error('the starter player scripts container is missing');
const camera = starterPlayerScripts.FindFirstChild('DemoCamera');
if (camera === null) throw new Error('the demo camera companion is missing');
camera.SetAttribute('Name','Forest Village Camera');
camera.SetAttribute('CFrame',CFrame.lookAt(Vector3.new(10,14,22),Vector3.new(10,0,8)));
camera.SetAttribute('FieldOfView',58);
