<#
.SYNOPSIS
    Write a profile's .display file from the PC game's display settings.

.DESCRIPTION
    SPEED2.EXE keeps its display settings (the Display and Customize pages and
    the Level of Detail) in the registry, one DWORD per value under
    HKLM\Software\EA Games\Need for Speed Underground 2 (FUN_005BEA20 and
    FUN_005BEEA0). The game runs without administrator rights, so Windows
    redirects its writes to the per-user VirtualStore copy of that key, which
    is read first here; the real HKLM key is the fallback.

    The 3DS port keeps the same values per profile, in <save>.display beside the
    profile's save (3DS Files/source/display.h, TASKS.md #326). This writes
    that file from the registry so the PC's settings carry over as they are.
    The port clamps each value to what it draws when it loads the file, as the
    PC clamps to what its card can do; the file keeps the PC's values.

    The 3DS-only stereo 3D lines (Stereo3D*) of an existing file are kept.

.PARAMETER ProfileName
    The profile's name: its folder and save file under SAVE\NFS Underground 2.

.PARAMETER SaveRoot
    The SAVE\NFS Underground 2 folder to write into. Defaults to the Azahar
    SD card's.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\display_from_registry.ps1 -ProfileName Player
#>
[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [Parameter(Mandatory = $true)]
    [string] $ProfileName,
    [string] $SaveRoot = (Join-Path $env:APPDATA 'Azahar\sdmc\nfsu2data\SAVE\NFS Underground 2')
)

$ErrorActionPreference = 'Stop'

# FUN_005BEEA0's names in its order. g_PerformanceLevel is not in the block
# but is saved between g_RacingResolution and g_VSyncOn.
$names = @(
    'VERSION', 'SIZE',
    'g_CarEnvironmentMapEnable', 'g_CarEnvironmentMapUpdateData',
    'g_CarShadowEnable', 'g_CarHeadlightEnable', 'g_CarLightingEnable',
    'g_CarDamageEnable', 'g_CrowdEnable', 'g_RoadReflectionEnable',
    'g_FogEnable', 'g_MotionBlurEnable', 'g_LightStreaksEnable',
    'g_LightGlowEnable', 'g_AnimatedTextureEnable', 'g_ParticleSystemEnable',
    'g_DepthOfFieldEnable', 'g_WorldLodLevel', 'g_CarLodLevel',
    'g_OverBrightEnable', 'g_BleachByPassEnable', 'g_TintingEnable',
    'g_FSAALevel', 'g_HorizonFogEnable', 'g_RainEnable',
    'g_TextureFiltering', 'g_RacingResolution', 'g_PerformanceLevel',
    'g_VSyncOn'
)

$keys = @(
    'HKCU:\Software\Classes\VirtualStore\MACHINE\SOFTWARE\WOW6432Node\EA GAMES\Need for Speed Underground 2',
    'HKLM:\SOFTWARE\WOW6432Node\EA GAMES\Need for Speed Underground 2',
    'HKLM:\SOFTWARE\EA GAMES\Need for Speed Underground 2'
)
$values = $null
foreach ($k in $keys) {
    if (-not (Test-Path $k)) { continue }
    $p = Get-ItemProperty $k
    if ($null -eq $p.PSObject.Properties['VERSION']) { continue }
    $values = $p
    Write-Host "Reading $k"
    break
}
if ($null -eq $values) {
    throw 'No Need for Speed Underground 2 display settings in the registry. Run the PC game once and set them on its Display page.'
}

$dir = Join-Path $SaveRoot $ProfileName
$save = Join-Path $dir $ProfileName
if (-not (Test-Path -LiteralPath $save)) {
    throw "No save for profile '$ProfileName' at $save."
}
$out = "$save.display"

$lines = New-Object System.Collections.Generic.List[string]
$lines.Add('# Need for Speed Underground 2 display settings: SPEED2.EXE''s registry')
$lines.Add('# values (HKLM\Software\EA Games\Need for Speed Underground 2).')
foreach ($n in $names) {
    $prop = $values.PSObject.Properties[$n]
    if ($null -eq $prop) {
        Write-Warning "$n is not in the registry; the port keeps its default for it."
        continue
    }
    $lines.Add(('{0}={1}' -f $n, [uint32]$prop.Value))
}

$stereo = @()
if (Test-Path -LiteralPath $out) {
    $stereo = @(Get-Content -LiteralPath $out | Where-Object { $_ -match '^\s*Stereo3D' })
}
if ($stereo.Count -gt 0) {
    $lines.Add('# 3DS stereoscopic 3D (not on the PC). Depth, focus and HUD are 0..10.')
    foreach ($s in $stereo) { $lines.Add($s.Trim()) }
}

if ($PSCmdlet.ShouldProcess($out, 'Write the display settings')) {
    $tmp = "$out.tmp"
    [System.IO.File]::WriteAllText($tmp, (($lines -join "`n") + "`n"), (New-Object System.Text.UTF8Encoding($false)))
    Move-Item -LiteralPath $tmp -Destination $out -Force
    Write-Host "Wrote $out"
    $lines | Where-Object { $_ -notmatch '^#' } | ForEach-Object { Write-Host "  $_" }
}
