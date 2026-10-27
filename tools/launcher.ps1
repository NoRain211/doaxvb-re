# SPDX-License-Identifier: GPL-3.0-or-later
# Build the game from an ISO with BuildGame.cmd, remember graphics and audio settings, then start RunGame.cmd.
# -PlaySaved (DOAXBV.exe) starts the game with the saved settings without showing the window.
param([switch]$PlaySaved)
Add-Type -AssemblyName System.Windows.Forms
# Panels and their controls each own a window, so repaint the whole window tree
# in one call; the screen then shows one completed section instead of strips.
Add-Type -Namespace DOAXBV -Name Native -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool RedrawWindow(IntPtr window, IntPtr rect, IntPtr region, uint flags);
[DllImport("user32.dll")] public static extern int GetWindowLong(IntPtr window, int index);
[DllImport("user32.dll")] public static extern int SetWindowLong(IntPtr window, int index, int value);
'@
# Controllers read through the game's own SDL3.dll, so Xbox and PlayStation pads both work.
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace DOAXBV {
public static class Pad {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] static extern IntPtr LoadLibraryW(string path);
    [DllImport("SDL3.dll", CallingConvention = CallingConvention.Cdecl)] [return: MarshalAs(UnmanagedType.I1)] static extern bool SDL_SetHint(string name, string value);
    [DllImport("SDL3.dll", CallingConvention = CallingConvention.Cdecl)] [return: MarshalAs(UnmanagedType.I1)] static extern bool SDL_Init(uint flags);
    [DllImport("SDL3.dll", CallingConvention = CallingConvention.Cdecl)] static extern void SDL_UpdateGamepads();
    [DllImport("SDL3.dll", CallingConvention = CallingConvention.Cdecl)] static extern IntPtr SDL_GetGamepads(out int count);
    [DllImport("SDL3.dll", CallingConvention = CallingConvention.Cdecl)] static extern IntPtr SDL_GetGamepadFromID(uint id);
    [DllImport("SDL3.dll", CallingConvention = CallingConvention.Cdecl)] static extern IntPtr SDL_OpenGamepad(uint id);
    [DllImport("SDL3.dll", CallingConvention = CallingConvention.Cdecl)] [return: MarshalAs(UnmanagedType.I1)] static extern bool SDL_GetGamepadButton(IntPtr pad, int button);
    [DllImport("SDL3.dll", CallingConvention = CallingConvention.Cdecl)] static extern short SDL_GetGamepadAxis(IntPtr pad, int axis);
    [DllImport("SDL3.dll", CallingConvention = CallingConvention.Cdecl)] static extern void SDL_free(IntPtr memory);
    static readonly string[] names = { "up", "down", "left", "right", "a", "b", "start", "lb", "rb" };
    // SDL buttons for a, b, start, lb and rb: south, east, start, left and right shoulder.
    static readonly int[] buttons = { 0, 1, 6, 9, 10 };
    static bool ready;
    static int last, nextRepeat;
    public static bool Start(string dll) {
        if (LoadLibraryW(dll) == IntPtr.Zero) return false;
        SDL_SetHint("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1");
        return ready = SDL_Init(0x2000);  // SDL_INIT_GAMEPAD
    }
    public static string Poll() { return ready ? Step(Held(), Environment.TickCount) : null; }
    // Bits in names order, from every pad's d-pad, left stick and buttons.
    static int Held() {
        SDL_UpdateGamepads();
        int count, held = 0;
        IntPtr ids = SDL_GetGamepads(out count);
        for (int i = 0; i < count; i++) {
            uint id = (uint)Marshal.ReadInt32(ids, i * 4);
            IntPtr pad = SDL_GetGamepadFromID(id);
            if (pad == IntPtr.Zero) pad = SDL_OpenGamepad(id);
            if (pad == IntPtr.Zero) continue;
            int x = SDL_GetGamepadAxis(pad, 0), y = SDL_GetGamepadAxis(pad, 1);
            if (SDL_GetGamepadButton(pad, 11) || y < -20000) held |= 1;
            if (SDL_GetGamepadButton(pad, 12) || y > 20000) held |= 2;
            if (SDL_GetGamepadButton(pad, 13) || x < -20000) held |= 4;
            if (SDL_GetGamepadButton(pad, 14) || x > 20000) held |= 8;
            for (int b = 0; b < buttons.Length; b++) if (SDL_GetGamepadButton(pad, buttons[b])) held |= 16 << b;
        }
        SDL_free(ids);
        return held;
    }
    // One action per call: each new press in turn, else a held direction
    // repeating after 350 ms, then every 120 ms, like launcher_sdl.cpp.
    public static string Step(int held, int now) {
        last &= held;
        for (int i = 0; i < names.Length; i++) {
            int bit = 1 << i;
            if ((held & bit) != 0 && (last & bit) == 0) { last |= bit; nextRepeat = now + 350; return names[i]; }
        }
        for (int i = 0; i < 4; i++) {
            if ((held & (1 << i)) != 0 && now - nextRepeat >= 0) { nextRepeat = now + 120; return names[i]; }
        }
        return null;
    }
}
}
'@
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
    height = [Windows.Forms.Screen]::PrimaryScreen.Bounds.Height; msaa = 1; smaa = $false; volume = 100; shuffle = $false
    sun_shadows = $false; demo_cut_hold = $false; title_idle = 'default'; unlock_swimsuits = $false; free_camera = $false
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
# Setup cuts the icon from the player's disc; without it the window keeps the default.
if (Test-Path (Join-Path $root 'private\doaxbv.ico')) { $form.Icon = New-Object Drawing.Icon (Join-Path $root 'private\doaxbv.ico') }
$form.Add_Load({
    [DOAXBV.Native]::SetWindowLong($form.Handle, -20, [DOAXBV.Native]::GetWindowLong($form.Handle, -20) -bor 0x02000000) | Out-Null
})
[Windows.Forms.Control].GetProperty('DoubleBuffered', [Reflection.BindingFlags]'Instance, NonPublic').SetValue($form, $true, $null)
$icon = Join-Path $root 'private\doaxbv.ico'
if (Test-Path $icon) { $form.Icon = New-Object Drawing.Icon $icon }
$backdrop = New-Object Drawing.Bitmap $W, $H
$g = [Drawing.Graphics]::FromImage($backdrop)
$g.InterpolationMode = 'HighQualityBicubic'
# A player's own island map frame wins over the beach preview setup extracts.
$artFile = Art 'background.png'
if (-not $artFile) { $artFile = Art 'beach.png' }
if ($artFile) {
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
Draw-Outlined $g "$([char]0x2191)$([char]0x2193) Select" 15 ($W - 420) ($H - 52) 110 30
Draw-Outlined $g 'Settings apply on next launch' 13 24 ($H - 52) 300 30 '#0b3a66'
$g.Dispose()
$form.BackgroundImage = $backdrop
# Panels copy the backdrop under themselves instead of painting it through
# their parent, so a page appears in one blit without translucency bands.
function Crop($x, $y, $w, $h) {
    $bitmap = New-Object Drawing.Bitmap $w, $h
    $graphics = [Drawing.Graphics]::FromImage($bitmap)
    $graphics.DrawImage($backdrop, (New-Object Drawing.Rectangle 0, 0, $w, $h), (New-Object Drawing.Rectangle $x, $y, $w, $h), 'Pixel')
    $graphics.Dispose()
    $bitmap
}

$title = New-Object Windows.Forms.Label -Property @{ Left = 372; Top = 54; Width = 452; Height = 46; BackColor = [Drawing.Color]::Transparent }
$title.Add_Paint({
    param($s, $e)
    Draw-Plate $e.Graphics 1 1 ($s.Width - 2) ($s.Height - 2) '#9af5c8' '#17886a' 245
    Draw-Outlined $e.Graphics $s.Text 24 0 0 $s.Width $s.Height '#1d4fb8' 'Center'
})
$card = New-Object Windows.Forms.Panel -Property @{ Left = 374; Top = 138; Width = 448; Height = 350; BackColor = (C '#185162'); BackgroundImage = (Crop 374 138 448 350); BackgroundImageLayout = 'None' }
$subtitle = New-Object Windows.Forms.Label -Property @{ Left = 0; Top = 0; Width = 448; Height = 22; ForeColor = $soft; BackColor = [Drawing.Color]::Transparent }
$card.Controls.Add($subtitle)

$sections = 'Main Menu', 'Build', 'Graphics', 'Audio', 'Game', 'Textures', 'Help'
$subtitles = 'Your next visit to Zack Island', 'Build the game from your ISO', 'Display and image quality', 'Volume and custom soundtrack', 'Swimsuits, demo and camera', 'Replacement packs and dumps', 'Keys while playing'
$pages = foreach ($name in $sections) {
    $page = New-Object Windows.Forms.Panel -Property @{ Left = 0; Top = 32; Width = 448; Height = 244; BackColor = (C '#185162'); BackgroundImage = (Crop 374 170 448 244); BackgroundImageLayout = 'None'; Visible = $false }
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
function New-Toggle($page, $text, $top, $checked, $left = 0, $width = 446) {
    $box = New-Object Windows.Forms.CheckBox -Property @{ Text = $text; Left = $left; Top = $top; Width = $width; ForeColor = $ink; BackColor = [Drawing.Color]::Transparent; Checked = [bool]$checked }
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

$ready = New-Object Windows.Forms.Label -Property @{ Left = 0; Top = 0; Width = 446; Height = 28; Font = (New-Object Drawing.Font('Arial', 14, [Drawing.FontStyle]'Bold, Italic')); BackColor = [Drawing.Color]::Transparent }
# Same places RunGame.cmd looks; it still validates the build before launch.
function Test-Built {
    if (@('private\active-build.json', 'recomp_program_runner.exe', 'RunGame.exe', 'build\recomp-program\Release\recomp_program_runner.exe') |
        Where-Object { Test-Path (Join-Path $root $_) }) { return $true }
    # Builds from before active-build.json; failed attempts also leave receipts.
    [bool](Get-ChildItem (Join-Path $root 'private\setup-*\build-receipt.json') -ErrorAction SilentlyContinue |
        Where-Object { (Get-Content $_.FullName -Raw | ConvertFrom-Json).status -eq 'built-unverified' })
}
function Update-Ready {
    if (Test-Built) { $ready.Text = 'Ready to play'; $ready.ForeColor = C '#9af5c8' }
    else { $ready.Text = 'Build the game first (Build page)'; $ready.ForeColor = C '#ffd27a' }
}
$summaryKeys = New-Note $pages[0] "Display`nRendering`nExtras" 44 80
$summaryKeys.Width = 110
$summary = New-Object Windows.Forms.Label -Property @{ Left = 120; Top = 44; Width = 326; Height = 80; ForeColor = $ink; BackColor = [Drawing.Color]::Transparent }
$pages[0].Controls.AddRange(@($ready, $summary))

# Build: run BuildGame.cmd hidden and tail its output into the log box.
$iso = New-Object Windows.Forms.TextBox -Property @{ Left = 0; Top = 2; Width = 350; ReadOnly = $true; BorderStyle = 'FixedSingle'; BackColor = $field; ForeColor = $ink }
$pickIso = New-Button 'ISO...' 88
$pickIso.Left = 358; $pickIso.Top = 0
$pickIso.Add_Click({
    $dialog = New-Object Windows.Forms.OpenFileDialog -Property @{ Title = 'Choose your game ISO'; Filter = 'Xbox disc image (*.iso)|*.iso|All files (*.*)|*.*' }
    if ($dialog.ShowDialog($form) -eq 'OK') { $iso.Text = $dialog.FileName }
    $dialog.Dispose()
})
$buildNow = New-Button 'Build game' 160
$buildNow.Top = 36
$buildState = New-Object Windows.Forms.Label -Property @{ Left = 168; Top = 42; Width = 278; ForeColor = $soft; BackColor = [Drawing.Color]::Transparent; Text = 'Pick your ISO, then build once.' }
$buildLog = New-Object Windows.Forms.TextBox -Property @{ Left = 0; Top = 72; Width = 446; Height = 170; Multiline = $true; ReadOnly = $true; ScrollBars = 'Vertical'; WordWrap = $false; BorderStyle = 'FixedSingle'; BackColor = (C '#062f3e'); ForeColor = $soft; Font = (New-Object Drawing.Font('Consolas', 8)) }
$pages[1].Controls.AddRange(@($iso, $pickIso, $buildNow, $buildState, $buildLog))
$build = @{ process = $null; log = (Join-Path $root 'private\launcher-build.log'); read = 0 }
$buildTimer = New-Object Windows.Forms.Timer -Property @{ Interval = 250 }
function Read-BuildLog {
    if (-not (Test-Path $build.log)) { return }
    $stream = [IO.File]::Open($build.log, 'Open', 'Read', 'ReadWrite, Delete')
    try {
        $stream.Position = $build.read
        $text = (New-Object IO.StreamReader($stream, [Text.Encoding]::Default)).ReadToEnd()
        $build.read = $stream.Length
    } finally { $stream.Dispose() }
    if (-not $text) { return }
    # Compiling prints thousands of lines; keep the box responsive.
    if ($buildLog.TextLength -gt 200000) { $buildLog.Text = $buildLog.Text.Substring(100000) }
    $buildLog.AppendText($text)
}
$buildTimer.Add_Tick({
    try {
        $done = $build.process.HasExited
        Read-BuildLog
        if (-not $done) { return }
        $buildTimer.Stop()
        $ok = $build.process.ExitCode -eq 0
        $build.process.Dispose(); $build.process = $null
        $buildState.Text = if ($ok) { 'Build complete. Press Play.' } else { 'Build failed. Log: private\launcher-build.log' }
        $buildNow.Enabled = $true; $pickIso.Enabled = $true
        Update-Ready
        if ($ok) { Select-Section 0 }
    } catch {
        $buildTimer.Stop()
        [Windows.Forms.MessageBox]::Show($_.Exception.Message, 'DOAXBV launcher') | Out-Null
    }
})
$buildNow.Add_Click({
    try {
        if (-not $iso.Text) { $pickIso.PerformClick() }
        if (-not $iso.Text) { return }
        New-Item -ItemType Directory -Force (Split-Path $build.log) | Out-Null
        Remove-Item -LiteralPath $build.log -Force -ErrorAction SilentlyContinue
        $build.read = 0; $buildLog.Clear()
        $start = New-Object Diagnostics.ProcessStartInfo 'cmd.exe'
        # cmd strips the outer quotes; NUL input lets BuildGame.cmd's closing pause return.
        $start.Arguments = '/d /c ""{0}" "{1}" > "{2}" 2>&1 < nul"' -f (Join-Path $root 'BuildGame.cmd'), $iso.Text, $build.log
        $start.UseShellExecute = $false; $start.CreateNoWindow = $true
        $start.EnvironmentVariables['PYTHONUNBUFFERED'] = '1'
        $build.process = [Diagnostics.Process]::Start($start)
        $buildNow.Enabled = $false; $pickIso.Enabled = $false
        $buildState.Text = 'Building... this can take a while.'
        $buildTimer.Start()
    } catch {
        [Windows.Forms.MessageBox]::Show($_.Exception.Message, 'DOAXBV launcher') | Out-Null
    }
})

$heightNames = [string[]]($heights | ForEach-Object { if ($_ -eq 480) { '480p (windowed)' } else { "$($_)p" } })
$nearest = @($heights | Where-Object { $_ -le [int]$settings.height }).Count - 1
$resolution = Add-Row $pages[2] 'Resolution' 0 (New-Choice $heightNames $nearest)
$msaaNames = [string[]]($samples | ForEach-Object { if ($_ -eq 1) { 'Off' } else { "$($_)x" } })
$msaa = Add-Row $pages[2] 'MSAA' 36 (New-Choice $msaaNames ([Array]::IndexOf($samples, [int]$settings.msaa)))
# Display settings on the left, renderer toggles on the right.
$smaa = New-Toggle $pages[2] 'SMAA edge smoothing' 72 $settings.smaa 0 218
$vrr = New-Toggle $pages[2] 'VRR display (even 60 Hz pacing)' 100 $settings.vrr 0 218
$fog = New-Toggle $pages[2] 'Fog' 128 $settings.fog 0 218
# Off keeps the game's original shadows for lower-powered PCs; see docs/sun-shadow.md.
$sun = New-Toggle $pages[2] 'Sun shadows' 156 $settings.sun_shadows 0 218
$pixelLighting = New-Toggle $pages[2] 'Per-pixel lighting' 72 $settings.pixelLighting 228 220
$smoothNormals = New-Toggle $pages[2] 'Smooth normals' 100 $settings.smoothNormals 228 220
$rtgi = New-Toggle $pages[2] 'Ray-traced GI (RTGI)' 128 $settings.rtgi 228 220
$hitColor = New-Toggle $pages[2] 'RTGI hit-point color' 156 $settings.rtgiHitColor 228 220
$strength = Add-Row $pages[2] 'RTGI strength' 184 (New-Object Windows.Forms.NumericUpDown -Property @{ Minimum = 0; Maximum = 4; DecimalPlaces = 2; Increment = 0.25; BorderStyle = 'FixedSingle'; BackColor = $field; ForeColor = $ink; Value = [Math]::Max([decimal]0, [Math]::Min([decimal]4, [decimal]$settings.rtgiStrength)); Enabled = $rtgi.Checked })
$hitColor.Enabled = $rtgi.Checked
$rtgi.Add_CheckedChanged({ $strength.Enabled = $rtgi.Checked; $hitColor.Enabled = $rtgi.Checked })
New-Note $pages[2] 'RTGI needs DXR and can slow matches. Direct3D 11 is the fallback.' 208 32 | Out-Null

# PowerShell 7 loads JSON numbers as Int64, so cast before finding their index.
$savedVolume = [Array]::IndexOf($volumes, [int]$settings.volume)
$volumeNames = [string[]]($volumes | ForEach-Object { if ($_ -eq 0) { 'Mute' } else { "$_%" } })
$volume = Add-Row $pages[3] 'Volume' 0 (New-Choice $volumeNames $savedVolume)
$shuffle = New-Toggle $pages[3] 'Shuffle music' 40 $settings.shuffle
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
$pages[3].Controls.Add($music)
New-Note $pages[3] 'Songs in this folder play at the Radio Station.' 114 40 | Out-Null

$unlock = New-Toggle $pages[4] 'Unlock swimsuits' 0 $settings.unlock_swimsuits
$demoCut = New-Toggle $pages[4] 'Shorter demo cuts' 28 $settings.demo_cut_hold
$freeCamera = New-Toggle $pages[4] 'Free camera (F1)' 56 $settings.free_camera
$idle = Add-Row $pages[4] 'Title screen idle' 92 (New-Choice $idleNames ([Array]::IndexOf($idleValues, [string]$settings.title_idle)))

$textures = New-Toggle $pages[5] 'Use texture pack' 0 $settings.textures
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
$pages[5].Controls.AddRange(@($textureFolder, $browse))
$textureDump = New-Toggle $pages[5] 'Dump original textures' 68 $settings.textureDump
New-Note $pages[5] 'F12 dumps the textures in view. Ctrl+F12 reloads the pack.' 100 40 | Out-Null

New-Note $pages[6] "Free camera`n    F1    Fly the camera`n`nTextures`n    F12    Dump textures in view`n    Ctrl+F12    Reload texture pack`n`nRTGI`n    F5 / F6    Strength down / up`n    F7    Split comparison`n    F8    Toggle GI`n    F9    Pause or resume`n    F10    Step one frame" 0 244 | Out-Null

function Update-Summary {
    $display = "$($resolution.SelectedItem)$dot" + $(if ($msaa.SelectedIndex) { "$($msaa.SelectedItem) MSAA" } else { 'No MSAA' }) + $(if ($smaa.Checked) { "$($dot)SMAA" })
    $render = 'Direct3D 11' + $(if ($rtgi.Checked) { ' / DXR' }) + $dot + $(if ($fog.Checked) { 'Fog on' } else { 'Fog off' }) + $(if ($vrr.Checked) { "$($dot)VRR" })
    $extras = @($(if ($sun.Checked) { 'Sun shadows' }), $(if ($unlock.Checked) { 'Swimsuits' }), $(if ($freeCamera.Checked) { 'Free camera' }), $(if ($textures.Checked) { 'Texture pack' }), $(if ($pixelLighting.Checked) { 'Per-pixel' }), $(if ($rtgi.Checked) { 'RTGI' })) | Where-Object { $_ }
    $summary.Text = "$display`n$render`n" + $(if ($extras) { $extras -join $dot } else { 'None' })
}

$state = @{ section = 0 }
$menu = for ($i = 0; $i -lt $sections.Count; $i++) {
    $item = New-Object Windows.Forms.Label -Property @{ Left = 24; Top = 214 + $i * 44; Width = 270; Height = 42; BackColor = [Drawing.Color]::Transparent; Cursor = 'Hand'; Tag = $i; AccessibleName = $sections[$i]; AccessibleRole = 'MenuItem' }
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
    $previous = $state.section
    $state.section = $index
    $menu[$previous].Invalidate()
    $menu[$index].Invalidate()
    $title.Text = $sections[$index].ToUpper()
    $subtitle.Text = $subtitles[$index]
    for ($i = 0; $i -lt $pages.Count; $i++) { $pages[$i].Visible = $i -eq $index }
    if ($index -eq 0) { Update-Summary; Update-Ready }
    # Paint only what changed, now, so the menu, title and new pane land together.
    [DOAXBV.Native]::RedrawWindow($form.Handle, [IntPtr]::Zero, [IntPtr]::Zero, 0x0180) | Out-Null
}
$form.Add_KeyDown({
    param($s, $e)
    # Lists and number boxes use the arrows themselves.
    if ($form.ActiveControl -is [Windows.Forms.ComboBox] -or $form.ActiveControl -is [Windows.Forms.NumericUpDown]) { return }
    $step = @{ Up = -1; Down = 1 }[[string]$e.KeyCode]
    if ($step) { Select-Section (($state.section + $step + $sections.Count) % $sections.Count); $e.Handled = $true }
    if ($e.KeyCode -eq 'Escape') { $form.Close() }
})
$form.Add_FormClosing({
    param($s, $e)
    if ($build.process) {
        [Windows.Forms.MessageBox]::Show('Setup is still building. Wait for it to finish before closing.', 'DOAXBV launcher') | Out-Null
        $e.Cancel = $true
    }
})

$play = New-Object Windows.Forms.Button -Property @{ Text = 'PLAY'; Left = 232; Top = 286; Width = 214; Height = 50; FlatStyle = 'Flat'; BackColor = [Drawing.Color]::Transparent; Cursor = 'Hand' }
$play.Add_Click({
    if ($build.process) { Select-Section 1 }
    elseif (-not (Test-Built)) { Select-Section 1; $pickIso.Focus() | Out-Null }
    else { $form.DialogResult = 'OK' }
})
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
# Controller, as in launcher_sdl.cpp: LB/RB switch sections, the d-pad moves between
# controls and left/right change a list or number, A toggles or presses, Start plays, B quits.
# shortcut: no pad input until a build supplies SDL3.dll; add an XInput fallback if players need the Build page by pad.
$sdl = @(Get-ChildItem (Join-Path $root 'private\setup-*\build\SDL3.dll'), (Join-Path $root 'SDL3.dll') -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending)[0]
$padReady = $false
if ($sdl -and [Environment]::Is64BitProcess) { try { $padReady = [DOAXBV.Pad]::Start($sdl.FullName) } catch {} }
if ($padReady) {
    function Select-PadSection($step) {
        Select-Section (($state.section + $step + $sections.Count) % $sections.Count)
        if (-not $pages[$state.section].SelectNextControl($null, $true, $true, $true, $false)) { $play.Focus() | Out-Null }
    }
    $padTimer = New-Object Windows.Forms.Timer -Property @{ Interval = 30 }
    $padTimer.Add_Tick({
        $action = [DOAXBV.Pad]::Poll()
        # Only the focused launcher acts, so the pad never types into another window.
        if (-not $action -or [Windows.Forms.Form]::ActiveForm -ne $form) { return }
        $focused = $form.ActiveControl
        $change = @{ left = -1; right = 1 }[$action]
        switch ($action) {
            'lb' { Select-PadSection -1 }
            'rb' { Select-PadSection 1 }
            'up' { $form.SelectNextControl($focused, $false, $true, $true, $true) | Out-Null }
            'down' { $form.SelectNextControl($focused, $true, $true, $true, $true) | Out-Null }
            { $change -and $focused -is [Windows.Forms.ComboBox] } {
                $focused.SelectedIndex = [Math]::Max(0, [Math]::Min($focused.Items.Count - 1, $focused.SelectedIndex + $change))
            }
            { $change -and $focused -is [Windows.Forms.NumericUpDown] } { if ($change -gt 0) { $focused.UpButton() } else { $focused.DownButton() } }
            'a' {
                if ($focused -is [Windows.Forms.CheckBox]) { $focused.Checked = -not $focused.Checked }
                elseif ($focused -is [Windows.Forms.Button]) { $focused.PerformClick() }
            }
            'start' { $play.PerformClick() }
            'b' { $form.Close() }
        }
    })
    $padTimer.Start()
}
# Without a build, -PlaySaved opens the window on the Build page instead.
if ($PlaySaved -and -not (Test-Built)) { Select-Section 1 }
if (-not ($PlaySaved -and (Test-Built)) -and $form.ShowDialog() -ne 'OK') { exit }

$height = $heights[$resolution.SelectedIndex]
$count = $samples[$msaa.SelectedIndex]
$percent = $volumes[$volume.SelectedIndex]
[pscustomobject]@{
    height = $height; msaa = $count; smaa = $smaa.Checked; volume = $percent; shuffle = $shuffle.Checked
    sun_shadows = $sun.Checked; demo_cut_hold = $demoCut.Checked; title_idle = $idleValues[$idle.SelectedIndex]; unlock_swimsuits = $unlock.Checked; free_camera = $freeCamera.Checked
    vrr = $vrr.Checked; fog = $fog.Checked; textures = $textures.Checked; textureFolder = $textureFolder.Text; textureDump = $textureDump.Checked
    pixelLighting = $pixelLighting.Checked; smoothNormals = $smoothNormals.Checked; rtgi = $rtgi.Checked; rtgiStrength = $strength.Value; rtgiHitColor = $hitColor.Checked
} | ConvertTo-Json | Set-Content $settingsPath -Encoding UTF8
# The runner parses the scale with atof, so always write a '.' decimal point.
$env:RECOMP_D3D_SCALE = ($height / 480).ToString([Globalization.CultureInfo]::InvariantCulture)
$env:RECOMP_D3D_MSAA = "$count"
$env:RECOMP_D3D_SMAA = if ($smaa.Checked) { '1' } else { '0' }
$env:RECOMP_D3D_SUN_SHADOW = if ($sun.Checked) { '1' } else { '0' }
$env:RECOMP_AUDIO_GAIN = ($percent / 100).ToString([Globalization.CultureInfo]::InvariantCulture)
$env:RECOMP_MUSIC_SHUFFLE = if ($shuffle.Checked) { '1' } else { '0' }
$env:RECOMP_TITLE_IDLE = $idleValues[$idle.SelectedIndex]
$env:RECOMP_UNLOCK_SWIMSUITS = if ($unlock.Checked) { '1' } else { '0' }
# Unset leaves the game's own one-second freeze; see docs/title-idle.md.
$env:RECOMP_DEMO_CUT_HOLD = if ($demoCut.Checked) { '12' } else { $null }
# F1 then flies the game's camera; see docs/free-camera.md.
$env:RECOMP_FREE_CAMERA = if ($freeCamera.Checked) { '1' } else { $null }
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
