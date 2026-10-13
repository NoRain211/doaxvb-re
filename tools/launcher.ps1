# SPDX-License-Identifier: GPL-3.0-or-later
# Pick resolution and anti-aliasing, remember them, then start RunGame.cmd.
Add-Type -AssemblyName System.Windows.Forms
# The console is hidden, so show any failure instead of exiting silently.
$ErrorActionPreference = 'Stop'
trap { [Windows.Forms.MessageBox]::Show("$_", 'DOAXBV launcher') | Out-Null; exit 1 }
$root = Split-Path $PSScriptRoot -Parent
$settingsPath = Join-Path $root 'private\launcher.json'
$heights = 480, 720, 1080, 1440, 2160
$samples = 1, 2, 4, 8
$volumes = 100, 75, 50, 25, 0
# What the game does when its title screen is left idle; see docs/title-idle.md.
$idleValues = 'default', 'no-cutscene', 'cpu-exhibition'
$idleNames = [string[]]('Default', 'No cutscene', 'CPU Exhibition')
$settings = [pscustomobject]@{ height = [Windows.Forms.Screen]::PrimaryScreen.Bounds.Height; msaa = 1; smaa = $false; sun_shadows = $false; volume = 100; shuffle = $false; demo_cut_hold = $false; title_idle = 'default'; unlock_swimsuits = $false }
try { $settings = Get-Content $settingsPath -Raw -ErrorAction Stop | ConvertFrom-Json } catch {}

$form = New-Object Windows.Forms.Form
$form.Text = 'DOAXBV'
$form.FormBorderStyle = 'FixedDialog'
$form.MaximizeBox = $false
$form.StartPosition = 'CenterScreen'
$form.ClientSize = New-Object Drawing.Size 240, 322
$icon = Join-Path $root 'private\doaxbv.ico'
if (Test-Path $icon) { $form.Icon = New-Object Drawing.Icon $icon }

function Add-Choice($text, $top, $items, $index) {
    $label = New-Object Windows.Forms.Label -Property @{ Text = $text; Left = 12; Top = $top + 3; Width = 80 }
    $box = New-Object Windows.Forms.ComboBox -Property @{ Left = 100; Top = $top; Width = 126; DropDownStyle = 'DropDownList' }
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
$smaa = New-Object Windows.Forms.CheckBox -Property @{ Text = 'SMAA'; Left = 100; Top = 76; Checked = [bool]$settings.smaa }
# Off keeps the game's original shadows for lower-powered PCs; see docs/sun-shadow.md.
$sun = New-Object Windows.Forms.CheckBox -Property @{ Text = 'Sun shadows'; Left = 100; Top = 100; Width = 126; Checked = [bool]$settings.sun_shadows }
# PowerShell 7 loads JSON numbers as Int64, so cast. Settings saved before
# 0.5.2 have no volume; casting $null would give 0 (Mute), so use -1 (100%).
$savedVolume = if ($null -ne $settings.volume) { [Array]::IndexOf($volumes, [int]$settings.volume) } else { -1 }
$volumeNames = [string[]]($volumes | ForEach-Object { if ($_ -eq 0) { 'Mute' } else { "$_%" } })
$volume = Add-Choice 'Volume' 128 $volumeNames $savedVolume
$shuffle = New-Object Windows.Forms.CheckBox -Property @{ Text = 'Shuffle music'; Left = 100; Top = 156; Width = 126; Checked = [bool]$settings.shuffle }
$demoCut = New-Object Windows.Forms.CheckBox -Property @{ Text = 'Shorter demo cuts'; Left = 100; Top = 180; Width = 126; Checked = [bool]$settings.demo_cut_hold }
# Settings saved before this option have no unlock_swimsuits; $null casts to off.
$unlock = New-Object Windows.Forms.CheckBox -Property @{ Text = 'Unlock swimsuits'; Left = 100; Top = 204; Width = 126; Checked = [bool]$settings.unlock_swimsuits }
# Settings saved before this option have no title_idle; IndexOf gives -1, Default.
$idleLabel = New-Object Windows.Forms.Label -Property @{ Text = 'Title screen idle behavior'; Left = 12; Top = 236; Width = 214 }
$idle = New-Object Windows.Forms.ComboBox -Property @{ Left = 12; Top = 254; Width = 214; DropDownStyle = 'DropDownList' }
$idle.Items.AddRange($idleNames)
$idle.SelectedIndex = [Math]::Max(0, [Array]::IndexOf($idleValues, [string]$settings.title_idle))
$play = New-Object Windows.Forms.Button -Property @{ Text = 'Play'; Left = 151; Top = 286; DialogResult = 'OK' }
$music = New-Object Windows.Forms.Button -Property @{ Text = 'Music folder'; Left = 12; Top = 286; Width = 90 }
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
$form.Controls.AddRange(@($smaa, $sun, $shuffle, $demoCut, $unlock, $idleLabel, $idle, $music, $play))
$form.AcceptButton = $play
if ($form.ShowDialog() -ne 'OK') { exit }

$height = $heights[$resolution.SelectedIndex]
$count = $samples[$msaa.SelectedIndex]
$percent = $volumes[$volume.SelectedIndex]
[pscustomobject]@{ height = $height; msaa = $count; smaa = $smaa.Checked; sun_shadows = $sun.Checked; volume = $percent; shuffle = $shuffle.Checked; demo_cut_hold = $demoCut.Checked; title_idle = $idleValues[$idle.SelectedIndex]; unlock_swimsuits = $unlock.Checked } |
    ConvertTo-Json | Set-Content $settingsPath -Encoding ASCII
# The runner parses the scale with atof, so always write a '.' decimal point.
$env:RECOMP_D3D_SCALE = ($height / 480).ToString([Globalization.CultureInfo]::InvariantCulture)
$env:RECOMP_D3D_MSAA = "$count"
$env:RECOMP_D3D_SMAA = if ($smaa.Checked) { '1' } else { '0' }
$env:RECOMP_D3D_SUN_SHADOW = if ($sun.Checked) { '1' } else { '0' }
$env:RECOMP_AUDIO_GAIN = ($percent / 100).ToString([Globalization.CultureInfo]::InvariantCulture)
$env:RECOMP_MUSIC_SHUFFLE = if ($shuffle.Checked) { '1' } else { '0' }
$env:RECOMP_TITLE_IDLE = $idleValues[$idle.SelectedIndex]
# Unset leaves the game's own one-second freeze; see docs/title-idle.md.
$env:RECOMP_DEMO_CUT_HOLD = if ($demoCut.Checked) { '12' } else { $null }
$env:RECOMP_UNLOCK_SWIMSUITS = if ($unlock.Checked) { '1' } else { '0' }
Start-Process -FilePath (Join-Path $root 'RunGame.cmd') -WorkingDirectory $root
