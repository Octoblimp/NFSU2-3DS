<#
.SYNOPSIS
    Repack and stage every runtime asset the port reads into the nfsu2data
    folder (TASKS.md #107), or pack a PC mod for it (#320).

.DESCRIPTION
    A PowerShell front end for tools\stage.py, which holds the plan (one step
    per output: a retail file copied as-is, or a packer run on the PC
    install; a step runs only when it is stale). stage.py runs on any system
    with Python 3; tools\nfsu2_setup.pyw and tools\nfsu2_mods.pyw are the
    same with a window.

    Usage (PowerShell, never cmd.exe):
      powershell -ExecutionPolicy Bypass -File tools\stage_sd.ps1 -List
      powershell -ExecutionPolicy Bypass -File tools\stage_sd.ps1
      powershell -ExecutionPolicy Bypass -File tools\stage_sd.ps1 -Only cars,tracks -Tracks L4RG
      powershell -ExecutionPolicy Bypass -File tools\stage_sd.ps1 -Force -Only audio
      powershell -ExecutionPolicy Bypass -File tools\stage_sd.ps1 -ModDir "<a PC mod>" -ModName mymod

.PARAMETER GameDir
    The nfsu2data folder to stage into. Defaults to the Azahar sdmc's.

.PARAMETER PcRoot
    The NFSU2 PC install. Defaults to the repo's "PC Files".

.PARAMETER Only
    Step groups: cars, kits, carextras, frontend, global, text, movies, video,
    audio, music, speech, nis, tracks. Default: all.

.PARAMETER Tracks
    The track sets the tracks group packs. Default: all six.

.PARAMETER Force
    Run every selected step even when it is up to date.

.PARAMETER List
    Print every step with its state and run nothing.

.PARAMETER ReportExtra
    After staging (or with -List), print files under GameDir no step makes.

.PARAMETER ModDir
    Pack this PC mod (a folder laid out like the PC game's) into
    GameDir\mods\<ModName> instead of staging the game data.

.PARAMETER ModName
    The mod's folder under mods\. Default: the mod folder's own name.
#>
param(
    [string]$GameDir = (Join-Path $env:APPDATA "Azahar\sdmc\nfsu2data"),
    [string]$PcRoot = (Join-Path (Split-Path -Parent $PSScriptRoot) "PC Files"),
    [string[]]$Only = @(),
    [string[]]$Tracks = @("L4RA", "L4RB", "L4RC", "L4RD", "L4RF", "L4RG"),
    [switch]$Force,
    [switch]$List,
    [switch]$ReportExtra,
    [string]$ModDir,
    [string]$ModName
)
$ErrorActionPreference = "Stop"
# powershell -File hands "a,b" over as one string.
$Only = @($Only | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$Tracks = @($Tracks | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
# The audio builders' dspenc needs the msys runtime.
$msys = "C:\devkitPro\msys2\usr\bin"
if ((Test-Path $msys) -and ($env:PATH -notlike "*$msys*")) { $env:PATH = "$env:PATH;$msys" }

$a = @((Join-Path $PSScriptRoot "stage.py"), "--pc", $PcRoot, "--gamedir", $GameDir)
if ($ModDir) {
    $a += @("--mod", $ModDir)
    if ($ModName) { $a += @("--mod-name", $ModName) }
} else {
    if ($Only.Count) { $a += @("--only", ($Only -join ",")) }
    $a += @("--tracks", ($Tracks -join ","))
    if ($Force) { $a += "--force" }
    if ($List) { $a += "--list" }
    if ($ReportExtra) { $a += "--report-extra" }
}
& python @a
exit $LASTEXITCODE
