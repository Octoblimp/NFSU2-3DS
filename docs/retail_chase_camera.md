# Retail drive camera -- RE notes

GameCube `Speed.elf` (unstripped) unless marked PC. The port's version is
`worldCamera()` in `3DS Files/source/main.c`, with the tables beside it.

## Where the POVs come from

- Car::GetPov(type) 0x80224160: CarTypeInfo (Car+0xC) +0x720 = CameraInfo; GetPov(type)
  falls back to type 0.
- CameraInfo::GetPov 0x8005FCE8: view mode 3 -> count at +4, POVs at +0x70; else count
  at +0, POVs at +0x10. Each POV is 0x10 bytes: s16 type, u16 angle (bAngle), float x
  (+4), float z (+8), float y (+0xC).
- GetEyeOffsetAndAngle 0x80071508: eye offset = (pov+4, pov+0xC, pov+8), i.e. (x, y, z)
  with x along the car's forward (negative = behind); angle = pov+2. Replay and
  birds-eye special cases use the tables at 0x803A3448 / 0x803A369C / 0x803A36AC.
- PC CarTypeInfo (GLOBALB chunk 0x34600, stride 0x890) carries the same CameraInfo at
  +0x720. PEUGOT, set A (+0x730): type 0 (-5.8, z 1.7), type 1 (-4.7, z 1.7), type 2
  bumper (1.016, 1.09, angle 0x1F4), type 3 (-0.195, 0.805, y -0.44), type 4 (0.8, 1.2),
  type 5 (-5.8, z 3.45, angle 0xBE0). Set B (+0x790, view mode 3): type 0 (-8.2, 1.85),
  type 1 (-6.8, 1.7), ...

## POV types and the Camera option

| POV type | view | outside? | FOV (DriveCameraFovTable 0x80354B60) |
|---|---|---|---|
| 0 | far chase | yes | 0x32DC (71.5 deg) |
| 1 | near chase | yes | 0x32DC |
| 2 | bumper | no | 0x4000 (90 deg) |
| 3 | in car | no | 0x4000 |
| 4 | hood | no | 0x4000 |
| 5 | high chase | yes | 0x32DC |

- OutsidePovType 0x8005FD60: types 0, 1 and 5.
- The FOV is a horizontal angle. PC SPEED2.EXE carries the same u16 table at 0x7EB5B4.
- GetPOVTypeFromPlayerCamera 0x801309D0 maps the PlayerSettings camera (GC profile
  0x803BC210 + idx*0x40, +0xC4) to a POV type: 0 Bumper -> 2, 1 Hood -> 4, 2 Close -> 1,
  3 Far -> 0, 4 Super Far -> 5, anything else -> 0. Type 3 (in car) is never chosen
  from the options.
- **Change Camera** (JOY_EVENT_CAMERA_POV_CHANGE) in Player::RacingJoyHandler (event
  0x7D, 0x8021AA90) calls Player::CycleDriveCameraPovType(1) (0x80214EB8):
  PlayerSettings::ScrollDriveCam (0x80130B04) steps the camera up one and wraps past
  the last to 0 (or down one for -1), then the mover takes the new POV if the car has
  one (CameraInfo::GetPov, SetCameraMoverPovType 0x80214FC4). The last camera is 3
  (Far) unless g_tweakIsDriftRace, the word at 0x8040CA84, or the word at 0x8040CA40
  minus 0x104E <= 5 -- then 4 (Super Far). 0x8040CA40 is TheRaceParameters, so that
  word is its +0x00, the race's track number (CalcRaceTypeForHighScores 0x801322FC hands
  the same word to GetTrackInfo): tracks 4174..4179. 0x8040CA84 is TheRaceParameters+0x44,
  a field not decoded yet. The
  handler also wants: not ShowcaseState::IsInPreviewMode, a delay since Player+0x98,
  Player+0x218 in {2, 3}, and a car at Player+0x224 with +8 == 1 and +0x10 != 0.

## CubicCameraMover

- **The smoothing data.** aCubicPovTables (0x803A36BC, .bss) is built by the static
  initialiser at 0x8007A588: one tTable per POV type, count 2, SetMinMax(0, 1) (f31 =
  0.0 at 0x8031C5E8, f29 = 1.0 at 0x8031C628). Data by type: 0 0x80354BAC, 1 0x80354E6C,
  2 0x80354FCC, 3 0x8035528C, 4 0x8035512C, 5 0x80354D0C (type 5 holds type 0's
  numbers). Update reads the table at the fixed 0.5 (0x8031C0D0), so each type's data
  is the average of its two entries (Blend 0x800703BC).
- **CubicPovData** (0xB0 bytes): 4 durations, then 10 vec4: upGain, upMin, upMax,
  eyeGain, eyeMin, eyeMax, lookGain, lookMin, lookMax, fwdDur. Types 2 and 3 are all
  zero but for the third duration (1.0 / 0.5).
- **Update 0x80071D3C.** The table index is the POV type (+0x48 -> +0x4C). Bumper and
  hood (2 / 4) fall back to index 0 under HighliteMode, CameraWouldBeInCar, a car+0x30
  virtual, or TopologyCoordinate::ConstrainToTopology. +0x58 is a snap flag, set when
  the index changes.
- **SetForward 0x80071114.** The chase types blend the car's velocity (car+0x68) and
  its forward (car+0x88) by Graph gDriftSpeed (0 at 8 m/s, 1 at 13). The other types
  aim the forward cubic at the car's forward (+0x88) and snap it. (Earlier notes had
  +0x68 / +0x88 the other way round.)
- **SetDesired 0x80071674.** Outside: look base = (0, 0, tan(angle) * eye.x + eye.z).
  Inside: look base = (eye.x + 1.0, 0, eye.z - tan(angle)). The FOV cubic aims at the
  type's DriveCameraFovTable entry. For types 2 / 3, or with the +0x58 flag, every cubic
  is snapped each call (fwd, fov, eye / look / up at +0x44 / +0x34 / +0x38 / +0x3C).
  HighliteMode scales the eye offset by 1.3 (0x8031C0A4; hydraulics / showcase).
- **Inside views, in Update:** Camera+0x280 = the car for types 2 / 3, which
  ShouldCarBeRendered (0x80236C48) uses to skip that car's body, neon, reflections and
  flares; its lights (ShouldCarLightsBeRendered) and shadow still draw. The hood view
  (4) keeps the car. The up vector is the car's up (car+0xE8) instead of (upCubic.x,
  upCubic.y, 1). MinGapTopology / MinGapCars run for the outside views only (cr4 =
  outside at 0x80072444, `beq cr4` at 0x80072624).

## Not taken by the port (yet)

- Camera+0xBC near plane: 0.4 for in car (type 3), else 0.5. Type 3 is unreachable
  from the options.
- TerrainVelocityNoise 0x8006A460 (PovVelocityNoiseScale / PovTerrainNoiseScale are
  all 1.0).
- MaybePullBackToSeeChaseCar 0x80071C10.
- The bumper / hood fallback conditions above (none of them hold in the port).
- Super Far (camera 4): the drift race, tracks 4174..4179 and TheRaceParameters+0x44.
- Look Back (JOY_EVENT_CAMERA_LOOKBACK, SetLookBack 0x80070DF0).
