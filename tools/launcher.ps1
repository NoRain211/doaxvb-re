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
# What the game does when its title screen is left idle; see docs/title-idle.md.
$idleValues = 'default', 'no-cutscene', 'cpu-exhibition'
$idleNames = [string[]]('Default', 'No cutscene', 'CPU Exhibition')
$settings = [pscustomobject]@{
    height = [Windows.Forms.Screen]::PrimaryScreen.Bounds.Height; msaa = 1; smaa = $false; volume = 100; shuffle = $false; title_idle = 'default'; demo_cut_hold = $false
    sun_shadows = $false; unlock_swimsuits = $false
    vrr = $false; fog = $true; textures = $false; textureFolder = (Join-Path $root 'private\textures\replace'); textureDump = $false
}
try {
    $saved = Get-Content $settingsPath -Raw -Encoding UTF8 -ErrorAction Stop | ConvertFrom-Json
    # Older settings files omit new options; keep their defaults.
    foreach ($property in $settings.PSObject.Properties) {
        if ($null -ne $saved.($property.Name)) { $property.Value = $saved.($property.Name) }
    }
} catch {}

# The window copies the game's Zack Island menus: glass plates, outlined italic text, A/B prompts.
function C($hex, $alpha = 255) { [Drawing.Color]::FromArgb($alpha, [Drawing.ColorTranslator]::FromHtml($hex)) }
function RF($x, $y, $w, $h) { New-Object Drawing.RectangleF ([single]$x), ([single]$y), ([single]$w), ([single]$h) }
$field = C '#0d4f63'; $ink = C '#ffffff'; $soft = C '#d9f6f7'
$dot = " $([char]0xB7) "
$W = 864; $H = 576
# Game textures stay private: setup or the player copies them into private\launcher.
$art = Join-Path $root 'private\launcher'
function Art($name) { $path = Join-Path $art $name; if (Test-Path $path) { $path } }

function New-Round($x, $y, $w, $h, $r) {
    $path = New-Object Drawing.Drawing2D.GraphicsPath
    $d = 2 * $r
    $path.AddArc((RF $x $y $d $d), 180, 90); $path.AddArc((RF ($x + $w - $d) $y $d $d), 270, 90)
    $path.AddArc((RF ($x + $w - $d) ($y + $h - $d) $d $d), 0, 90); $path.AddArc((RF $x ($y + $h - $d) $d $d), 90, 90)
    $path.CloseFigure(); $path
}
function Draw-Plate($g, $x, $y, $w, $h, $top, $bottom, $alpha = 235) {
    $g.SmoothingMode = 'AntiAlias'
    $r = [Math]::Min(12, $h / 2)
    $shape = New-Round $x $y $w $h $r
    $fill = New-Object Drawing.Drawing2D.LinearGradientBrush((RF $x $y $w $h), (C $top $alpha), (C $bottom $alpha), [single]90)
    $gloss = New-Round ($x + 3) ($y + 2) ($w - 6) ($h * 0.45) ([Math]::Max(1, $r - 3))
    $shine = New-Object Drawing.SolidBrush ([Drawing.Color]::FromArgb(70, 255, 255, 255))
    $edge = New-Object Drawing.Pen ([Drawing.Color]::FromArgb(210, 255, 255, 255)), ([single]1.5)
    $g.FillPath($fill, $shape); $g.FillPath($shine, $gloss); $g.DrawPath($edge, $shape)
    $shape.Dispose(); $gloss.Dispose(); $fill.Dispose(); $shine.Dispose(); $edge.Dispose()
}
function Draw-Outlined($g, $text, $size, $x, $y, $w, $h, $line = '#1d4fb8', $align = 'Near') {
    $g.SmoothingMode = 'AntiAlias'
    $path = New-Object Drawing.Drawing2D.GraphicsPath
    $format = New-Object Drawing.StringFormat
    $format.Alignment = $align; $format.LineAlignment = 'Center'
    $path.AddString($text, (New-Object Drawing.FontFamily 'Arial'), [int][Drawing.FontStyle]'Bold, Italic', [single]$size, (RF $x $y $w $h), $format)
    $pen = New-Object Drawing.Pen (C $line), ([single][Math]::Max(2, $size / 5))
    $pen.LineJoin = 'Round'
    $g.DrawPath($pen, $path); $g.FillPath([Drawing.Brushes]::White, $path)
    $path.Dispose(); $format.Dispose(); $pen.Dispose()
}
function Draw-Fit($g, $path, $x, $y, $width, $height, [switch]$cover) {
    $image = [Drawing.Image]::FromFile($path)
    $scale = if ($cover) { [Math]::Max($width / $image.Width, $height / $image.Height) } else { [Math]::Min($width / $image.Width, $height / $image.Height) }
    $w = $image.Width * $scale; $h = $image.Height * $scale
    $left = if ($cover) { $x + ($width - $w) / 2 } else { $x }
    $g.DrawImage($image, (RF $left ($y + ($height - $h) / 2) $w $h))
    $image.Dispose()
}
function Draw-Icon($g, $name, $letter, $color, $x, $y, $size) {
    if ($path = Art $name) { Draw-Fit $g $path $x $y $size $size; return }
    $brush = New-Object Drawing.SolidBrush (C $color)
    $g.SmoothingMode = 'AntiAlias'; $g.FillEllipse($brush, (RF $x $y $size $size)); $brush.Dispose()
    Draw-Outlined $g $letter ($size * 0.6) $x $y $size $size '#203040' 'Center'
}

$form = New-Object Windows.Forms.Form -Property @{
    Text = 'DOAXBV'; FormBorderStyle = 'FixedSingle'; MaximizeBox = $false; StartPosition = 'CenterScreen'; KeyPreview = $true
    ClientSize = (New-Object Drawing.Size $W, $H); BackColor = (C '#1a8fc0'); ForeColor = $ink; Font = (New-Object Drawing.Font('Segoe UI', 9.75))
}
[Windows.Forms.Control].GetProperty('DoubleBuffered', [Reflection.BindingFlags]'Instance, NonPublic').SetValue($form, $true, $null)
$icon = Join-Path $root 'private\doaxbv.ico'
if (Test-Path $icon) { $form.Icon = New-Object Drawing.Icon $icon }

$backdrop = New-Object Drawing.Bitmap $W, $H
$g = [Drawing.Graphics]::FromImage($backdrop)
$g.InterpolationMode = 'HighQualityBicubic'
if ($artFile = Art 'background.png') {
    Draw-Fit $g $artFile 0 0 $W $H -cover
} else {
    $sea = New-Object Drawing.Drawing2D.LinearGradientBrush((RF 0 0 $W $H), (C '#7fd8ff'), (C '#178ab8'), [single]90)
    $g.FillRectangle($sea, 0, 0, $W, $H); $sea.Dispose()
}
# The map art carries the game's own island sign, so the logo only fills in without it.
if (Art 'background.png') {
} elseif ($artFile = Art 'logo.png') {
    Draw-Fit $g $artFile 14 4 300 140
} else {
    Draw-Outlined $g 'DEAD OR ALIVE' 34 20 18 330 50
    Draw-Outlined $g 'Xtreme Beach Volleyball' 18 24 64 330 30
}
# The settings frame matches the location preview frame on the island map.
Draw-Plate $g 346 110 504 402 '#d4fffa' '#53bcc8' 235
$inner = New-Round 358 122 480 378 8
$fill = New-Object Drawing.SolidBrush (C '#073e52' 225)
$g.FillPath($fill, $inner); $fill.Dispose(); $inner.Dispose()
Draw-Icon $g 'button-a.png' 'A' '#3fc43f' ($W - 302) ($H - 54) 34
Draw-Outlined $g 'Play' 17 ($W - 266) ($H - 52) 70 30
Draw-Icon $g 'button-b.png' 'B' '#e03a3a' ($W - 192) ($H - 54) 34
Draw-Outlined $g 'Quit' 17 ($W - 156) ($H - 52) 70 30
Draw-Outlined $g "$([char]0x2191)$([char]0x2193) Select" 15 ($W - 420) ($H - 52) 110 30
Draw-Outlined $g 'Settings apply on next launch' 13 24 ($H - 52) 300 30 '#0b3a66'
$g.Dispose()
$form.BackgroundImage = $backdrop

$title = New-Object Windows.Forms.Label -Property @{ Left = 372; Top = 54; Width = 452; Height = 46; BackColor = [Drawing.Color]::Transparent }
$title.Add_Paint({
    param($s, $e)
    Draw-Plate $e.Graphics 1 1 ($s.Width - 2) ($s.Height - 2) '#9af5c8' '#17886a' 245
    Draw-Outlined $e.Graphics $s.Text 24 0 0 $s.Width $s.Height '#1d4fb8' 'Center'
})
$card = New-Object Windows.Forms.Panel -Property @{ Left = 374; Top = 138; Width = 448; Height = 350; BackColor = [Drawing.Color]::Transparent }
$subtitle = New-Object Windows.Forms.Label -Property @{ Left = 0; Top = 0; Width = 448; Height = 22; ForeColor = $soft; BackColor = [Drawing.Color]::Transparent }
$card.Controls.Add($subtitle)

$sections = 'Main Menu', 'Graphics', 'Audio', 'Textures', 'Help'
$subtitles = 'Your next visit to Zack Island', 'Display and image quality', 'Volume and custom soundtrack', 'Replacement packs and dumps', 'Keys while playing'
$pages = foreach ($name in $sections) {
    $page = New-Object Windows.Forms.Panel -Property @{ Left = 0; Top = 32; Width = 448; Height = 244; BackColor = [Drawing.Color]::Transparent; Visible = $false }
    $card.Controls.Add($page); $page
}
function Add-Row($page, $text, $top, $control) {
    $label = New-Object Windows.Forms.Label -Property @{ Text = $text; Left = 0; Top = $top + 4; Width = 150; ForeColor = $soft; BackColor = [Drawing.Color]::Transparent }
    $control.Left = 156; $control.Top = $top; $control.Width = 290
    $page.Controls.AddRange(@($label, $control)); $control
}
function New-Choice($items, $index) {
    $box = New-Object Windows.Forms.ComboBox -Property @{ DropDownStyle = 'DropDownList'; FlatStyle = 'Flat'; BackColor = $field; ForeColor = $ink }
    $box.Items.AddRange($items)
    $box.SelectedIndex = [Math]::Max(0, $index)
    $box
}
function New-Toggle($page, $text, $top, $checked) {
    $box = New-Object Windows.Forms.CheckBox -Property @{ Text = $text; Left = 0; Top = $top; Width = 446; ForeColor = $ink; BackColor = [Drawing.Color]::Transparent; Checked = [bool]$checked }
    $page.Controls.Add($box); $box
}
function New-Note($page, $text, $top, $height) {
    $note = New-Object Windows.Forms.Label -Property @{ Text = $text; Left = 0; Top = $top; Width = 446; Height = $height; ForeColor = $soft; BackColor = [Drawing.Color]::Transparent }
    $page.Controls.Add($note); $note
}
function New-Button($text, $width) {
    $button = New-Object Windows.Forms.Button -Property @{ Text = $text; Width = $width; Height = 28; FlatStyle = 'Flat'; BackColor = $field; ForeColor = $ink }
    $button.FlatAppearance.BorderColor = $soft
    $button
}

$ready = New-Object Windows.Forms.Label -Property @{ Text = 'Ready to play'; Left = 0; Top = 0; Width = 446; Height = 28; Font = (New-Object Drawing.Font('Arial', 14, [Drawing.FontStyle]'Bold, Italic')); ForeColor = (C '#9af5c8'); BackColor = [Drawing.Color]::Transparent }
$summaryKeys = New-Note $pages[0] "Display`nRendering`nExtras" 44 80
$summaryKeys.Width = 110
$summary = New-Object Windows.Forms.Label -Property @{ Left = 120; Top = 44; Width = 326; Height = 80; ForeColor = $ink; BackColor = [Drawing.Color]::Transparent }
$pages[0].Controls.AddRange(@($ready, $summary))
$idle = Add-Row $pages[0] 'Title screen idle' 140 (New-Choice $idleNames ([Array]::IndexOf($idleValues, [string]$settings.title_idle)))
$demoCut = New-Toggle $pages[0] 'Shorter demo cuts' 176 $settings.demo_cut_hold
$unlock = New-Toggle $pages[0] 'Unlock swimsuits' 204 $settings.unlock_swimsuits

$heightNames = [string[]]($heights | ForEach-Object { if ($_ -eq 480) { '480p (windowed)' } else { "$($_)p" } })
$nearest = @($heights | Where-Object { $_ -le [int]$settings.height }).Count - 1
$resolution = Add-Row $pages[1] 'Resolution' 0 (New-Choice $heightNames $nearest)
$msaaNames = [string[]]($samples | ForEach-Object { if ($_ -eq 1) { 'Off' } else { "$($_)x" } })
$msaa = Add-Row $pages[1] 'MSAA' 36 (New-Choice $msaaNames ([Array]::IndexOf($samples, [int]$settings.msaa)))
$smaa = New-Toggle $pages[1] 'SMAA edge smoothing' 76 $settings.smaa
$vrr = New-Toggle $pages[1] 'VRR display (even 60 Hz pacing)' 104 $settings.vrr
$fog = New-Toggle $pages[1] 'Fog' 132 $settings.fog
# Off keeps the game's original shadows for lower-powered PCs; see docs/sun-shadow.md.
$sun = New-Toggle $pages[1] 'Sun shadows' 160 $settings.sun_shadows

# PowerShell 7 loads JSON numbers as Int64, so cast before finding their index.
$savedVolume = [Array]::IndexOf($volumes, [int]$settings.volume)
$volumeNames = [string[]]($volumes | ForEach-Object { if ($_ -eq 0) { 'Mute' } else { "$_%" } })
$volume = Add-Row $pages[2] 'Volume' 0 (New-Choice $volumeNames $savedVolume)
$shuffle = New-Toggle $pages[2] 'Shuffle music' 40 $settings.shuffle
$music = New-Button 'Open music folder' 160
$music.Top = 76
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
$pages[2].Controls.Add($music)
New-Note $pages[2] 'Songs in this folder play at the Radio Station.' 114 40 | Out-Null

$textures = New-Toggle $pages[3] 'Use texture pack' 0 $settings.textures
$textureFolder = New-Object Windows.Forms.TextBox -Property @{ Left = 0; Top = 32; Width = 350; ReadOnly = $true; BorderStyle = 'FixedSingle'; BackColor = $field; ForeColor = $ink; Text = [string]$settings.textureFolder }
$browse = New-Button 'Folder...' 88
$browse.Left = 358; $browse.Top = 30
$browse.Add_Click({
    $dialog = New-Object Windows.Forms.FolderBrowserDialog
    $dialog.Description = 'Choose a texture replacement pack'
    $dialog.SelectedPath = $textureFolder.Text
    if ($dialog.ShowDialog($form) -eq 'OK') { $textureFolder.Text = $dialog.SelectedPath }
    $dialog.Dispose()
})
$pages[3].Controls.AddRange(@($textureFolder, $browse))
$textureDump = New-Toggle $pages[3] 'Dump original textures' 68 $settings.textureDump
New-Note $pages[3] 'F12 dumps the textures in view. Ctrl+F12 reloads the pack.' 100 40 | Out-Null

New-Note $pages[4] "Textures`n    F12    Dump textures in view`n    Ctrl+F12    Reload texture pack" 0 240 | Out-Null

function Update-Summary {
    $display = "$($resolution.SelectedItem)$dot" + $(if ($msaa.SelectedIndex) { "$($msaa.SelectedItem) MSAA" } else { 'No MSAA' }) + $(if ($smaa.Checked) { "$($dot)SMAA" })
    $render = 'Direct3D 11' + $dot + $(if ($fog.Checked) { 'Fog on' } else { 'Fog off' }) + $(if ($vrr.Checked) { "$($dot)VRR" }) + $(if ($sun.Checked) { "$($dot)Sun shadows" })
    $extras = @($(if ($textures.Checked) { 'Texture pack' }), $(if ($textureDump.Checked) { 'Texture dump' }), $(if ($unlock.Checked) { 'Swimsuits unlocked' })) | Where-Object { $_ }
    $summary.Text = "$display`n$render`n" + $(if ($extras) { $extras -join $dot } else { 'None' })
}

$state = @{ section = 0 }
$menu = for ($i = 0; $i -lt $sections.Count; $i++) {
    $item = New-Object Windows.Forms.Label -Property @{ Left = 24; Top = 214 + $i * 50; Width = 270; Height = 42; BackColor = [Drawing.Color]::Transparent; Cursor = 'Hand'; Tag = $i; AccessibleName = $sections[$i]; AccessibleRole = 'MenuItem' }
    $item.Add_Paint({
        param($s, $e)
        if ($state.section -eq $s.Tag) {
            Draw-Plate $e.Graphics 1 1 ($s.Width - 2) ($s.Height - 2) '#9af5c8' '#17886a' 245
        } else {
            Draw-Plate $e.Graphics 1 4 ($s.Width - 30) ($s.Height - 8) '#c4f6fa' '#3a9ec0' 150
        }
        Draw-Outlined $e.Graphics $sections[$s.Tag].ToUpper() 20 16 0 ($s.Width - 16) $s.Height
    })
    $item.Add_Click({ param($s) Select-Section $s.Tag })
    $item
}
function Select-Section($index) {
    $state.section = $index
    for ($i = 0; $i -lt $pages.Count; $i++) { $pages[$i].Visible = $i -eq $index; $menu[$i].Invalidate() }
    $title.Text = $sections[$index].ToUpper(); $title.Invalidate()
    $subtitle.Text = $subtitles[$index]
    if ($index -eq 0) { Update-Summary }
}
$form.Add_KeyDown({
    param($s, $e)
    # Lists and number boxes use the arrows themselves.
    if ($form.ActiveControl -is [Windows.Forms.ComboBox] -or $form.ActiveControl -is [Windows.Forms.NumericUpDown]) { return }
    $step = @{ Up = -1; Down = 1 }[[string]$e.KeyCode]
    if ($step) { Select-Section (($state.section + $step + $sections.Count) % $sections.Count); $e.Handled = $true }
    if ($e.KeyCode -eq 'Escape') { $form.Close() }
})

$play = New-Object Windows.Forms.Button -Property @{ Text = 'PLAY'; Left = 232; Top = 286; Width = 214; Height = 50; FlatStyle = 'Flat'; BackColor = [Drawing.Color]::Transparent; DialogResult = 'OK'; Cursor = 'Hand' }
$play.FlatAppearance.BorderSize = 0
$play.FlatAppearance.MouseOverBackColor = [Drawing.Color]::Transparent
$play.FlatAppearance.MouseDownBackColor = [Drawing.Color]::Transparent
$play.Add_Paint({
    param($s, $e)
    $e.Graphics.Clear((C '#073e52'))
    Draw-Plate $e.Graphics 1 1 ($s.Width - 2) ($s.Height - 2) '#9af5c8' '#17886a' 255
    Draw-Icon $e.Graphics 'button-a.png' 'A' '#3fc43f' 14 8 34
    Draw-Outlined $e.Graphics 'PLAY' 24 56 0 ($s.Width - 70) $s.Height '#1d4fb8' 'Center'
})
$card.Controls.Add($play)
$form.Controls.AddRange(@($menu) + @($title, $card))
Select-Section 0
$form.AcceptButton = $play
$form.ActiveControl = $play
if ($form.ShowDialog() -ne 'OK') { exit }

$height = $heights[$resolution.SelectedIndex]
$count = $samples[$msaa.SelectedIndex]
$percent = $volumes[$volume.SelectedIndex]
[pscustomobject]@{
    height = $height; msaa = $count; smaa = $smaa.Checked; volume = $percent; shuffle = $shuffle.Checked; title_idle = $idleValues[$idle.SelectedIndex]; demo_cut_hold = $demoCut.Checked
    vrr = $vrr.Checked; fog = $fog.Checked; textures = $textures.Checked; textureFolder = $textureFolder.Text; textureDump = $textureDump.Checked
    sun_shadows = $sun.Checked; unlock_swimsuits = $unlock.Checked
} | ConvertTo-Json | Set-Content $settingsPath -Encoding UTF8
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
$env:RECOMP_D3D_VRR = if ($vrr.Checked) { '1' } else { '0' }
$env:RECOMP_D3D_FOG = if ($fog.Checked) { '1' } else { '0' }
$env:RECOMP_TEXTURES = if ($textures.Checked) { $textureFolder.Text } else { $null }
$env:RECOMP_TEXTURE_DUMP = if ($textureDump.Checked) { '1' } else { '0' }
$env:RECOMP_TEXTURE_DUMP_DIR = Join-Path $root 'private\textures'
Start-Process -FilePath (Join-Path $root 'RunGame.cmd') -WorkingDirectory $root
