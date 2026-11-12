<#
.SYNOPSIS
    Copy the rebuilt game data and the build onto the 3DS SD card.

.DESCRIPTION
    The port reads its data from sdmc:/nfsu2data. Only some of it is rebuilt in
    any given session, and the folders are large (CARS is ~420 MB), so this
    copies the set that has actually changed rather than the whole tree, and
    skips files whose size and timestamp already match.

    TRACKS/L4RA is copied file by file since 2026-10-26: WORLD.NFSZ is
    required and holds every section and the texture bank, so the loose
    section .nfsc files and WORLD.NFSB are no longer read and are not copied
    (TASKS.md #270). pack_world.py deletes them once the archive is verified
    (2026-10-29), and a card that still has them can delete them too.

    WORLD.NFST is the one that matters most: without it the port has no retail
    collision data and falls back to colliding with the drawn geometry, which
    puts walls on kerbs, hedges and overhangs (TASKS.md #231). A version 1 file
    (before TASKS.md #232) loads but leaves every race's barriers solid.

    Run it with the card in the reader and nothing on the 3DS using it.

.PARAMETER SdRoot
    The root of the SD card, e.g. E:\ -- the drive that has a 3ds\ folder on it.

.PARAMETER Source
    Where the packed data is. Defaults to the Azahar sdmc used for testing.

.PARAMETER WhatIf
    List what would be copied and how much, and copy nothing.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\copy_to_sd.ps1 -SdRoot E:\ -WhatIf
    powershell -ExecutionPolicy Bypass -File tools\copy_to_sd.ps1 -SdRoot E:\
#>
[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [Parameter(Mandatory = $true)]
    [string] $SdRoot,

    [string] $Source = (Join-Path $env:APPDATA 'Azahar\sdmc\nfsu2data'),

    [string] $Build
)

$ErrorActionPreference = 'Stop'

# $PSScriptRoot is not bound yet while the param defaults are evaluated, so the
# build path is resolved here instead.
if (-not $Build) {
    $Build = Join-Path (Split-Path $MyInvocation.MyCommand.Path -Parent) '..\3DS Files\NFSU2-3DS.3dsx'
}

if (-not (Test-Path -LiteralPath $Source)) {
    throw "No packed data at $Source. Run tools\pack_world.py first."
}
if (-not (Test-Path -LiteralPath $SdRoot)) {
    throw "$SdRoot is not there. Is the card in the reader?"
}

# A card that has never held this port has no nfsu2data; one that has should
# also have a 3ds\ folder. Neither is fatal, but both are worth saying out loud
# before writing 700 MB to the wrong drive.
if (-not (Test-Path -LiteralPath (Join-Path $SdRoot '3ds'))) {
    Write-Warning "$SdRoot has no 3ds\ folder -- check this is the 3DS card."
}

# Folders are mirrored in full; single files are named. Both are copied only
# where the source is newer or a different size.
$folders = @(
    @{ Path = 'CARS';            Why = 'car and part models, texture-slot fix; .nfss drop shadows (#257)' }
    @{ Path = 'video';          Why = 'boot videos, the first career movie (scene01.mvd) and the 23 in-game career movies (dragtut.mvd .. upperclass.mvd, #268 J), for the New 3DS hardware decoder' }
    @{ Path = 'MOVIES';          Why = 'the .vp6 movies the port plays: the boot four, Scene01.vp6, the first career movie (#259), and the 23 in-game career movies, DragTut.vp6 .. UpperClass.vp6 (#268 J; all optional, a missing one is skipped)' }
    @{ Path = 'audio';           Why = "each movie's soundtrack as PCM (Scene01.wav and the 23 in-game career movies' DragTut.wav .. UpperClass.wav, #268 J), FESFX.BIN, the menus' sounds (#259; optional), FEMB.BIN, the shop's install sounds (#298; optional), NOS\*.aem, the nitrous banks, ENGINE\*.aem and *.gin, the engine banks (SWTN_CAR_*_MB.aem the shift sweeteners) and Ginsu recordings, SHIFTING\*.aem, the gear banks, TURBO\*.aem, the turbo banks, SKIDS\*.aem, the tyre screech banks, IG_GLOBAL\*.aem, the road and wind noise banks (WIND_00_MB also the weather's wind), the collisions' (MB_Stich_Collision, MB_Stitch_Whoosh, FX_Misc_MB) ENV_COMMON_MB, the stereos and the traffic's horns, and TRAFFIC_MB, the traffic's engines (#316), and MIXMAPS\MAPOUTPUT*.mxb, the mix maps: free roam's and the drift, drag and URL races' (#298; optional)" }
    @{ Path = 'CREDITS';         Why = "Credits.fng's roll, retail's own UTF-16 text files copied verbatim (#259; optional, the screen opens with the roll empty without them)" }
)
# Each packed track set (#271): L4RA is the city, L4RG the URL speedways, L4RB
# and L4RD the drag strips (#268 D). The world loads the set of the track it is
# entered for, so every set a race can be on needs its folder, its retail .BUN
# and its ROUTES folder. A set the source has not been packed for is skipped
# with the missing-file warning.
$trackSets = @('L4RA', 'L4RG', 'L4RB', 'L4RD', 'L4RC', 'L4RF')
$setFiles = @(
    @{ Name = 'WORLD.NFSZ';   Why = 'every section and the texture bank in one LZ4 archive -- required since #270' }
    @{ Name = 'WORLD.NFSX';   Why = 'the section index the archive is checked against' }
    @{ Name = 'WORLD.NFST';   Why = 'collision, v3 with barrier groups and section trees' }
    @{ Name = 'WORLD.NFSG';   Why = 'scenery groups (#237)' }
    @{ Name = 'ACIDFX.nfsa';  Why = 'shop and event markers, smoke (G2)' }
    @{ Name = 'SKYDOME.nfsc'; Why = 'the sky dome' }
    @{ Name = 'SHADOW.nfsc';  Why = 'the world shadow pass' }
    @{ Name = 'WORLD.NFSL';   Why = 'the street lamps that light a car, tools/lightdb_pack.py (#166; optional)' }
    @{ Name = 'WORLD.NFSE';   Why = 'the sound spheres the car sets off, the roadside wooshes, tools/emevents_pack.py (#298; optional)' }
    @{ Name = 'WORLD.NFSK';   Why = 'the props a car knocks over, tools/smokeable_pack.py (#318; optional, and a set with none has no file)' }
    @{ Name = 'WORLD.NFSF';   Why = 'the world light flares, tools/flare_pack.py (#323; optional)' }
    @{ Name = 'WORLD.NFSW';   Why = 'the world animations, the jets, trains, cranes and spotlights, tools/track_pack.py world_anim.py (#239; optional, and a set with none has no file)' }
)
foreach ($t in $trackSets) {
    $folders += @{ Path = "TRACKS\ROUTES$t"; Why = "the $t set's race routes, AI lines and trigger zones" }
}
$files = @()
foreach ($t in $trackSets) {
    $files += @{ Path = "TRACKS\$t.BUN"; Why = "retail's own, the $t set's visible sections and parameter maps" }
    foreach ($sf in $setFiles) {
        $files += @{ Path = "TRACKS\$t\$($sf.Name)"; Why = $sf.Why }
    }
}
$files += @(
    @{ Path = 'GLOBAL\HUD.nfsh';       Why = 'in-game HUD pack (the speedometer)' }
    @{ Path = 'GLOBAL\PARTMODELS.nfsm'; Why = 'car part models' }
    @{ Path = 'GLOBAL\LIGHTMATS.nfsl';  Why = 'the car light materials the paint gloss reads, tools/lightmat_pack.py (#166; optional)' }
    @{ Path = 'GLOBAL\PAINT.nfsp';      Why = 'the paints, v2 with gloss (#296)' }
    @{ Path = 'GLOBAL\NEON.nfsn';       Why = 'car neon colours and tube texture (#257)' }
    @{ Path = 'GLOBAL\CARLIGHTS.nfsl';  Why = 'car head- and brake-light flares and bulb colours, tools/carlights_pack.py (#296; optional)' }
    @{ Path = 'GLOBAL\CARFX.nfsa';      Why = 'the Nitrous Purge spray, the nitrous flame and the flag girl''s flares, tools/carfx_pack.py (#372/#317/#497; optional, the purge then only sounds)' }
    @{ Path = 'GLOBAL\STOCKPARTS.nfss'; Why = 'every car''s stock parts for the career car lot, tools/stockparts_pack.py (#277)' }
    @{ Path = 'GLOBAL\GPSARROW.nfsc';   Why = 'the GPS arrow over the car, tools/gps_arrow_pack.py (#275; optional)' }
    @{ Path = 'FRONTEND\Crib01.nfsc';   Why = 'the career garage behind UI_CareerCrib (#259; optional, the alley stands in)' }
    @{ Path = 'FRONTEND\CarLot01.nfsc'; Why = 'the car lot''s room behind UI_CareerCarLot (#277; optional, the alley stands in)' }
    @{ Path = 'FRONTEND\PerfShop01.nfsc'; Why = 'the performance shop''s room, tools/scene_pack.py (#259; optional, the alley stands in)' }
    @{ Path = 'FRONTEND\PartsShop01.nfsc'; Why = 'the body shop''s room, tools/scene_pack.py from PLATFORMS/PartsShop01.BIN (#371; optional, the alley stands in)' }
    @{ Path = 'FRONTEND\PaintShop01.nfsc'; Why = 'the graphics shop''s room, tools/scene_pack.py from PLATFORMS/PaintShop01.BIN (#372; optional, the alley stands in)' }
    @{ Path = 'FRONTEND\AudioShop01.nfsc'; Why = 'the specialties shop''s room, tools/scene_pack.py from PLATFORMS/AudioShop01.BIN (#372; optional, the alley stands in)' }
    @{ Path = 'FRONTEND\MAGAZINES_SHOWCASE.BIN'; Why = "retail's own, the photo shoot's camera, magazine and masthead setups (#268; optional, a showcase then shoots with the chase camera and no masthead)" }
    @{ Path = 'FRONTEND\MAGAZINES_STREAMING_LOW.bin'; Why = "retail's own, the photo shoot's magazine mastheads (#268; optional, the shot is then taken without one)" }
    @{ Path = 'FRONTEND\MAGAZINES_FRONTEND.BIN'; Why = "retail's own, the frontend magazine's car, camera, light and cover setups (#268; optional, the cover then has no car)" }
    @{ Path = 'FRONTEND\MAGAZINES_STREAMING_HIGH.bin'; Why = "retail's own, the frontend magazine's backs, mastheads, models and extras (#268; optional, the cover is then the car alone)" }
    @{ Path = 'GLOBAL\InGameRace.bun';  Why = "retail's own, for the post-race screens (#220; optional, a race then goes straight back to free roam)" }
    @{ Path = 'GLOBAL\InGameDrag.bun';  Why = "retail's own, HUD_Drag.fng for the drag races' HUD (#268 D; optional, a drag then keeps the circuit HUD)" }
    @{ Path = 'GLOBAL\InGameDrift.bun'; Why = "retail's own, HUD_Drift.fng for the drift races' HUD (#268 E; optional, a drift then keeps the circuit HUD)" }
    @{ Path = 'NIS\Scene_CareerNis1_BundleB.bun'; Why = "retail's own, the career-start NIS's scene header and anims (#343; optional, a new career then starts without it)" }
    @{ Path = 'NIS\EndingNis01.crowd'; Why = 'the race-end crowd scene 1, tools/crowd_pack.py (#354; optional, a race then ends without it)' }
    @{ Path = 'NIS\EndingNis02.crowd'; Why = 'the race-end crowd scene 2, tools/crowd_pack.py (#354; optional)' }
    @{ Path = 'NIS\EndingNis03.crowd'; Why = 'the race-end crowd scene 3, tools/crowd_pack.py (#354; optional)' }
    @{ Path = 'NIS\EndingNis04.crowd'; Why = 'the race-end crowd scene 4, tools/crowd_pack.py (#354; optional)' }
    @{ Path = 'NIS\IntroNis01.crowd'; Why = 'the race-start crowd scene 1, tools/crowd_pack.py (#244; optional, the countdown then starts at once)' }
    @{ Path = 'NIS\IntroNis02.crowd'; Why = 'the race-start crowd scene 2, tools/crowd_pack.py (#244; optional, the countdown then starts at once)' }
    @{ Path = 'NIS\IntroNis03.crowd'; Why = 'the race-start crowd scene 3, tools/crowd_pack.py (#244; optional, the countdown then starts at once)' }
    @{ Path = 'NIS\IntroNis05.crowd'; Why = 'the race-start crowd scene 5, tools/crowd_pack.py (#244; optional, the countdown then starts at once)' }
    @{ Path = 'NIS\IntroNis06.crowd'; Why = 'the race-start crowd scene 6, tools/crowd_pack.py (#244; optional, the countdown then starts at once)' }
    @{ Path = 'NIS\IntroNis07.crowd'; Why = 'the race-start crowd scene 7, tools/crowd_pack.py (#244; optional, the countdown then starts at once)' }
    @{ Path = 'NIS\IntroNis08.crowd'; Why = 'the race-start crowd scene 8, tools/crowd_pack.py (#244; optional, the countdown then starts at once)' }
    @{ Path = 'NIS\IntroNis10.crowd'; Why = 'the race-start crowd scene 10, tools/crowd_pack.py (#244; optional, the countdown then starts at once)' }
    @{ Path = 'NIS\IntroNis12.crowd'; Why = 'the race-start crowd scene 12, tools/crowd_pack.py (#244; optional, the countdown then starts at once)' }
    @{ Path = 'NIS\IntroNis13.crowd'; Why = 'the race-start crowd scene 13, tools/crowd_pack.py (#244; optional, the countdown then starts at once)' }
    @{ Path = 'NIS\IntroNis14.crowd'; Why = 'the race-start crowd scene 14, tools/crowd_pack.py (#244; optional, the countdown then starts at once)' }
    @{ Path = 'NIS\IntroNis15.crowd'; Why = 'the race-start crowd scene 15, tools/crowd_pack.py (#244; optional, the countdown then starts at once)' }
    @{ Path = 'NIS\IntroNis19.crowd'; Why = 'the race-start crowd scene 19, tools/crowd_pack.py (#244; optional, the countdown then starts at once)' }
    @{ Path = 'NIS\IntroNis20.crowd'; Why = 'the race-start crowd scene 20, tools/crowd_pack.py (#244; optional, the countdown then starts at once)' }
    @{ Path = 'NIS\IntroNis21.crowd'; Why = 'the race-start crowd scene 21, tools/crowd_pack.py (#244; optional, the countdown then starts at once)' }
    @{ Path = 'NIS\IntroNis22.crowd'; Why = 'the race-start crowd scene 22, tools/crowd_pack.py (#244; optional, the countdown then starts at once)' }
    @{ Path = 'NIS\IntroNis24.crowd'; Why = 'the race-start crowd scene 24, tools/crowd_pack.py (#244; optional, the countdown then starts at once)' }
    @{ Path = 'NIS\IntroNis25.crowd'; Why = 'the race-start crowd scene 25, tools/crowd_pack.py (#244; optional, the countdown then starts at once)' }
    @{ Path = 'NIS\IntroNis26.crowd'; Why = 'the race-start crowd scene 26, tools/crowd_pack.py (#244; optional, the countdown then starts at once)' }
    @{ Path = 'NIS\IntroNis27.crowd'; Why = 'the race-start crowd scene 27, tools/crowd_pack.py (#244; optional, the countdown then starts at once)' }
    @{ Path = 'NIS\ShopEnter1.crowd'; Why = 'the shop walk-in scene (car track + scene header), tools/crowd_pack.py (#408; optional, the walk-in then goes straight in)' }
    @{ Path = 'NIS\ShopExit1.crowd'; Why = 'the way-out-of-a-shop scene, tools/crowd_pack.py (#408; optional)' }
    @{ Path = 'NIS\Scene_ShopEnter1_EvtTrk.txt'; Why = "retail's own, the walk-in scene's door events (#408; optional, the doors then stay shut)" }
    @{ Path = 'NIS\Scene_ShopExit1_EvtTrk.txt'; Why = "retail's own, the way-out scene's door events (#408; optional)" }
    @{ Path = 'NIS\Scene_DynoEnter_EvtTrk.txt'; Why = "retail's own, the scene's engine revs (CAR_AUDIO_PATTERN, #497/#520; optional)" }
    @{ Path = 'NIS\Scene_EndingNis01_EvtTrk.txt'; Why = "retail's own, the scene's engine revs (CAR_AUDIO_PATTERN, #497/#520; optional)" }
    @{ Path = 'NIS\Scene_EndingNis02_EvtTrk.txt'; Why = "retail's own, the scene's engine revs (CAR_AUDIO_PATTERN, #497/#520; optional)" }
    @{ Path = 'NIS\Scene_EndingNis03_EvtTrk.txt'; Why = "retail's own, the scene's engine revs (CAR_AUDIO_PATTERN, #497/#520; optional)" }
    @{ Path = 'NIS\Scene_IntroNis01_EvtTrk.txt'; Why = "retail's own, the scene's engine revs (CAR_AUDIO_PATTERN, #497/#520; optional)" }
    @{ Path = 'NIS\Scene_IntroNis02_EvtTrk.txt'; Why = "retail's own, the scene's engine revs (CAR_AUDIO_PATTERN, #497/#520; optional)" }
    @{ Path = 'NIS\Scene_IntroNis03_EvtTrk.txt'; Why = "retail's own, the scene's engine revs (CAR_AUDIO_PATTERN, #497/#520; optional)" }
    @{ Path = 'NIS\Scene_IntroNis05_EvtTrk.txt'; Why = "retail's own, the scene's engine revs (CAR_AUDIO_PATTERN, #497/#520; optional)" }
    @{ Path = 'NIS\Scene_IntroNis06_EvtTrk.txt'; Why = "retail's own, the scene's engine revs (CAR_AUDIO_PATTERN, #497/#520; optional)" }
    @{ Path = 'NIS\Scene_IntroNis07_EvtTrk.txt'; Why = "retail's own, the scene's engine revs (CAR_AUDIO_PATTERN, #497/#520; optional)" }
    @{ Path = 'NIS\Scene_IntroNis08_EvtTrk.txt'; Why = "retail's own, the scene's engine revs (CAR_AUDIO_PATTERN, #497/#520; optional)" }
    @{ Path = 'NIS\Scene_IntroNis10_EvtTrk.txt'; Why = "retail's own, the scene's engine revs (CAR_AUDIO_PATTERN, #497/#520; optional)" }
    @{ Path = 'NIS\Scene_IntroNis12_EvtTrk.txt'; Why = "retail's own, the scene's engine revs (CAR_AUDIO_PATTERN, #497/#520; optional)" }
    @{ Path = 'NIS\Scene_IntroNis13_EvtTrk.txt'; Why = "retail's own, the scene's engine revs (CAR_AUDIO_PATTERN, #497/#520; optional)" }
    @{ Path = 'NIS\Scene_IntroNis14_EvtTrk.txt'; Why = "retail's own, the scene's engine revs (CAR_AUDIO_PATTERN, #497/#520; optional)" }
    @{ Path = 'NIS\Scene_IntroNis15_EvtTrk.txt'; Why = "retail's own, the scene's engine revs (CAR_AUDIO_PATTERN, #497/#520; optional)" }
    @{ Path = 'NIS\Scene_IntroNis19_EvtTrk.txt'; Why = "retail's own, the scene's engine revs (CAR_AUDIO_PATTERN, #497/#520; optional)" }
    @{ Path = 'NIS\Scene_IntroNis20_EvtTrk.txt'; Why = "retail's own, the scene's engine revs (CAR_AUDIO_PATTERN, #497/#520; optional)" }
    @{ Path = 'NIS\Scene_IntroNis21_EvtTrk.txt'; Why = "retail's own, the scene's engine revs (CAR_AUDIO_PATTERN, #497/#520; optional)" }
    @{ Path = 'NIS\Scene_IntroNis22_EvtTrk.txt'; Why = "retail's own, the scene's engine revs (CAR_AUDIO_PATTERN, #497/#520; optional)" }
    @{ Path = 'NIS\Scene_IntroNis24_EvtTrk.txt'; Why = "retail's own, the scene's engine revs (CAR_AUDIO_PATTERN, #497/#520; optional)" }
    @{ Path = 'NIS\Scene_IntroNis25_EvtTrk.txt'; Why = "retail's own, the scene's engine revs (CAR_AUDIO_PATTERN, #497/#520; optional)" }
    @{ Path = 'NIS\Scene_IntroNis26_EvtTrk.txt'; Why = "retail's own, the scene's engine revs (CAR_AUDIO_PATTERN, #497/#520; optional)" }
    @{ Path = 'NIS\Scene_IntroNis27_EvtTrk.txt'; Why = "retail's own, the scene's engine revs (CAR_AUDIO_PATTERN, #497/#520; optional)" }
)

$total = 0L
foreach ($f in $folders + $files) {
    $src = Join-Path $Source $f.Path
    if (Test-Path -LiteralPath $src) {
        $bytes = (Get-ChildItem -LiteralPath $src -Recurse -File |
                  Measure-Object -Property Length -Sum).Sum
        if ($null -eq $bytes) { $bytes = 0 }
        $total += $bytes
        "{0,-24} {1,8:N1} MB  {2}" -f $f.Path, ($bytes / 1MB), $f.Why
    } else {
        Write-Warning ("missing from the pack, skipped: {0}" -f $f.Path)
    }
}
"{0,-24} {1,8:N1} MB  total to consider" -f '', ($total / 1MB)
""

foreach ($f in $folders) {
    $src = Join-Path $Source $f.Path
    if (-not (Test-Path -LiteralPath $src)) { continue }
    $dst = Join-Path $SdRoot (Join-Path 'nfsu2data' $f.Path)
    if ($PSCmdlet.ShouldProcess($dst, 'mirror folder')) {
        # /E all subdirectories, /XO skip files the card already has newer,
        # /R:2 /W:2 so a flaky reader does not hang the run.
        robocopy $src $dst /E /XO /R:2 /W:2 /NJH /NJS /NDL /NP | Out-Null
        if ($LASTEXITCODE -ge 8) { throw "robocopy failed on $($f.Path) ($LASTEXITCODE)" }
        "copied  $($f.Path)"
    }
}

foreach ($f in $files) {
    $src = Join-Path $Source $f.Path
    if (-not (Test-Path -LiteralPath $src)) { continue }
    $dstDir = Join-Path $SdRoot (Join-Path 'nfsu2data' (Split-Path $f.Path -Parent))
    # As robocopy's /XO does for the folders: a file the card already has at
    # the same size and no older is skipped (WORLD.NFSZ alone is 144 MB).
    $dstFile = Join-Path $dstDir (Split-Path $f.Path -Leaf)
    if (Test-Path -LiteralPath $dstFile) {
        $s = Get-Item -LiteralPath $src
        $d = Get-Item -LiteralPath $dstFile
        if ($s.Length -eq $d.Length -and $s.LastWriteTimeUtc -le $d.LastWriteTimeUtc) {
            "current $($f.Path)"
            continue
        }
    }
    if ($PSCmdlet.ShouldProcess($dstFile, 'copy file')) {
        if (-not (Test-Path -LiteralPath $dstDir)) {
            New-Item -ItemType Directory -Path $dstDir -Force | Out-Null
        }
        Copy-Item -LiteralPath $src -Destination $dstDir -Force
        "copied  $($f.Path)"
    }
}

if (Test-Path -LiteralPath $Build) {
    $dst = Join-Path $SdRoot '3ds'
    if ($PSCmdlet.ShouldProcess((Join-Path $dst 'NFSU2-3DS.3dsx'), 'copy build')) {
        if (-not (Test-Path -LiteralPath $dst)) {
            New-Item -ItemType Directory -Path $dst -Force | Out-Null
        }
        Copy-Item -LiteralPath $Build -Destination $dst -Force
        "copied  NFSU2-3DS.3dsx -> 3ds\"
    }
} else {
    Write-Warning "no .3dsx at $Build -- build it first."
}

""
"Done. Launch the .3dsx from the Homebrew Launcher; a .cia has to be installed"
"first and goes stale against the card's data."
