# SPDX-License-Identifier: GPL-3.0-or-later
# Run with Windows PowerShell 5 and PowerShell 7 on Windows.
$ErrorActionPreference = 'Stop'
$originalCulture = [Threading.Thread]::CurrentThread.CurrentCulture
$originalEnv = @{}
$privateRoot = [IO.Path]::GetFullPath((Join-Path (Split-Path $PSScriptRoot -Parent) 'private'))
$testRoot = Join-Path $privateRoot ('launcher-test-' + [guid]::NewGuid())
New-Item -ItemType Directory -Path (Join-Path $testRoot 'tools') -Force | Out-Null
$launcher = Join-Path $testRoot 'tools\launcher.ps1'
$source = Get-Content (Join-Path $PSScriptRoot 'launcher.ps1') -Raw -Encoding UTF8
$tokens = $null; $errors = $null
$ast = [Management.Automation.Language.Parser]::ParseInput($source, [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw ($errors | Out-String) }
# Construct the real controls and execute Play's save/env path without a game.
$source = $source.Replace($ast.EndBlock.Traps[0].Extent.Text, 'trap { throw $_ }')
$source = $source.Replace('$form.ShowDialog()', '(& $choose)')
Set-Content -LiteralPath $launcher -Value $source -Encoding UTF8

function Assert($condition, $message) { if (!$condition) { throw $message } }
function Start-Process($FilePath, $WorkingDirectory) {
    Assert ($FilePath -eq (Join-Path $testRoot 'RunGame.cmd')) 'Wrong launch target'
    Assert ($WorkingDirectory -eq $testRoot) 'Wrong working directory'
    $script:launched = @{}
    foreach ($name in $expected.Keys) { $script:launched[$name] = [Environment]::GetEnvironmentVariable($name) }
}
function Check-Launch($json, $choose, $expected) {
    $path = Join-Path $testRoot 'private\launcher.json'
    New-Item -ItemType Directory -Path (Split-Path $path) -Force | Out-Null
    Set-Content -LiteralPath $path -Value $json -Encoding UTF8
    # Opposite inherited values must not override the selected settings.
    foreach ($name in $expected.Keys) {
        if (!$originalEnv.ContainsKey($name)) { $originalEnv[$name] = [Environment]::GetEnvironmentVariable($name) }
        [Environment]::SetEnvironmentVariable($name, 'inherited')
    }
    $script:launched = $null
    try {
        . $launcher
        Assert ($null -ne $script:launched) 'Play did not launch'
        foreach ($name in $expected.Keys) {
            Assert ($script:launched[$name] -ceq $expected[$name]) "Wrong ${name}: got '$($script:launched[$name])', expected '$($expected[$name])'"
        }
        $saved = Get-Content -LiteralPath $path -Raw -Encoding UTF8 | ConvertFrom-Json
        Assert ($saved.fog -eq $fog.Checked) 'Fog did not persist'
        Assert ($saved.textureFolder -ceq $textureFolder.Text) 'Texture path did not persist'
        Assert ($saved.rtgiStrength -eq $strength.Value) 'Strength did not persist'
    } finally { if ($form) { $form.Dispose() } }
}

try {
    [Threading.Thread]::CurrentThread.CurrentCulture = 'de-DE'
    $defaults = @{ RECOMP_D3D_VRR = '0'; RECOMP_D3D_FOG = '1'; RECOMP_TEXTURES = $null; RECOMP_TEXTURE_DUMP = '0'; RECOMP_TEXTURE_DUMP_DIR = (Join-Path $testRoot 'private\textures'); RECOMP_D3D_PIXEL_LIGHTING = '0'; RECOMP_D3D_SMOOTH_NORMALS = '0'; RECOMP_D3D_RTGI = '0'; RECOMP_D3D_RTGI_STRENGTH = '1'; RECOMP_D3D_RTGI_HITCOLOR = '1'; RECOMP_AUDIO_GAIN = '1' }
    $chooseDefaults = {
        Assert ($fog.Checked -and !$rtgi.Checked -and !$vrr.Checked -and !$textures.Checked) 'Wrong new-option defaults'
        Assert (!$strength.Enabled -and !$hitColor.Enabled) 'RTGI controls must be disabled'
        'OK'
    }
    Check-Launch '{"height":720,"msaa":4,"smaa":true}' $chooseDefaults ($defaults + @{ RECOMP_D3D_SCALE = '1.5'; RECOMP_D3D_MSAA = '4'; RECOMP_D3D_SMAA = '1' })
    Check-Launch '{"fog":null,"volume":null}' $chooseDefaults $defaults
    Check-Launch '{invalid json' $chooseDefaults $defaults
    $pack = Join-Path $testRoot ('pack ' + [char]0x00e9)
    $all = @{ height = 1080; msaa = 8; smaa = $true; volume = 75; shuffle = $true; vrr = $true; fog = $false; textures = $true; textureFolder = $pack; textureDump = $true; pixelLighting = $true; smoothNormals = $true; rtgi = $true; rtgiStrength = 1.25; rtgiHitColor = $false } | ConvertTo-Json
    Check-Launch $all {
        Assert ($rtgi.Checked -and $strength.Enabled -and $hitColor.Enabled) 'RTGI controls must be enabled'
        'OK'
    } @{ RECOMP_D3D_SCALE = '2.25'; RECOMP_D3D_MSAA = '8'; RECOMP_D3D_SMAA = '1'; RECOMP_AUDIO_GAIN = '0.75'; RECOMP_MUSIC_SHUFFLE = '1'; RECOMP_D3D_VRR = '1'; RECOMP_D3D_FOG = '0'; RECOMP_TEXTURES = $pack; RECOMP_TEXTURE_DUMP = '1'; RECOMP_D3D_PIXEL_LIGHTING = '1'; RECOMP_D3D_SMOOTH_NORMALS = '1'; RECOMP_D3D_RTGI = '1'; RECOMP_D3D_RTGI_STRENGTH = '1.25'; RECOMP_D3D_RTGI_HITCOLOR = '0' }
    Check-Launch $all {
        $rtgi.Checked = $false; $vrr.Checked = $false; $fog.Checked = $true; $textures.Checked = $false
        $textureDump.Checked = $false; $pixelLighting.Checked = $false; $smoothNormals.Checked = $false
        Assert (!$strength.Enabled -and !$hitColor.Enabled) 'RTGI toggle did not disable controls'
        'OK'
    } @{ RECOMP_D3D_VRR = '0'; RECOMP_D3D_FOG = '1'; RECOMP_TEXTURES = $null; RECOMP_TEXTURE_DUMP = '0'; RECOMP_D3D_PIXEL_LIGHTING = '0'; RECOMP_D3D_SMOOTH_NORMALS = '0'; RECOMP_D3D_RTGI = '0'; RECOMP_D3D_RTGI_STRENGTH = '1.25'; RECOMP_D3D_RTGI_HITCOLOR = '0' }
    Write-Output "Launcher settings checks passed (PowerShell $($PSVersionTable.PSVersion))."
} finally {
    [Threading.Thread]::CurrentThread.CurrentCulture = $originalCulture
    foreach ($entry in $originalEnv.GetEnumerator()) { [Environment]::SetEnvironmentVariable($entry.Key, $entry.Value) }
    $resolved = (Resolve-Path -LiteralPath $testRoot).Path
    if (!$resolved.StartsWith($privateRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe test cleanup path' }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
