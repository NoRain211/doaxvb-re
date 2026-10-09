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

# The window follows the game's island menus: art backdrop, italic section list, navy card.
function C($hex) { [Drawing.ColorTranslator]::FromHtml($hex) }
$navy = C '#08394f'; $field = C '#114a63'; $lime = C '#b5de39'; $ink = C '#f4fbff'; $soft = C '#d5e8ed'; $edge = C '#70a6b4'; $deep = C '#062c40'
$dot = " $([char]0xB7) "
$menuFont = New-Object Drawing.Font('Arial', 17, [Drawing.FontStyle]'Bold, Italic')
$headFont = New-Object Drawing.Font('Arial', 19, [Drawing.FontStyle]'Bold, Italic')
$accentFont = New-Object Drawing.Font('Arial', 13, [Drawing.FontStyle]'Bold, Italic')
$W = 880; $H = 560

$form = New-Object Windows.Forms.Form -Property @{
    Text = 'DOAXBV'; FormBorderStyle = 'FixedSingle'; MaximizeBox = $false; StartPosition = 'CenterScreen'; KeyPreview = $true
    ClientSize = (New-Object Drawing.Size $W, $H); BackColor = $deep; ForeColor = $ink; Font = (New-Object Drawing.Font('Segoe UI', 9.75))
}
[Windows.Forms.Control].GetProperty('DoubleBuffered', [Reflection.BindingFlags]'Instance, NonPublic').SetValue($form, $true, $null)
$icon = Join-Path $root 'private\doaxbv.ico'
if (Test-Path $icon) { $form.Icon = New-Object Drawing.Icon $icon }

function Draw-Text($g, $text, $font, $color, $x, $y) {
    $shadow = New-Object Drawing.SolidBrush (C '#06384a')
    $brush = New-Object Drawing.SolidBrush $color
    $g.DrawString($text, $font, $shadow, [single]($x + 1), [single]($y + 2))
    $g.DrawString($text, $font, $shadow, [single]($x - 1), [single]($y - 1))
    $g.DrawString($text, $font, $brush, [single]$x, [single]$y)
    $shadow.Dispose(); $brush.Dispose()
}
function Draw-Fit($g, $path, $x, $y, $width, $height, [switch]$cover) {
    $image = [Drawing.Image]::FromFile($path)
    $scale = if ($cover) { [Math]::Max($width / $image.Width, $height / $image.Height) } else { [Math]::Min($width / $image.Width, $height / $image.Height) }
    $w = $image.Width * $scale; $h = $image.Height * $scale
    $left = if ($cover) { $x + ($width - $w) / 2 } else { $x }
    $g.DrawImage($image, [single]$left, [single]($y + ($height - $h) / 2), [single]$w, [single]$h)
    $image.Dispose()
}

# Retail art stays private: use private\launcher\background.jpg and logo.png when present.
$art = Join-Path $root 'private\launcher'
$backdrop = New-Object Drawing.Bitmap $W, $H
$g = [Drawing.Graphics]::FromImage($backdrop)
$g.InterpolationMode = 'HighQualityBicubic'; $g.TextRenderingHint = 'AntiAliasGridFit'
if (Test-Path (Join-Path $art 'background.jpg')) {
    Draw-Fit $g (Join-Path $art 'background.jpg') 0 0 $W $H -cover
} else {
    $sky = New-Object Drawing.Drawing2D.LinearGradientBrush((New-Object Drawing.Point 0, 0), (New-Object Drawing.Point 0, $H), (C '#2b7aa8'), (C '#f0b46a'))
    $g.FillRectangle($sky, 0, 0, $W, $H); $sky.Dispose()
}
$shade = New-Object Drawing.Drawing2D.LinearGradientBrush((New-Object Drawing.Point 0, 0), (New-Object Drawing.Point 340, 0), ([Drawing.Color]::FromArgb(140, 6, 44, 64)), ([Drawing.Color]::FromArgb(0, 6, 44, 64)))
$g.FillRectangle($shade, 0, 0, 340, $H); $shade.Dispose()
if (Test-Path (Join-Path $art 'logo.png')) {
    Draw-Fit $g (Join-Path $art 'logo.png') 24 12 320 104
} else {
    Draw-Text $g 'DEAD OR ALIVE' (New-Object Drawing.Font('Arial', 30, [Drawing.FontStyle]'Bold, Italic')) $ink 24 24
    Draw-Text $g 'XTREME BEACH VOLLEYBALL' $accentFont (C '#9fd4ea') 30 72
}
Draw-Text $g 'NATIVE PC PORT' (New-Object Drawing.Font('Segoe UI', 8, [Drawing.FontStyle]'Bold')) $soft 34 116
$bar = New-Object Drawing.SolidBrush ([Drawing.Color]::FromArgb(235, 6, 44, 64))
$g.FillRectangle($bar, 0, $H - 34, $W, 34); $bar.Dispose()
$cardShadow = New-Object Drawing.SolidBrush (C '#032b3d')
$g.FillRectangle($cardShadow, 515, 115, 344, 392); $cardShadow.Dispose()
$prompts = "$([char]0x2191)$([char]0x2193) Select     Enter  Play     Esc  Quit"
Draw-Text $g 'Settings apply on next launch' $form.Font $soft 24 ($H - 26)
Draw-Text $g $prompts $form.Font $ink ($W - 24 - $g.MeasureString($prompts, $form.Font).Width) ($H - 26)
$g.Dispose()
$form.BackgroundImage = $backdrop

$card = New-Object Windows.Forms.Panel -Property @{ Left = 512; Top = 112; Width = 344; Height = 392; BackColor = $navy }
$card.Add_Paint({ param($s, $e) $pen = New-Object Drawing.Pen $edge; $e.Graphics.DrawRectangle($pen, 0, 0, $s.Width - 1, $s.Height - 1); $pen.Dispose() })
$title = New-Object Windows.Forms.Label -Property @{ Left = 18; Top = 14; Width = 310; Height = 34; Font = $headFont; ForeColor = $ink }
$subtitle = New-Object Windows.Forms.Label -Property @{ Left = 20; Top = 48; Width = 310; ForeColor = $soft }
$card.Controls.AddRange(@($title, $subtitle))

$sections = 'Main Menu', 'Graphics', 'Audio', 'Textures', 'Experimental', 'Help'
$subtitles = 'Your next visit to Zack Island', 'Display and image quality', 'Volume and custom soundtrack', 'Replacement packs and dumps', 'Unfinished renderer features', 'Keys while playing'
$pages = foreach ($name in $sections) {
    $page = New-Object Windows.Forms.Panel -Property @{ Left = 20; Top = 82; Width = 306; Height = 236; BackColor = $navy; Visible = $false }
    $card.Controls.Add($page); $page
}
function Add-Row($page, $text, $top, $control) {
    $label = New-Object Windows.Forms.Label -Property @{ Text = $text; Left = 0; Top = $top + 4; Width = 116; ForeColor = $soft }
    $control.Left = 120; $control.Top = $top; $control.Width = 184
    $page.Controls.AddRange(@($label, $control)); $control
}
function New-Choice($items, $index) {
    $box = New-Object Windows.Forms.ComboBox -Property @{ DropDownStyle = 'DropDownList'; FlatStyle = 'Flat'; BackColor = $field; ForeColor = $ink }
    $box.Items.AddRange($items)
    $box.SelectedIndex = [Math]::Max(0, $index)
    $box
}
function New-Toggle($page, $text, $top, $checked) {
    $box = New-Object Windows.Forms.CheckBox -Property @{ Text = $text; Left = 0; Top = $top; Width = 304; FlatStyle = 'Flat'; ForeColor = $ink; Checked = [bool]$checked }
    $page.Controls.Add($box); $box
}
function New-Note($page, $text, $top, $height) {
    $note = New-Object Windows.Forms.Label -Property @{ Text = $text; Left = 0; Top = $top; Width = 304; Height = $height; ForeColor = $soft }
    $page.Controls.Add($note); $note
}
function New-Button($text, $width) {
    $button = New-Object Windows.Forms.Button -Property @{ Text = $text; Width = $width; Height = 28; FlatStyle = 'Flat'; BackColor = $field; ForeColor = $ink }
    $button.FlatAppearance.BorderColor = $edge
    $button
}

$ready = New-Object Windows.Forms.Label -Property @{ Text = 'Ready to play'; Left = 0; Top = 0; Width = 304; Height = 26; Font = $accentFont; ForeColor = $lime }
$summaryKeys = New-Note $pages[0] "Display`nRendering`nExtras" 44 80
$summaryKeys.Width = 96
$summary = New-Object Windows.Forms.Label -Property @{ Left = 100; Top = 44; Width = 204; Height = 80; ForeColor = $ink }
$pages[0].Controls.AddRange(@($ready, $summary))

$heightNames = [string[]]($heights | ForEach-Object { if ($_ -eq 480) { '480p (windowed)' } else { "$($_)p" } })
$nearest = @($heights | Where-Object { $_ -le [int]$settings.height }).Count - 1
$resolution = Add-Row $pages[1] 'Resolution' 0 (New-Choice $heightNames $nearest)
$msaaNames = [string[]]($samples | ForEach-Object { if ($_ -eq 1) { 'Off' } else { "$($_)x" } })
$msaa = Add-Row $pages[1] 'MSAA' 36 (New-Choice $msaaNames ([Array]::IndexOf($samples, [int]$settings.msaa)))
$smaa = New-Toggle $pages[1] 'SMAA edge smoothing' 76 $settings.smaa
$vrr = New-Toggle $pages[1] 'VRR display (even 60 Hz pacing)' 104 $settings.vrr
$fog = New-Toggle $pages[1] 'Fog' 132 $settings.fog

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
$textureFolder = New-Object Windows.Forms.TextBox -Property @{ Left = 0; Top = 32; Width = 214; ReadOnly = $true; BorderStyle = 'FixedSingle'; BackColor = $field; ForeColor = $ink; Text = [string]$settings.textureFolder }
$browse = New-Button 'Folder...' 84
$browse.Left = 220; $browse.Top = 30
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

$pixelLighting = New-Toggle $pages[4] 'Per-pixel lighting' 0 $settings.pixelLighting
$smoothNormals = New-Toggle $pages[4] 'Smooth normals' 28 $settings.smoothNormals
$rtgi = New-Toggle $pages[4] 'Ray-traced GI (RTGI)' 56 $settings.rtgi
$strength = Add-Row $pages[4] 'RTGI strength' 88 (New-Object Windows.Forms.NumericUpDown -Property @{ Minimum = 0; Maximum = 4; DecimalPlaces = 2; Increment = 0.25; BorderStyle = 'FixedSingle'; BackColor = $field; ForeColor = $ink; Value = [Math]::Max([decimal]0, [Math]::Min([decimal]4, [decimal]$settings.rtgiStrength)); Enabled = $rtgi.Checked })
$hitColor = New-Toggle $pages[4] 'RTGI hit-point color' 120 $settings.rtgiHitColor
$hitColor.Enabled = $rtgi.Checked
$rtgi.Add_CheckedChanged({ $strength.Enabled = $rtgi.Checked; $hitColor.Enabled = $rtgi.Checked })
New-Note $pages[4] 'RTGI needs DXR and can slow matches. Direct3D 11 is the fallback.' 154 40 | Out-Null

New-Note $pages[5] "Textures`n  F12  Dump textures in view`n  Ctrl+F12  Reload texture pack`n`nRTGI`n  F5 / F6  Strength down / up`n  F7  Split comparison`n  F8  Toggle GI`n  F9  Pause or resume`n  F10  Step one frame" 0 230 | Out-Null

function Update-Summary {
    $display = "$($resolution.SelectedItem)$dot" + $(if ($msaa.SelectedIndex) { "$($msaa.SelectedItem) MSAA" } else { 'No MSAA' }) + $(if ($smaa.Checked) { "$($dot)SMAA" })
    $render = 'Direct3D 11' + $(if ($rtgi.Checked) { ' / DXR' }) + $dot + $(if ($fog.Checked) { 'Fog on' } else { 'Fog off' }) + $(if ($vrr.Checked) { "$($dot)VRR" })
    $extras = @($(if ($textures.Checked) { 'Texture pack' }), $(if ($pixelLighting.Checked) { 'Per-pixel' }), $(if ($rtgi.Checked) { 'RTGI' })) | Where-Object { $_ }
    $summary.Text = "$display`n$render`n" + $(if ($extras) { $extras -join $dot } else { 'None' })
}

$state = @{ section = 0 }
$menu = for ($i = 0; $i -lt $sections.Count; $i++) {
    $item = New-Object Windows.Forms.Label -Property @{ Left = 20; Top = 150 + $i * 50; Width = 230; Height = 42; BackColor = [Drawing.Color]::Transparent; Cursor = 'Hand'; Tag = $i; AccessibleName = $sections[$i]; AccessibleRole = 'MenuItem' }
    $item.Add_Paint({
        param($s, $e)
        $e.Graphics.TextRenderingHint = 'AntiAliasGridFit'
        $name = $sections[$s.Tag].ToUpper()
        if ($state.section -eq $s.Tag) {
            $fill = New-Object Drawing.SolidBrush $lime
            $text = New-Object Drawing.SolidBrush (C '#17391c')
            $e.Graphics.FillRectangle($fill, 4, 0, $s.Width - 4, $s.Height)
            $e.Graphics.DrawString($name, $menuFont, $text, [single]14, [single]9)
            $fill.Dispose(); $text.Dispose()
        } else {
            Draw-Text $e.Graphics $name $menuFont $ink 10 9
        }
    })
    $item.Add_Click({ param($s) Select-Section $s.Tag })
    $item
}
function Select-Section($index) {
    $state.section = $index
    for ($i = 0; $i -lt $pages.Count; $i++) { $pages[$i].Visible = $i -eq $index; $menu[$i].Invalidate() }
    $title.Text = $sections[$index].ToUpper()
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

$play = New-Object Windows.Forms.Button -Property @{ Text = 'PLAY GAME'; Left = 20; Top = 330; Width = 200; Height = 44; FlatStyle = 'Flat'; BackColor = $lime; ForeColor = (C '#172c15'); Font = $accentFont; DialogResult = 'OK' }
$play.FlatAppearance.BorderSize = 0
$play.FlatAppearance.MouseOverBackColor = C '#c8ec55'
$card.Controls.Add($play)
$form.Controls.AddRange(@($menu) + $card)
Select-Section 0
$form.AcceptButton = $play
$form.ActiveControl = $play
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
