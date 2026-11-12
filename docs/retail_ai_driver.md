# Retail AI driver (GameCube Speed.elf) -- RE notes for TASKS.md #221

Recovered 2026-10-23 for the AI-opponent port. Source of truth is the GameCube
build, decompiled with Ghidra; the PC build has the same classes.

`opponent.c` already ports the *roster* side of #221 (`AddCareerEventAIOpponents`,
`PickRandomCarsOfSomeSort`) and main.c grid-places and draws the field. What this
file covers is the part that was missing: what makes an opponent *drive*.
"The port" at the end says what is PORT and what is faithful.

## Where things are

- Base registers: r13 = 0x80474DE0, r2 = 0x80484DE0.
  `tools/re/r13.py -0x53ec ...` resolves any `unaff_r13 + -0xNNNN` the decompiler
  prints, and shows the word/float stored there.
- Decompiles must come from **SpeedPS.elf** (project `NFSU2PS`), the paired-singles
  patched copy from `tools/re/gekko_patch.py`. On the stock `Speed.elf` every
  PsychoDriver function decompiles to `halt_baddata()` -- Ghidra's PowerPC decoder
  chokes on the `psq_st` in the prologue.
  `tools/re/ghidra_decomp.ps1 -Program SpeedPS.elf -Addrs 0x...,0x... -Out <file>`
  (the `-Project` default follows the `*PS.elf` name).
- `tools/re/physics/tabinit.py <start> <end>`: dump `Table` objects a static
  initialiser fills. The PsychoDriver tables come from the unnamed initialiser at
  **0x80041c1c** (Ghidra mis-attributes it to `RecordSignpostDecision__...`; start
  the trace at the real prologue or the counts come back `None`).
- `tools/re/graphinit.py <start> <end>`: the same for `Graph` objects, whose point
  arrays live in .bss and only exist in the init code's `stfs`/`stfsu` stream.

## Class map

| Class | What it is |
| --- | --- |
| `PsychoDriver` | the AI *driver*: turns a `DriveTarget` into steering + throttle + brake. `RealDriver` is the human. |
| `DriveTarget` | the line to follow: a resampled point list with a speed per point. |
| `DrivingChoice` | picks which driving line (and lateral offset) to build the target from. |
| `FuturePoints` | the window of upcoming `TrackRoutePoint`s, with driving lines interpolated and speeds scrubbed. |
| `AICarController` | per-frame brain: `Think` -> `FindWhereIAm` -> goal -> action -> `GeneratePointsToFulfillAction` -> `ExecuteCurrentAction`. |
| `RaceCarController` | the racer specialisation (nitrous, rubber-band boost, pathfinding). |
| `AIGoal` | what the car is trying to do; `SetDesiredSpeedAsRacer` / `ChooseNewActionAsRacer` are the race ones. |

Key addresses:

```
8001eff4  PsychoDriver::PsychoDriver(Car*)
8001f7dc  PsychoDriver::GetSteeringError(float*, float*, float*)
8001fa8c  PsychoDriver::GetMovementIndex()
8001faf0  PsychoDriver::RecordWheelSlip(float)
8001fbec  PsychoDriver::Drive(float)                     <- the control loop
80020f0c  PsychoDriver::CalcSteerToPoint()
800210dc  PsychoDriver::AdaptiveControlIsDesired()
80021114  PsychoDriver::CalcThrottleAndBrake(float*, float*, int, float)
80021898  PsychoDriver::TractionControlAllowed()
800219ec  PsychoDriver::GetForceYaw()
800070d4  AICarController::Think(float)
80007db0  AICarController::FindWhereIAm(bool)
80008714  AICarController::ExecuteCurrentAction()
800089d8  AICarController::GetCurrentDrivingLineSpeed()
8000b644  AIGoal::SetDesiredSpeedAsRacer(DrivingInformation*)
8000be48  AIGoal::ChooseNewActionAsRacer(DrivingInformation*, AIActionType, int)
80027bdc  RaceCarController::DecideCurrentGoal()
800292b0  RaceCarController::DecideBoostFactor()
80029954  RaceCarController::CalculateDistanceAndPositionBoost(RacingCar*, RacingCar*, float*, float*)
8001cfbc  DrivingChoice::BuildDriveTarget(DriveTarget*, DrivingInformation*)
8001d500  DrivingChoice::FillUpDriveTarget(...)
8001ade0  DriveTarget::InterpolatePoint(Car*, float, bVector3*)
8001b1c8  DriveTarget::GetLateralOffset(bVector3*)
800386ac  FuturePoints::InterpolateDrivingLineSpeed(int, float, Car*)
```

## Units

Speeds are **m/s** throughout (the engine compares against 26.8216 = 60 mph).
Angles are radians in the maths and bWare `Us` fixed-point angles across the
`bATan` / `RadianWrap` boundary. Tables index on speed in m/s unless noted.

## `Table` / `Graph`

- `Table { int n; float min, max, indexMul; float *data; }`, linear interpolation,
  clamped at both ends -- the same `TableBase` as `docs/retail_car_physics.md`.
- `Graph { bVector2 *pts; int n; }`, `GetValue(x)` (0x801A5214) walks the points
  linearly and clamps outside the first and last x.

## The movement index

`GetMovementIndex` (0x8001fa8c) picks which column of every 7-wide weight array
the driver uses this frame -- the driving "style". Indices 3 and 4 are the drift /
drag styles, which is why `CalcSteerToPoint` branches on `index - 3u < 2`.

Every 7-float array below is indexed by it:

```
                                     [0]    [1]   [2]   [3]   [4]   [5]  [6]
PsychoSteeringProportionalWeight      0.9   1.0   0.8   0.7   0.85  0.5  0.0
PsychoSteeringIntegralWeight          0.2   0.0   0.1   1.42  0.79  1.0  0.0
PsychoSteeringDerivativeWeight        0.3   0.08  0.18  0.0   0.12  0.0  0.0
PsychoDriverSteerToPointSecondsAhead  0.55  0.4   0.35  0.55  0.9   0.4  0.0
PsychoDriverSteerToPointMinDistanceAhead
                                     15.0  15.0  15.0   5.0   9.0  15.0  0.0
fThrottlePAccelerating                0.1   0.1   0.1   0.1   0.005 0.1  0.1
fThrottleIAccelerating                0.02  0.02  0.02  0.02  0.005 0.02 0.02
fThrottleDAccelerating                0.02  0.02  0.01  0.01  0.01  0.01 0.01
fThrottlePDecelerating                0.1   0.1   0.1   0.1   0.005 0.1  0.1
fThrottleIDecelerating                0.02  0.02  0.02  0.02  0.0   0.02 0.02
fThrottleDDecelerating                0.02  0.02  0.01  0.01  0.008 0.01 0.01
```

## PsychoDriver tables

From `tabinit.py 0x80041c1c 0x80042500`. All index on speed in m/s over
[0, 160] in 10 steps unless the range says otherwise.

```
PsychoThrottleSpeedTable      30, 15, 7.5, 6, 6, 6, 6, 6, 6, 6
PidProportionalTable          0.328, 0.22, 0.148, 0.115, 0.09,
                              0.074, 0.057, 0.049, 0.043, 0.04
PidIntegralTable              0.21, 0.244, 0.267, 0.29, 0.305,
                              0.321, 0.328, 0.336, 0.341, 0.344
PidDerivativeTable            0, 0.07583, 0.07583, 0.07383, 0.0604,
                              0.04698, 0.04698, 0.04027, 0.04027, 0.04027
PidSteerToTable               22.8972, 22.8972, 24, 27.9, 33,
                              39, 45.8, 49.4, 53.1, 55.9
PsychoSteeringRangeTable      45, 30, 5.5, 5, 3, 2, 2, 2, 2, 2
PsychoSteeringSpeedVsCarSpeedTable
                              4, 3, 2, 1, 0.45, 0.14, 0.1, 0.1, 0.1, 0.1
PsychoSteeringSpeedVsAngleErrorTable   n=20 over [0, 95]:  all 1.0
PsychoSteeringCurveTable               n=6  over [0, 1]:
                              0, 0.1, 0.24, 0.53, 0.79, 1
PidSteerToScaleTableMellowPhysics      n=2 over [0, 3]:  0.6, 1.1
PidSteerToScaleTableRealPhysics        n=2 over [0, 3]:  1.3, 1.1
```

`PidSteerToScaleTable*` is indexed by the **car class** (`RidePhysicsInfo::
GetCarClass`), not by speed. Its backing arrays are the .sdata symbols
`PidSteerToScaleDataMellowPhysics` / `...RealPhysics`; `fPidSteerToScale` = 1.0
is the global multiplier.

## PsychoDriver graphs

From `graphinit.py 0x80041c1c 0x80042500`.

```
HeadingErrorModelGraph   (0,0) (1,-0.5) (3,-1.5) (5,-2.5) (10,-5)
                         (50,-5) (60,-5) (70,-5) (80,-5) (90,-5)
VelocityErrorModelGraph  (-20,100) (-10,10) (-5,7) (-2,3) (0,0)
                         (2,0) (10,-3) (20,-10) (30,-100)
PidHeadingBlendGraph     (5, 0)  (10, 1)
PidBodyBlendGraph        (0.1745329, 0.3)  (0.5235988, 1.0)     # 10deg, 30deg
DragLateralSpeedGraph    (0, 0.75)  (100, 0.25)
DriveTargetAccelerationBlendingGraph
                         (0,0.75) (0.2,0.6) (0.4,0.5) (0.6,0.4) (0.8,0.3)
                         (1.0,0.2) (1.2,0.1) (1.4,0.1) (1.6,0.1) (1.8,0.1) (2.0,0)
OffsetPursuitDistanceVsSpeedFactorGraph
                         (0,1.1) (20,1.0) (50,0.8) (100,0.5) (150,0.1)
PostCollisionSteerReductionTable
                         (0,0.2) (0.2,0.5) (0.5,0.7) (0.7,1.0)
```

`PidHeadingBlendGraph` is the one `GetSteeringError` reads, keyed on **speed**:
below 5 m/s the driver steers purely on heading error; above 10 m/s purely on
velocity-direction error; in between it lerps. (`PidBodyBlendGraph` is keyed on
an angle and belongs to the body-slip path.)

## `CalcSteerToPoint` (0x80020f0c)

Picks the look-ahead point on the drive target.

```
i = GetMovementIndex()
if (i == 3 || i == 4)                      # drift / drag styles
    ahead = max(PsychoDriverSteerToPointMinDistanceAhead[i],
                speed * PsychoDriverSteerToPointSecondsAhead[i])
else
    ahead = PidSteerToTable(speed)
          * PidSteerToScaleTable{Real|Mellow}Physics(carClass)
          # Real when CarState+0x28 == 1, Mellow otherwise

if (no drive target)                        # *this < 1
    if (speed >= 1.0)  p = SteerStraightVelocity3D(car, ahead)
    else               p = SteerStraightVector3D (car, ahead)
else
    p = DriveTarget::InterpolatePoint(car, ahead)
steerToPoint = p.xz
```

`speed` here is `CarState+0x43c` (speed along the body axis), while
`CarState+0x440` -- the one the throttle code and the blend graphs use -- is the
signed forward speed.

## `GetSteeringError` (0x8001f7dc)

Returns three errors and the bearing to the steer-to point.

```
bearing    = bATan(steerTo.x - pos.x, steerTo.z - pos.z)
headingErr = RadianWrap(carYaw - bearing)                   # out param 1

blend = PidHeadingBlendGraph(speed)
if (blend <= 0)  blendedErr = headingErr
else             blendedErr = blend * RadianWrap(bATan(vel.x, vel.z) - bearing)
                            + (1 - blend) * headingErr      # out param 2

lateralErr = RadianWrap(bATan(1.0, DriveTarget::GetLateralOffset(pos)))
                                                            # out param 3
```

## `CalcThrottleAndBrake` (0x80021114)

```
if (not in a goal that forces throttle) and |speed| < 5:
    # crawl: full throttle or full brake toward the desired speed
    if (|speed| - desired| <= 1)  throttle = 0, brake = 0
    elif (|speed| <= desired)     throttle = 0, brake = 1    # see note
    else                          throttle = 1, brake = 0
    goto clamp

if (adaptive control is on and |speed| > 30 and not replay-ish):
    AdaptivePIDControllerSimple::Update(|speed|, desired,
        VelocityErrorModelGraph(desired - |speed|), dt)
    out = GetOutput()
    if (desired < 10)  reset the controller's integral terms
else:
    # fixed-weight PID on the speed error
    i = movementIndex
    if (|speed| + 2 <= desired)      # accelerating
        P,I,D = fThrottle{P,I,D}Accelerating[i];  lo = -1,  hi = throttleMax
    else                             # decelerating
        P,I,D = fThrottle{P,I,D}Decelerating[i];  lo = -1,  hi = 0
    out = I*avgIntegral + P*lastError + D*clamp(deriv, -10, +10)
            # fThrottleDerivativeClamp = 10.0

# rate limit: the control can move by at most this much per second
lim = PsychoThrottleSpeedTable(desired) * dt
out = clamp(out, -lim, +lim)
cmd = clamp(prevThrottle - prevBrake + out, -1, +1)

if (TractionControlAllowed()) {
    # slip-based ceiling; different gains going forwards vs backwards
    if (cmd < 0)  a,b,c =  0.02,  0.02,  0.005
    else          a,b,c = -0.02, -0.2,  -0.005
    tc = 1 + a*slipAvg + b*lastSlip + c*slipDeriv
    tc = max(tc, 0)
}
if (cmd >= 0) { throttle = min(cmd, tc) if TC else cmd; brake = 0; }
else          { throttle = 0; brake = min(-cmd, tc) if TC else -cmd; }
throttle = clamp(throttle, 0, throttleMax)   # PsychoDriver+0x20c
brake    = clamp(brake,    0, 1)
```

The drag-race yaw wobble (`bSin(2*yaw + 90deg)` * 0.5) only applies in the
drag-race goal, where `CarState+0x1c` is set.

## Difficulty

`DifficultyInfo` is chunk **0x34b00** in `GLOBAL/GLOBALB.BUN` (file offset
7670144, size 0xAE0). The ten 0x68-byte skill records start at payload+0x524 and
scale the AI's speed, catch-up and nitrous use. The port already loads GLOBALB.BUN,
so no new file is needed.

## AI driving-line data

`TRACKS/ROUTES<track>/Routes<id><F|B>.bin`, chunk **0x34121** = `TrackRoute`:
an 0x8C header then `numPoints` x 0x38 `TrackRoutePoint`. Each point carries ten
driving-line lateral offsets and ten target speeds (u8, m/s). Chunks 0x34122 and
0x34123 hold the companion tables. `tools/routes_dump.py` decodes the whole thing
in Python and names every field with the accessor address it came from.

These files are **not** on the emulated SD yet: `.../sdmc/nfsu2data/TRACKS/
ROUTESL4RA` holds only `Paths*.bin`, `PathsFreeRoam.bin` and
`TrackPosMarkersAll.bin`. One track's forward file is ~155 KB; all tracks ~18.7 MB.
`tools/copy_to_sd.ps1` already mirrors the whole `ROUTESL4RA` folder, so anything
added to the Azahar sdmc reaches the card automatically.

## The port

Retail reaches `PsychoDriver::Drive` through `FuturePoints` ->
`DrivingChoice::BuildDriveTarget` -> `DriveTarget`: a graph walk over
`TrackRoute`/`Signpost` topology with spline interpolation, corner smoothing,
speed scrubbing, `NearbyAvoidableList`, `CollisionPredictor` and action scoring.
That is several hundred functions and a world representation the port does not
have -- the port's world is the `RaceRoute` splines from `Paths*.bin`.

So the split is:

- **Faithful**: `PsychoDriver` itself -- `GetMovementIndex`, `CalcSteerToPoint`,
  `GetSteeringError`, `CalcThrottleAndBrake`, the rate limit, traction control,
  and every table, graph and weight array above, with retail's values.
- **PORT**: the drive target. Instead of rebuilding `FuturePoints`, the port
  samples the ported `RaceRoute` spline (`race_route_sample`, which already
  takes a lateral offset) at the look-ahead distance `CalcSteerToPoint` asks for,
  and takes the desired speed from the retail `TrackRoutePoint` driving-line
  speeds. That is the same pair of inputs retail's pipeline ultimately produces,
  from the same retail data, without the topology machinery.
- **Ported over that line** (TASKS.md #280, `opponent.c`): the racer's
  `DrivingChoice`. `AIActionJustDrive::TryAndDriveBestLine` (0x8000EF34) takes
  a car back to line 0 at `DrivingChoiceMinLateralSpeed` (0.05 m across per m
  along) once no nearby car is within 0.75 s of meeting it and the way back is
  clear for 3 s; `AIActionAvoidAvoidable` (score 0x80012724, points 0x80012A9C)
  takes over when the current choice hits a car within 3 s, and
  `ChooseOffsetFromDrivingLine` (0x8001E09C) steps out from the current offset
  in (road width / 10, at least 1 m) steps, at 0.05 / 0.10 / 0.15 lateral
  speed, the side away from the car first, keeping a new choice 0.5 s;
  `ChangeCurrentL` walks the drive target's offset toward the choice.
  `DoWeCareAboutThisAvoidable` (0x80017D88) drops a car once it is wholly behind
  (the player 0.5 m sooner). **PORT** within it: `CollisionPredictor` sweeps
  boxes down each car's path along the line (20 samples over 3 s, footprint
  along/across the line, 1 m extra side clearance for the port driver's
  tracking); a blocked car follows the blocker the way
  `SetDesiredSpeedSlowerThanThisAvoidable` (0x8000DDE8) does, since retail's own
  no-gap speed (0x80012BBC, from `+0x208`) is not decoded far enough; and the
  grid holds its lanes through the start grace period instead of all making for
  the line at once.
- **PORT**: the steering's self-tuning controller. `Drive` (0x8001FBEC) steers
  every circuit and sprint racer through `AdaptivePIDControllerComplicated` at
  PsychoDriver +0x264, at any speed; only the drift and drag styles (movement
  index 3 / 4), movement mode 4 and index 5 take the fixed
  `PsychoSteering{Proportional,Integral,Derivative}Weight[movementIndex]`. The
  controller is ported (`aidriver.c`: `AdaptivePIDControllerBase::UpdateBase`
  0x80030444 with rule 0, `GetOutput` 0x80030634, the sensitivity 0x80030878,
  `AverageWindow` 0x801A58A8; Drive's per-frame limits, the MellowMover's P
  0.15-0.4 / I 0-0.1 / D 0.04-0.06, Street X's and the SUV's; the
  `HeadingErrorModelGraph` target; the speed tables below 10 m/s forward), and
  so is Drive's airborne branch (no front wheel down: hold the wheel 0.3 s, then
  steer down the direction of travel, 5 degrees at most on the MellowMover). But
  the controller is left off (`kSteerAdaptive`): it is tuned against retail's
  own drive target, and the port's -- the line at the look-ahead, cut back to
  as little as 8 m in a tight bend (`opponent.c`'s `chordAhead`) -- needs more
  proportional gain than the MellowMover's 0.4. Over every track (`airace_sim.py
  --all --mode 1`) the controller ran the field 3,241 s off its line with 87
  unsticks, against 121 s and 13 on the fixed weights, so every racer steers on
  the fixed weights.
- **PORT**: the throttle's self-tuning PID (`AdaptivePIDControllerSimple`, which
  `CalcThrottleAndBrake` runs above 30 m/s when `AdaptiveControlIsDesired`) is
  not ported. The port takes retail's own fixed-weight branch
  (`fThrottle{P,I,D}{Ac,De}celerating[movementIndex]`) at every speed.
