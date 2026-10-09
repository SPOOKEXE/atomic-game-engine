const frame = script.GetAttribute("CFrame");
if (frame == null) {
	return;
}

const camera = Instance.new("Camera");
camera.Name = script.GetAttribute("Name") || "Camera";
camera.CFrame = frame;

for (const property of ["FieldOfView", "NearPlaneZ", "FarPlaneZ", "ImageWidth", "ImageHeight"]) {
	const value = script.GetAttribute(property);
	if (value != null) {
		camera[property] = value;
	}
}

camera.CameraSubject = null;
camera.Parent = workspace;
workspace.CurrentCamera = camera;
const published = Instance.new("ObjectValue");
published.Name = "PublishedCamera";
published.Value = camera;
published.Parent = script;
