--!strict
-- A dock widget, a toggle button and a setting that survives a restart.
local wasOpen = plugin:GetSetting("open") == true

local info = DockWidgetPluginGuiInfo.new(Enum.InitialDockState.Right, wasOpen, false, 240, 160, 160, 120)
local widget = plugin:CreateDockWidgetPluginGui("DockPanel", info)
widget.Title = "Dock Panel"

local label = Instance.new("TextLabel")
label.Size = UDim2.fromScale(1, 1)
label.Text = "Hello from a dock widget"
label.Parent = widget

local toolbar = plugin:CreateToolbar("Dock Panel")
local button = toolbar:CreateButton("dock-panel-toggle", "Show or hide the panel", "", "Panel")
button:SetActive(widget.Enabled)

local function setOpen(open: boolean)
	widget.Enabled = open
	button:SetActive(open)
	plugin:SetSetting("open", open)
end

button.Click:Connect(function()
	setOpen(not widget.Enabled)
end)

widget:BindToClose(function()
	setOpen(false)
end)

-- Copies the panel text and saves it through a file dialog. The dialog picks
-- the path; the plugin only suggests a name.
local export = plugin:CreatePluginAction("dock-panel-export", "Export Panel Text", "Save the panel text to a file")
export.Triggered:Connect(function()
	plugin:SetClipboard(label.Text)
	local saved = plugin:SaveFile("dock-panel.txt", label.Text, { { Name = "Text", Pattern = "txt" } })
	if saved then
		label.Text = "Saved " .. saved
	end
end)
