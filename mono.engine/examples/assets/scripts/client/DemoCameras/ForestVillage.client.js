const frame = script.GetAttribute('CFrame');
if (frame != null) {
  const camera = Instance.new('Camera');
  const name = script.GetAttribute('Name');
  camera.Name = typeof name === 'string' ? name : 'Camera';
  camera.CFrame = frame;

  const fieldOfView = script.GetAttribute('FieldOfView');
  if (typeof fieldOfView === 'number') camera.FieldOfView = fieldOfView;

  const nearPlaneZ = script.GetAttribute('NearPlaneZ');
  if (typeof nearPlaneZ === 'number') camera.NearPlaneZ = nearPlaneZ;

  const farPlaneZ = script.GetAttribute('FarPlaneZ');
  if (typeof farPlaneZ === 'number') camera.FarPlaneZ = farPlaneZ;

  const imageWidth = script.GetAttribute('ImageWidth');
  if (typeof imageWidth === 'number') camera.ImageWidth = imageWidth;

  const imageHeight = script.GetAttribute('ImageHeight');
  if (typeof imageHeight === 'number') camera.ImageHeight = imageHeight;

  camera.CameraSubject = null;
  camera.Parent = workspace;
  workspace.CurrentCamera = camera;

  const marker = Instance.new('ObjectValue');
  marker.Name = 'PublishedCamera';
  marker.Value = camera;
  marker.Parent = script;
}
