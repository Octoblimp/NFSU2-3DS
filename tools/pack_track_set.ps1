<#
.SYNOPSIS
    Pack one retail track set for the port (TASKS.md #271).

.DESCRIPTION
    The world loads the set of the track it is entered for -- TrackInfo +0x40,
    "L4RA" the city, "L4RG" the URL speedways, L4RB/L4RD drags, L4RC drift,
    L4RF Street X (see trackdb.h) -- so each set a race can be on is packed the
    way L4RA is: pack_world.py's archive, collision and index, then the optional
    per-set packs, then the set's retail .BUN and ROUTES folder copied as-is.

    smokeable_pack.py fails on a set with no smokeable props (L4RG has none);
    its file is optional, so that is a warning here, not a stop.

    Usage: powershell -ExecutionPolicy Bypass -File tools/pack_track_set.ps1 -Track L4RG

.PARAMETER Track
    The set, e.g. L4RG.

.PARAMETER GameDir
    The nfsu2data folder to write into. Defaults to the Azahar sdmc's.

.PARAMETER PcRoot
    The NFSU2 PC install. Defaults to the repo's "PC Files". (tools/stage.py
    runs these same steps itself, on any system.)
#>
param(
    [Parameter(Mandatory = $true)][string]$Track,
    [string]$GameDir = (Join-Path $env:APPDATA "Azahar\sdmc\nfsu2data"),
    [string]$PcRoot
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$pc = if ($PcRoot) { $PcRoot } else { Join-Path $root "PC Files" }
$out = Join-Path $GameDir "TRACKS\$Track"
Set-Location $root
$t0 = Get-Date
python tools/pack_world.py "$pc" --out "$out" --track $Track --topology
if ($LASTEXITCODE -ne 0) { throw "pack_world.py failed on $Track" }
python tools/lightdb_pack.py "$pc" "$GameDir" --track $Track
if ($LASTEXITCODE -ne 0) { throw "lightdb_pack.py failed on $Track" }
python tools/emevents_pack.py "$pc" "$GameDir" --track $Track
if ($LASTEXITCODE -ne 0) { throw "emevents_pack.py failed on $Track" }
python tools/smokeable_pack.py "$pc" "$GameDir" --track $Track
if ($LASTEXITCODE -ne 0) { Write-Warning "smokeable_pack.py wrote nothing for $Track (optional)" }
python tools/flare_pack.py "$pc" "$GameDir" --track $Track
if ($LASTEXITCODE -ne 0) { throw "flare_pack.py failed on $Track" }
# The resident track file and the routes, as-is from the PC install.
Copy-Item (Join-Path $pc "TRACKS\$Track.BUN") (Join-Path $GameDir "TRACKS\$Track.BUN") -Force
# Copy-Item keeps the PC install's own (old) timestamp; stage_sd.ps1 judges this
# set fresh by its outputs being newer than the packers, so stamp the copy now.
(Get-Item -LiteralPath (Join-Path $GameDir "TRACKS\$Track.BUN")).LastWriteTimeUtc = [datetime]::UtcNow
$rdst = Join-Path $GameDir "TRACKS\ROUTES$Track"
New-Item -ItemType Directory -Force $rdst | Out-Null
Copy-Item (Join-Path $pc "TRACKS\ROUTES$Track\*") $rdst -Force
"packed $Track in {0:N0} s" -f ((Get-Date) - $t0).TotalSeconds
Get-ChildItem $out | Select-Object Name, Length | Format-Table -AutoSize
