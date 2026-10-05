# SPDX-License-Identifier: GPL-3.0-or-later
# Remember graphics and audio settings, then start RunGame.cmd.
Add-Type -AssemblyName System.Windows.Forms
# The console is hidden, so show any failure instead of exiting silently.
$ErrorActionPreference = 'Stop'
trap { [Windows.Forms.MessageBox]::Show("$_", 'DOAXBV launcher') | Out-Null; exit 1 }
$root = Split-Path $PSScriptRoot -Parent
$settingsPath = Join-Path $root 'private\launcher.json'
$heights = 480, 720, 1080, 1440, 2160
$samples = 1, 2, 4, 8
$volumes = 100, 75, 50, 25, 0
$settings = [pscustomobject]@{
    height = [Windows.Forms.Screen]::PrimaryScreen.Bounds.Height; msaa = 1; smaa = $false; volume = 100; shuffle = $false
    vrr = $false; fog = $true; textures = $false; textureFolder = (Join-Path $root 'private\textures\replace'); textureDump = $false
    pixelLighting = $false; smoothNormals = $false; rtgi = $false; rtgiStrength = 1; rtgiHitColor = $true
}
try {
    $saved = Get-Content $settingsPath -Raw -Encoding UTF8 -ErrorAction Stop | ConvertFrom-Json
    # Older settings files omit new options; keep their defaults.
    foreach ($property in $settings.PSObject.Properties) {
        if ($null -ne $saved.($property.Name)) { $property.Value = $saved.($property.Name) }
    }
} catch {}

$form = New-Object Windows.Forms.Form
$form.Text = 'DOAXBV'
$form.FormBorderStyle = 'FixedDialog'
$form.MaximizeBox = $false
$form.StartPosition = 'CenterScreen'
$form.ClientSize = New-Object Drawing.Size 360, 540
$icon = Join-Path $root 'private\doaxbv.ico'
if (Test-Path $icon) { $form.Icon = New-Object Drawing.Icon $icon }

function Add-Choice($text, $top, $items, $index) {
    $label = New-Object Windows.Forms.Label -Property @{ Text = $text; Left = 12; Top = $top + 3; Width = 112 }
    $box = New-Object Windows.Forms.ComboBox -Property @{ Left = 136; Top = $top; Width = 212; DropDownStyle = 'DropDownList' }
    $box.Items.AddRange($items)
    $box.SelectedIndex = [Math]::Max(0, $index)
    $form.Controls.AddRange(@($label, $box))
    $box
}
$heightNames = [string[]]($heights | ForEach-Object { if ($_ -eq 480) { '480p (windowed)' } else { "$($_)p" } })
$nearest = @($heights | Where-Object { $_ -le [int]$settings.height }).Count - 1
$resolution = Add-Choice 'Resolution' 12 $heightNames $nearest
$msaaNames = [string[]]($samples | ForEach-Object { if ($_ -eq 1) { 'Off' } else { "$($_)x" } })
$msaa = Add-Choice 'MSAA' 44 $msaaNames ([Array]::IndexOf($samples, [int]$settings.msaa))
$smaa = New-Object Windows.Forms.CheckBox -Property @{ Text = 'SMAA'; Left = 136; Top = 76; Width = 80; Checked = [bool]$settings.smaa }
$vrr = New-Object Windows.Forms.CheckBox -Property @{ Text = 'VRR display'; Left = 220; Top = 76; Width = 128; Checked = [bool]$settings.vrr }
$fog = New-Object Windows.Forms.CheckBox -Property @{ Text = 'Fog'; Left = 136; Top = 104; Checked = [bool]$settings.fog }
# PowerShell 7 loads JSON numbers as Int64, so cast before finding their index.
$savedVolume = [Array]::IndexOf($volumes, [int]$settings.volume)
$volumeNames = [string[]]($volumes | ForEach-Object { if ($_ -eq 0) { 'Mute' } else { "$_%" } })
$volume = Add-Choice 'Volume' 132 $volumeNames $savedVolume
$shuffle = New-Object Windows.Forms.CheckBox -Property @{ Text = 'Shuffle music'; Left = 136; Top = 164; Width = 212; Checked = [bool]$settings.shuffle }

$textureGroup = New-Object Windows.Forms.GroupBox -Property @{ Text = 'Textures'; Left = 12; Top = 196; Width = 336; Height = 100 }
$textures = New-Object Windows.Forms.CheckBox -Property @{ Text = 'Use texture pack'; Left = 12; Top = 18; Width = 200; Checked = [bool]$settings.textures }
$textureFolder = New-Object Windows.Forms.TextBox -Property @{ Left = 12; Top = 44; Width = 224; ReadOnly = $true; Text = [string]$settings.textureFolder }
$browse = New-Object Windows.Forms.Button -Property @{ Text = 'Folder...'; Left = 244; Top = 42; Width = 80 }
$browse.Add_Click({
    $dialog = New-Object Windows.Forms.FolderBrowserDialog
    $dialog.Description = 'Choose a texture replacement pack'
    $dialog.SelectedPath = $textureFolder.Text
    if ($dialog.ShowDialog($form) -eq 'OK') { $textureFolder.Text = $dialog.SelectedPath }
    $dialog.Dispose()
})
$textureDump = New-Object Windows.Forms.CheckBox -Property @{ Text = 'Dump original textures'; Left = 12; Top = 72; Width = 300; Checked = [bool]$settings.textureDump }
$textureGroup.Controls.AddRange(@($textures, $textureFolder, $browse, $textureDump))

$experimental = New-Object Windows.Forms.GroupBox -Property @{ Text = 'Experimental'; Left = 12; Top = 306; Width = 336; Height = 188 }
$pixelLighting = New-Object Windows.Forms.CheckBox -Property @{ Text = 'Per-pixel lighting'; Left = 12; Top = 18; Width = 300; Checked = [bool]$settings.pixelLighting }
$smoothNormals = New-Object Windows.Forms.CheckBox -Property @{ Text = 'Smooth normals'; Left = 12; Top = 44; Width = 300; Checked = [bool]$settings.smoothNormals }
$rtgi = New-Object Windows.Forms.CheckBox -Property @{ Text = 'Ray-traced GI (RTGI)'; Left = 12; Top = 70; Width = 300; Checked = [bool]$settings.rtgi }
$strengthLabel = New-Object Windows.Forms.Label -Property @{ Text = 'RTGI strength'; Left = 12; Top = 103; Width = 112 }
$strength = New-Object Windows.Forms.NumericUpDown -Property @{ Left = 136; Top = 100; Width = 188; Minimum = 0; Maximum = 4; DecimalPlaces = 2; Increment = 0.25; Value = [Math]::Max([decimal]0, [Math]::Min([decimal]4, [decimal]$settings.rtgiStrength)); Enabled = $rtgi.Checked }
$hitColor = New-Object Windows.Forms.CheckBox -Property @{ Text = 'RTGI hit-point color'; Left = 12; Top = 128; Width = 300; Checked = [bool]$settings.rtgiHitColor; Enabled = $rtgi.Checked }
$rtgi.Add_CheckedChanged({ $strength.Enabled = $rtgi.Checked; $hitColor.Enabled = $rtgi.Checked })
$rtgiNote = New-Object Windows.Forms.Label -Property @{ Text = 'RTGI needs DXR and can slow matches. D3D11 is the fallback.'; Left = 12; Top = 154; Width = 312; Height = 30 }
$experimental.Controls.AddRange(@($pixelLighting, $smoothNormals, $rtgi, $strengthLabel, $strength, $hitColor, $rtgiNote))

$play = New-Object Windows.Forms.Button -Property @{ Text = 'Play'; Left = 273; Top = 506; DialogResult = 'OK' }
$music = New-Object Windows.Forms.Button -Property @{ Text = 'Music folder'; Left = 12; Top = 506; Width = 90 }
$music.Add_Click({
    # Report here; the script-wide trap would close the launcher.
    try {
        $folder = Join-Path $root 'private\UserMusic'
        New-Item -ItemType Directory -Force $folder | Out-Null
        Invoke-Item $folder
    } catch {
        [Windows.Forms.MessageBox]::Show($_.Exception.Message, 'DOAXBV launcher') | Out-Null
    }
})
$hotkeys = New-Object Windows.Forms.Button -Property @{ Text = 'Hotkeys'; Left = 110; Top = 506; Width = 80 }
$hotkeys.Add_Click({
    [Windows.Forms.MessageBox]::Show("Textures (pack or dumping enabled):`nF12 - Dump this frame's textures`nCtrl+F12 - Reload texture pack`n`nRTGI:`nF5/F6 - Decrease/increase strength`nF7 - Split comparison`nF8 - Toggle GI`nF9 - Pause/resume`nF10 - Pause/step one frame", 'DOAXBV hotkeys') | Out-Null
})
$form.Controls.AddRange(@($smaa, $vrr, $fog, $shuffle, $textureGroup, $experimental, $music, $hotkeys, $play))
$form.AcceptButton = $play
if ($form.ShowDialog() -ne 'OK') { exit }

$height = $heights[$resolution.SelectedIndex]
$count = $samples[$msaa.SelectedIndex]
$percent = $volumes[$volume.SelectedIndex]
[pscustomobject]@{
    height = $height; msaa = $count; smaa = $smaa.Checked; volume = $percent; shuffle = $shuffle.Checked
    vrr = $vrr.Checked; fog = $fog.Checked; textures = $textures.Checked; textureFolder = $textureFolder.Text; textureDump = $textureDump.Checked
    pixelLighting = $pixelLighting.Checked; smoothNormals = $smoothNormals.Checked; rtgi = $rtgi.Checked; rtgiStrength = $strength.Value; rtgiHitColor = $hitColor.Checked
} | ConvertTo-Json | Set-Content $settingsPath -Encoding UTF8
# The runner parses the scale with atof, so always write a '.' decimal point.
$env:RECOMP_D3D_SCALE = ($height / 480).ToString([Globalization.CultureInfo]::InvariantCulture)
$env:RECOMP_D3D_MSAA = "$count"
$env:RECOMP_D3D_SMAA = if ($smaa.Checked) { '1' } else { '0' }
$env:RECOMP_AUDIO_GAIN = ($percent / 100).ToString([Globalization.CultureInfo]::InvariantCulture)
$env:RECOMP_MUSIC_SHUFFLE = if ($shuffle.Checked) { '1' } else { '0' }
$env:RECOMP_D3D_VRR = if ($vrr.Checked) { '1' } else { '0' }
$env:RECOMP_D3D_FOG = if ($fog.Checked) { '1' } else { '0' }
$env:RECOMP_TEXTURES = if ($textures.Checked) { $textureFolder.Text } else { $null }
$env:RECOMP_TEXTURE_DUMP = if ($textureDump.Checked) { '1' } else { '0' }
$env:RECOMP_TEXTURE_DUMP_DIR = Join-Path $root 'private\textures'
$env:RECOMP_D3D_PIXEL_LIGHTING = if ($pixelLighting.Checked) { '1' } else { '0' }
$env:RECOMP_D3D_SMOOTH_NORMALS = if ($smoothNormals.Checked) { '1' } else { '0' }
$env:RECOMP_D3D_RTGI = if ($rtgi.Checked) { '1' } else { '0' }
$env:RECOMP_D3D_RTGI_STRENGTH = $strength.Value.ToString([Globalization.CultureInfo]::InvariantCulture)
$env:RECOMP_D3D_RTGI_HITCOLOR = if ($hitColor.Checked) { '1' } else { '0' }
Start-Process -FilePath (Join-Path $root 'RunGame.cmd') -WorkingDirectory $root
