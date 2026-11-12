# Retail car physics (GameCube Speed.elf) -- RE notes for TASKS.md #222

Ported to `carsim.c` on 2026-10-22; "The port" at the end says what is PORT.

Recovered 2026-10-22 for the handling/collision port ("Really gotta fix the cars
handling and collision and whatnot lol", CONTINUE.MD I1). Source of truth is the
GameCube build, decompiled with Ghidra; the PC car record carries the same data.

## Where things are

- Decompiles: `extracted/re_physics/`
  - `phys_ps.c` / `phys2_ps.c`: Ghidra output on `SpeedPS.elf` (the paired-singles
    patched copy made by `tools/re/gekko_patch.py`, run via `tools/re/ghidra_ps.ps1`).
    Paired-single vector maths is patched to one lane, so CHECK vector code against
    the raw disassembly (`tools/gc_disasm.py`).
  - `phys_a.c` / `phys2_a.c`: the same, annotated by `tools/re/physics/annot.py`
    (every `DAT_` and `r13`/`r2` relative load resolved to its symbol and value).
  - `phys_index.txt`, `phys_addrs.txt`: function list and addresses.
- Tools: `tools/re/physics/`
  - `fn.sh <file> <name>...`: print the decompiled functions whose header matches.
  - `tables.py`, `tabinit.py`: dump `TableBase` tables (incl. ones filled by the
    static initialiser at 0x801d56c0).
  - `findref.py`, `stwscan.py`: find references / stores with a given displacement.
  - `dumpcti.py`: dump a car's CarTypeInfo physics block.
  - `terr.py`: dump the terrain-type table at 0x8036fe2c.
- Base registers: r13 = 0x80474DE0, r2 = 0x80484DE0.

## Units and conventions

- Tonnes, kN, kN*m, m, s, rad. Gear enum: 0 = R, 1 = N, 2 = 1st.
- Wheel order: FL, FR, RR, RL. The record's y axis is LEFT-positive.
- bWare matrices are row-vector: rows are the basis axes;
  `out = v.x*row0 + v.y*row1 + v.z*row2`. Implement `w = R * Iinv_body * R^T * L`
  physically, not literally from the decompile's index order.
- `TableBase { int n; float x0, x1, k = (n-1)/(x1-x0); float *data; }`.
  `GetValue(x)`: t = (x-x0)*k, i = int(t); t < 0 -> d[0]; i >= n-1 -> d[n-1]; else lerp.
- Control mode `CarState+0x434` comes from the player's **Stability Control** option:
  PlayerSettings+0x2a -> Player+0x21 -> Car+0x474. `Default__14PlayerSettings`
  (0x80130a98) writes 1. POHandling draws 0 = "On" (hash 0x63c), 1 = "Off" (0xccfa),
  title hash 0xdf06ed22. **The player defaults to mode 1 (Stability Control Off).**

## Data: CarPhysicsInfo

`CarPhysicsInfo` (0x2B0 bytes) = `CarTypeInfo + 0x110` (RebuildPhysicsInfo 0x801C89A8),
then upgrades are applied (BuildCarPhysicsInfo 0x801C4D44: Weight / DriveTrain / Nitrous /
Engine / Turbo / Suspension / Brakes / ECU / Tires packages, Aero,
BuildInertiaTensor(cpi+0x110)), then Junkman parts and custom tunings.

The PC car record (stride 0x890, GLOBALB chunk 0x34600) is the same CarTypeInfo.
Offsets below: CPI offset [CTI offset = CPI + 0x110].

| CPI | CTI | Contents |
|---|---|---|
| +0x000 | 0x110 | bVector3 CG offset -> RigidBody+0x1d8 |
| +0x010 | 0x120 | WheelInfo[4], stride 0x30 (FL, FR, RR, RL); +0x10 radius, +0x18 index |
| +0x0D0 | 0x1E0 | TireInfo[2], stride 0x20 (front, rear) |
| +0x110 | 0x220 | RigidBodyInfo: mass (t) at +0; inertia 4x4 at +0x10 |
| +0x160 | 0x270 | Izz scale (RigidBodyInfo+0x50) |
| +0x170 | 0x280 | SuspensionInfo[2], stride 0x20 |
| +0x1B0 | 0x2C0 | DriveTrainInfo |
| +0x1F0 | 0x300 | EngineInfo (EI+0xC at CTI 0x30C = **engine inertia**) |
| +0x260 | 0x370 | AeroInfo {Cd, A1 front, A2 rear, downforce-load factor} |
| +0x270 | 0x380 | ControlInfo {steer scale, brake k, ebrake k, brake bias (CPI+0x27c)} |
| +0x280/+0x284 | 0x390/0x394 | YawFrictionBoost p4 / p5 |
| +0x288 | 0x398 | yaw torque clamp, control mode 0 (17) |
| +0x28c | 0x39C | yaw torque clamp, mode != 0 (0 = off) |
| +0x290 | 0x3A0 | speed governor, mph -> RigidBody+0x1ec |
| +0x2AC | 0x3BC | NOS |

`CTI+0xD8` (s8) -> RigidBody+0x148 and Engine+0x74 (the "1.05 torque" flag).
`GetMiscCarTunings(cpi)` -> MiscCarTunings (DriveTrain ctor).

**cardb.h's `dragCoef` read from +0x30C is wrong: +0x30C is the engine inertia**
(UpdateCar divides SmoothRPMDecel by it).

### Sample values

- 350Z: tyre F 12, 1, 1, 1.68, 1.58 / R 12, 1, 1, 1.7, 1.585; mass 1.45 t;
  Izz scale 2.045; susp F 5.3, 50, 5.15, 6, 21, 0.15, -0.15, 0.045 /
  R 5.1, 43, 5.4, 5.7, 23, 0.15, -0.15, 0.05; DTI 0.8, 500, 3.54, 3.54, split 1,
  0.04, 6 gears, 2; EI 800, 7000, 7500, 0.00035; engine braking 0.17, 0.29, 0.2;
  aero 0.00042, 0.145, 0.15, 0.0275; control 1.01, 0.495, 0.4, 0.47, 0, 2.5, 17, 0;
  governor 155 mph.
- CIVIC: FWD (split 0), governor 127 mph.

### TireInfo (0x20)

- +0 max slip angle in **degrees** (12). GuessGroundForce compares it with the slip
  angle in **radians**: a retail quirk, port it as is.
- +4 load normaliser (1), +8 cornering scale (1), +0xC static mu (FinalizeGroundForce
  cap), +0x10 dynamic mu (sliding force).

### SuspensionInfo (0x20)

[0] progressive k2, [1] spring k, [2] bump damping (xdot > 0), [3] rebound damping,
[4] anti-roll, [5] max compression 0.15 (bottom-out), [6] min / droop -0.15,
[7] ride height?

### DriveTrainInfo

[0] launch fraction, +8 front final drive, +0xC rear final drive, +0x10 torque split
(1 = RWD, 0 = FWD), +0x14 clutch k, +0x18 number of gears, +0x1c clutch speed,
+0x20 ratios[gear].

### EngineInfo

[0] idle rpm, [1] redline, [2] limiter, [3] (+0xC) inertia. Tables: torque n9 over
[EI0, EI2] at EI+0x10; engine braking n3 at EI+0x58; turbo n9 at EI+0x34 (pressure
only, not torque); NOS n9 over [EI0, EI1].

## Static tables (initialiser 0x801d56c0)

- AngularMomentumDamping n6 [0,10]: .05 .05 .25 .7 2 5
- MotionDampingTable n8 [0,1]: 1.5 1 .6 .22 .06 .03 .015 0
- CorneringForce n7 [0, 0.22340 rad]: 0 .4 .8 1.2 1.6 1.75 1.65 (unused: UseNewTireForces = 1)
- LoadSensitivityTable[7] (Zero..TwelveDegree), each n6 over load [0,10]:
  - 0: 0 0 0 0 0 0
  - 2: 0 1.2 2.3 3 3 2.8
  - 4: 0 1.7 3.2 4.3 5.1 5.2
  - 6: 0 1.8 3.5 4.9 5.8 6.1
  - 8: 0 1.83 3.6 5 5.96 6.4
  - 10: 0 1.86 3.7 5.1 6.13 6.7
  - 12: 0 1.9 3.8 5.2 6.3 7.1
- EBrakeSettings [0, 0, 1, 1]; ClutchSpeed [-3, 0, 1.5, 0].
- YawFrictionBoost constants: YawEBrakeThreshold 0.5, YawAngleThreshold 20 deg,
  LowSpeedYawBoost 0, HighSpeedYawBoost 1, LowSpeedSpeed 0, HighSpeedSpeed 30,
  MaxYawBonus 0.35.
- MiscCarTunings at 0x8035fd9c, stride 0x144; +0x10 is the 1st-gear upshift scale.
- Post-collision constants at 0x8033a79c (not 0x8034a79c): default 0, scale 0.00025, limit 4 s.

## The frame

### PhysicsMover (ctor 0x801BF960)

Layout: +0x00 CarDriver*, +0x04 vt CarMover, +0x08 PhysicsSystem (list head +0x10,
count +0x18, vt +0x1C), +0x20 RigidBody*, +0x24 Steering*, +0x28..0x34 Suspension*[4],
+0x38..0x44 Wheel*[4], +0x48 Engine*, +0x4C DriveTrain*, +0x50 Aerodynamics*,
+0x54 CarPhysicsInfo*, +0x58 CarState* (= Car+0x40), +0x5C Car*, +0x84 dyno flag,
+0xF8 average force, +0x108 average torque.

- Wheels / suspensions with i < 2 use Tire/Susp info [0], the rest [1].
- Front suspensions get the Steering. Anti-roll partners: 0 <-> 1, 2 <-> 3 (susp+0x18).
- RB+0x1ec = CPI+0x290; RB+0x148 = CTI+0xD8; Aero [2] = [3] = [10] = 1, [4] = [5] = 0.
- +0x6c = 1 (grip base), +0x70 = 1 (ebrake recovery).
- Then Calibrate, InitializeStateFromCar, ComputeAuxiliaries.
- Component order in the system: RigidBody, W0, S0, W1, S1, W2, S2, W3, S3, Engine,
  DriveTrain.

### Move (0x801C0458)

1. Zero CarState+0x3cc[4] and the 4 floats ending at CarState+0x334.
2. Calibrate; TuneBoostAndDampers; DelegateDriverInput; TuneWheelParams(dt); CalcBurnout.
3. n = int(dt / MaximumTimeStep 0.0044) + 1; h = dt / n.
4. n times: UglyTimestepHack = h; IntegrateEuler(h); accumulate RB+0x11c (force) and
   RB+0x12c (torque); orthonormalise the RB+0x28 matrix.
5. Average the accumulators over n; UpdateCar(dt); skids.

IntegrateEuler (0x801B0EDC): every component's derivative (vt+0x38) into one array;
for each component state[i] += h * deriv[i] (GetState vt+0x18, GetSize vt+0x10,
GetZeroIndices vt+0x48); then PhysicsSystem::ComputeAuxiliaries (each component vt+0x40),
BottomOut, ResolveForces__12PhysicsMover.

ResolveForces (0x801C1398): RigidBody::ResolveForces (empty); Aerodynamics::ResolveForces;
Suspension::ResolveForces(0) x4; IterateForces; Suspension::ResolveMoreForces x4;
JumpStabilizer; Car::MaybeForceYaw; DriftStabilizer (drag/drift only);
ResolveHydraulicForces; dyno.

### Calibrate (0x801BFFA0)

- FixCarCG only in wild mode (Car+0x38 == 1); then the inertia tensor.
- rearShare = (W0.x - cg.x) / ((W0.x - cg.x) - (W3.x - cg.x)) * 0.5; frontShare = 0.5 - rearShare.
- load = m*9.8 + AeroInfo[3] * GetDownforceAtSpeed(Car+0x43c) where
  GetDownforceAtSpeed(v) = Aero[10] * max(0, v) * (A1 + A2).
- Susp[0,1].Calibrate(load*front, CarState+0x3bc+4i); Susp[2,3].Calibrate(load*rear, ...).
  The ride-height argument is 0 for our purposes.

### TuneBoostAndDampers

- Mode Car+0x38: 1 = wild (gravity 11, throttle <= 0.4). 0/2: RB+0x13c = +0x140 = 1,
  gravity 9.8, Aero[10] = 1, Aero[11] = terrain drag average.
- SetZRotationDamping(1) in modes 0/2 unless Car+0x434 == 0. Car+0x434 is CarState+0x3f4,
  the wheels-down count SetWheelTouchingGround keeps (CarState sits at Car+0x40), so the
  damping is on whenever a wheel touches the ground -- not the control mode at
  CarState+0x434. The port had read it as the control mode, which left Stability
  Control On (mode 0) with no Z damping at all (fixed 2026-10-24).
- SetBoost(Car+0x478): Engine+0x90, RB+0x1f0, Aero[2], wheels+0x180 = boost;
  Aero[3] = 1 - Car+0x4dc.
- RB+0x1e8 yaw clamp = CPI+0x288 (17) if +0x434 == 0, else CPI+0x28c
  (or 20 when 0 and counter-yawing with |yaw| > 0.1).
- If split > 0.8: DT+0xec = split when mph >= 60 or airborne, else 0.65.

### DelegateDriverInput

- If not neutral and no burnout: if thr > brake && thr > ebrake, zero brake/ebrake
  when < 0.5; otherwise zero thr when < 0.5.
- thr <= 0.15 -> 0; thr > 0 && gear > N && rpm < 3000 -> max(0.5, thr).
- Steering.SetSteering(driver u16 +0x1c4); Engine+0x78 = thr;
  SetDesiredGear(driver desired, rpm); Aero[4] = thr; Aero[5] = wheelsOnGround * 0.25.

### TuneWheelParams(dt) (mode 1 = the player)

- grip = 1 * (1 - rain*0.025). (Mode 0 multiplies by NFSAdjust 0.85 outside drag/drift.)
- ebrake recovery r: +dt while ebrake < 0.5 (max 1), else -20*dt (min 0.5).
- Per wheel:
  - +0x174 = brake * (front 2(1 - bias), rear 2*bias), bias = CPI+0x27c (0.47).
    Burnout: rear = mph/20 when thr > 0.8, brake > 0.5, mph < 10.
  - +0x178 = ebrake * EBrakeSettings[i], +0.5 if > 0.2 (player: only when |yaw| > 0.3
    and mph < 80).
  - +0x17c: front = grip; rear = grip * (r * (YFB - 1) + 1), with
    YFB = YawFrictionBoost(yaw, +0x178, CarState+0x400, CPI+0x280, CPI+0x284,
    postColl = 0, mode = 1, &fwd.z).
  - +0x170 = 0 (damage wobble).

YawFrictionBoost (0x801C0378): *p = |*p|; base = *p + 1; if mode, p5 *= p6;
if ebrake > 0.5 && |yaw| < 20 deg return base;
bonus = min(0.35, |yaw| * (p4 + p5) * lerp(0..1 over speed 0..30)); return base + bonus.

### UpdateCar (0x801C1AC0)

RB position/matrix -> CarState; heading u16 CarState+0x3dc = bFastATan(fwd), +0x3de
slewed at 120 deg/s. Wheel data -> CarState +0x2e8 / +0x2f8 / +0x338 / +0x38c / +0x36c
/ +0x308 / +0x318. RPM smoothing: TestRPM 100, *9.5493 (rad/s -> rpm),
SmoothRPMDecel[upgrade] / EngineInfo+0xC, blend 0.55/0.45 -> CarState+0x410.
Slip / 6 clamped to 1 -> +0x3ec / +0x3f0.

### InitializeStateFromCar

RigidBody::SetState(CarState+0x18 pos, +0x48 matrix, +0x28 vel, +200 angvel);
Wheel SetState(CarState+0x2f8, +0x2e8); Engine+0x14 = rpm*0.10472;
DriveTrain +0x44 = 3, +0x38 = +0x3c = gear (CarState+0x404), +0x14 = 1, +0x58 = 0.

### BottomOut

MovementDousingAndRoofReset; Suspension::ApplyGroundPenaltyImpulse for each suspension
with x == info[5] (bottomed); TriggerBounce; re-run ComputeAuxiliaries with
UglyTimestepHack = 0.

### MovementDousingAndRoofReset

CheckDampingSuitability(prev): 0 leave it, 1 bleed it off, 2 hold it still.
1. thr > 0.3 && gear != N && wheelsDown (CarState+0x3f4) > 0 && attached (+0x3f8) > 2 -> 0.
2. |v| > 0.7 || |w| > 0.7 -> 0.
3. (|fwd.z| > 0.15 || |left.z| > 0.15) && wheelsDown > 0 -> 0.
4. !IsTouchingGround && prev != 2 -> 0 (touching = a wheel down, or any collision with
   the ground in it).
5. |v| > 0.25 || |w| > 0.25 -> 1, else 2.

Then:
- 1: P, L and every wheel's w *= 0.95; the roof timer clears if |v| > 0.8; gravity
  9.8 (WildCollisionGravity 11 when Car+0x38 == 1); returns 1.
- 0: the timer clears if |v| > 0.8; gravity as above; returns 0.
- 2: P = L = 0, and every wheel's w = 0 if a wheel is down. The timer runs while
  up.z < 0.1, or while GetDriverInfo()+4 == 1 (the player) and AreDriveWheelsOffGround;
  once it passes 1 s (> 4000 WorldTimer ticks, a tick is 0.00025 s) the car calls
  PleaseResetMeLater, unless IsFinishedRacing. Otherwise the timer clears. Gravity 0;
  returns 1.

## RigidBody (component 0)

Fields: +0x14 RigidBodyInfo*, +0x18 pos, +0x28 bMatrix4 orientation (rows = car axes
in world), +0x68 linear momentum P, +0x78 angular momentum L, +0x88 damping scalar,
+0x8c vel = P/m, +0x9c accel in g, +0xac angular velocity, +0xbc..0xf8 transform copy,
+0xfc force accumulator, +0x10c torque accumulator (copied to +0x11c / +0x12c and
zeroed in every ComputeAuxiliaries), +0x13c / +0x140 damping flags, +0x144 Z-rotation
damping flag, +0x148 CTI+0xD8, +0x14c gravity g, +0x150 mass, +0x154 1/mass,
+0x158 inv(I), +0x198 inv(I with Izz * info+0x50), +0x1d8 CG offset, +0x1e8 yaw
torque clamp, +0x1ec / +0x1f0 speed governor.

- ctor: +0x144 = 1, +0x13c = +0x140 = 1, +0x14c = 9.8, +0x1f0 = 1.
- State vector: pos [0..3], matrix rows [4..19], P [20..23], L [24..27], damping [28].
- GetDerivative:
  - Governor (if +0x1ec != 0): lim = +0x1ec * +0x1f0 + 1.9 (*1.03 if +0x148);
    f = min(0.3, |fwd.z|); factor = 1 + f*0.1/0.3 nose-down (fwd.z <= 0), else
    1 - f*0.1/0.3. If lim*factor < |v_xy| * 3.6/1.61 and F.fwd > 0: car-frame
    F.x = min(-5f, -0.5), transformed back. **This is what caps the 223 mph autopilot.**
  - d pos = vel; d rows = w x row; dP = F (+0xfc); dL = torque (+0x10c).
  - If +0x140: dL -= AngularMomentumDamping(|w|) * L.
  - d damping = +0.1 if damping < MotionDampingTable(max(|w|*0.5, |v|*0.25)), else -10.
  - If +0x13c: dL += L * (-2*damping); dP += P * (-8*damping).
  - If +0x1e8 != 0: clamp the z torque to +-c (dL.z = +-c).
- ComputeAuxiliaries: vel = P/m (if |vel| > VelocityClamp 200 and m > 1.8, scale P);
  w = R * Iinv * R^T * L (Iinv = +0x198 when +0x144), |w| clamped at 250; damping >= 0;
  copy F / torque to +0x11c / +0x12c and zero them; if +0x14c > 0 ApplyForce((0,0,-g*m), 0).
- ApplyForce(F world, r world offset from CG): |F| clamped to 1200; torque = r x F
  (unclamped F), clamped |torque| <= 100. ApplyForceInFrame rotates both vectors first.
  ApplyMoment adds unclamped.
- MoveCenterOfGravity(v): pos += R*v - R*old; +0x1d8 = v.

## Suspension

Fields: +0x14 info, +0x18 partner, +0x1c x, +0x20 xdot, +0x24 F, +0x28 wheel car pos,
+0x78 onGround, +0x80 preload, +0x84 attach, +0x94 wheel, +0x98 RB, +0x9c steering,
+0xa0 index.

- Calibrate(load, rh): preload = load; attach = WheelInfo.pos - RB+0x1d8; attach.z -= rh.
- ComputeAuxiliaries:
  - P = R*attach + RBpos; x = CheckTerrain(P, RB matrix) + radius.
  - x > info[5]: x = info[5], onGround = 1 (bottomed). Else onGround = (x >= info[6]),
    x = max(x, info[6]).
  - If UglyTimestepHack > 0.0001: xdot = (x - xprev) / Ugly.
  - wheel car pos = attach + (0, 0, x).
  - Wheel car matrix = steer matrix (front only) * wobble, translation = wheel car pos.
  - Contact velocity = RBvel (+0x8c) + w (+0xac) x (R * wheel car pos).
  - World matrix = car matrix * RB transform (+0xbc).
  - F = 0 if !onGround; else xdot * (xdot > 0 ? info[2] : info[3])
    + x * info[1] * (max(0, x) * info[0] + 1) + preload + info[4] * (x - partner.x),
    clamped >= 0.
- ResolveForces(e): wheel.ExchangeZForces(F + (onGround ? e : 0), onGround).
- ResolveMoreForces: if onGround, RB.ApplyForce(wheel+0x12c, groundPt - RBpos).
- ApplyGroundPenaltyImpulse: P' = R * wheel car pos; vn = n.(v + w x P');
  if -vn >= 0: ApplyImpulse(n * (-vn * 0.225 * mass), P').

### Steering: TwoWheelAckerman::SetSteering(u16 angle)

L = W0.x - W3.x, T = W0.y - W1.y; a = |th|; a2 = L*a / (T*a + L).
th > 0: FL = a, FR = a2; th < 0: FL = -a2, FR = -a. Rows (c, s, 0), (-s, c, 0).

## Wheel

State: +0x24 angle, +0x28 angular velocity (rad/s).

Fields: +0x14 WheelInfo*, +0x18 TireInfo*, +0x1c ControlInfo*, +0x20 CarState*,
+0x2c inertia (Calibrate: WheelMomentOfInertia 0.01), +0x30 spin matrix, +0x70 world
matrix (rows +0x70 fwd, +0x80 side, +0x90 up, +0xa0 pos), +0xb0.. car-frame matrix
(+0xb0 fwd, +0xc0 side), +0xf0 contact velocity (world), +0x100 / +0x104 slip velocity
(long / lat) when sliding, +0x108 drive + brake torque, +0x10c brake locked, +0x110 spin
direction (0 fwd, 1 ~0, 2 back), +0x114 terrain, +0x118 long slip (w*r - vx),
+0x11c on ground, +0x120 smoothed ground-torque term, +0x124 Flong, +0x128 Flat,
+0x12c world force, +0x13c ground point, +0x14c ground normal, +0x15c sliding,
+0x160 Fz (kN), +0x164 road noise, +0x168 slip, +0x16c grip use, +0x170 rolling drag,
+0x174 brake, +0x178 ebrake, +0x17c / +0x180 grip multipliers, +0x184 static grip
multiplier, +0x188 ground z offset, +0x18c long force scale, +0x190 lat force scale.

- ctor: +0x110 = 1; +0x17c, +0x180, +0x184, +0x18c, +0x190 = 1; +0x188 = 0.
- ComputeAuxiliaries: spin matrix from the angle; zero +0x108 / +0x118 / +0x124 /
  +0x128; wrap the angle; spin direction from w (> 1e-5 fwd 0, < -1e-5 back 2, else 1);
  if the direction changed through 0, clamp w to 0.
- ExchangeZForces(F, onGround): groundPt = wheelPos (+0xa0) - r*n; if F > 0:
  k = max(0.3, 4*dot((side x n) x side, n) - 3) (= 1 - 4(side.n)^2); Fz = F*k; else 0.
- CheckTerrain(normal out, terrain out, P, mat): G = (P.x, P.y, elevation + +0x188)
  (elevation -10000 on no hit); returns n.(G - P) / (n.up), or -100 if n.up < 0.001.
- GetDerivative: d angle = w; +0x120 = 0.5 * (+0x120 + (T - Flong*r));
  dw = (+0x120 - w*terrainRollingRes*0.002 - w*0.1*+0x170) / I; locked -> 0;
  if !sliding && h > 0.001: dw -= slip / (r*h) (snaps wheel speed to road speed).
- CheckForBrakeLock(Flat_possible): Tb = (4*brake*Ctl[1] + 10*ebrake*Ctl[2]) * 1.2;
  if Flat*r + |w|*0.1 < Tb: locked = (Tb > 1), w = 0; else unlocked.
- GetPilotFactor (mode 1): if !locked && w >= 0 && rear wheel:
  f = clamp((speed - 13.41)/8.94, 0, 1), return 0.85 + 0.15*f; else 1.

### GuessGroundForce(flag = 0)

1. Brake Tb = 4*brake*Ctl[1]; ebrake Te = 10*ebrake*Ctl[2]. Each is added to T (+0x108),
   negated when w > 0, unless locked.
2. Local v = (fwd.v, side.v) from the wheel world rows and the contact velocity.
3. a = atan2(vy, vx) wrapped to (-pi, pi]; if vx < 0, a += pi and wrap.
4. s = w*r - vx -> +0x118, +0x168.
5. If s < -0.5 && T < 0 && Te == 0 && flag == 0: T *= 0.5 / -s.
6. mu_d = tire[0x10] * terrain(1) * +0x17c * +0x180 (* GetPilotFactor in mode 1).
7. |slip| = sqrt(s^2 + vy^2). CheckForBrakeLock(|vx| * Fz * mu_d / |v|).
8. Sliding if |s| > 0.5 || tire[0] (degrees) < |a| (radians) || locked:
   - Flong = Fz*mu_d/|slip| * s * +0x18c; Flat = -Fz*mu_d/|slip| * vy * +0x190.
   - If speed*2.237 < 1 && mu_d > 0.1: both / mu_d.
   - +0x15c = 1, +0x100 = s, +0x104 = vy.
9. Else (grip): Flong = +0x18c * T / r;
   Flat = sgn * +0x190 * GetLateralForce(Fz, |a|), sgn = (vx > 0 ? 1 : -1) * (a > 0 ? -1 : 1);
   +0x100 = 0, +0x104 = vy.

GetLateralForce(Fz, a): idx = deg(a) * (12 / tire[0]) * 0.5; L = Fz * 0.8 / tire[1];
if int(idx) < 6: lerp(LST[i](L), LST[i+1](L), frac) * tire[2] * 2.5;
else LST[6](L) * tire[2] * 2.5.

### ModifyGroundForce(a)

Flong += (((T - Flong*r) / I) * r - (b0*a.x + c0*a.y)) * I / r, using column 0 of the
car-frame wheel matrix (a retail quirk: port it faithfully).

### FinalizeGroundForce

1. If T > 0 && !locked: Flong *= TireForceEllipseRatio 1.5 (undone if not capped).
2. cap = Fz * tire[0xC] * terrain[0x24](1) * +0x17c * +0x180 * +0x184 (* pilot).
   If |F| > cap and |F| > 0.001: scale both components.
3. +0x16c = min(1, |F|/cap), or 1 if cap <= 0.001; 1 if sliding or locked; 0 if off the ground.
4. Recompute a; if |vx| > 1: Flong -= sin(a) * Flat * 0.15.
5. World force +0x12c = Flong*row(+0x70) + Flat*row(+0x80) + Fz*row(+0x90).

## IterateForces (0x801C1520)

1. DriveTrain::GuessClutchTorque; Wheel::GuessGroundForce(0) x4.
2. v = R^T * RB+0xfc (gravity + aero accumulated so far, in the car frame).
3. 5 iterations:
   - F = v + sum of each wheel's horizontal car-frame force (Flong*row0 + Flat*row1 of
     the +0xb0 matrix).
   - M = sum(susp.attach.x * Fy - susp.attach.y * Fx); a = F/m; alpha = M * Iinv_zz.
   - For wheels with !sliding && !locked: ModifyGroundForce(a.x - alpha*attach.y,
     a.y + alpha*attach.x).
   - DriveTrain::ModifyClutchTorque.
4. FinalizeGroundForce x4; DriveTrain::FinalizeClutchTorque.

## Engine

State: w at +0x14, clamped to [idle, limiter] * 0.10472.

- Limiter flag +0x88: on when rpm > limiter - 25, off when rpm < limiter * 0.9875.
- effThr = limiter ? 0 : throttle (+0x78) * mult (+0x80).
- Total = torque(rpm) * boost (+0x90) * (CTI+0xD8 ? 1.05 : 1) * effThr + NOS
  - engineBraking(rpm) * (1 - effThr) (* 2 if limiter).
- If Total < 0 && !limiter: * 0.85 (* 0.65 in neutral).
- dw = (Total + ext (+0x7c)) / EI[3]; ext is zeroed in ComputeAuxiliaries.
- Turbo only if the turbo curve max > 0. Heat inactive (+0x8c = -1).

## DriveTrain

State: clutch at +0x14. Fields: +0x38 desired gear, +0x3c gear, +0x44 clutch state
(0 disengaging, 1 disengaged, 2 engaging, 3 engaged), +0x48 shifting, +0x4c diff w,
+0x54 auto flag, +0x58 rpm slip, +0x5c, +0x60 (3 = manual clutch), +0x64 / +0x68 shift
bonus, +0x70 clutch torque, +0x74 neutral flag, +0x78 / +0x7c / +0x80 shift modifiers,
+0x84 up[] / +0xa8 down[] shift points, +0xcc..+0xe0, +0xe4 / +0xe8 shift speed mult 1,
+0xec torque split.

- ctor: state 2, gear = desired = 1, clutch 0, +0xec = split, +0xe4 = +0xe8 = 1,
  +0xcc = -100, +0xd4 = +0xdc = -100, +0xd0 = +0xd8 = +0xe0 = 0.
  CalculateShiftPoints -> up[], down[]; up[1] *= MiscCarTunings[idx]+0x10 with
  idx = split < 0.2 -> 0 (1.0), split > 0.8 -> 2 (1.1), else 1 (1.2).
- ComputeAuxiliaries:
  - s0: Engine+0x80 = 0 (1 if +0x74); clutch <= 0 -> s1.
  - s1: gear = desired; if shifting || !idling -> s2 (unless +0x74); ChooseNewShiftModifiers.
  - s2: mult 0.7; clutch >= 1 -> clamp; ClutchMatched (|engw - diffw| <= 50) or neutral -> s3;
    idling && !N -> s0.
  - s3: mult 1; idling && !N -> s0; desired != gear -> shifting = 1, s0.
  - idling = throttle < 0.3 && diffw <= idle*0.10472 + 0.0001.
  - If +0x5c != 0, engine mult = 0. The shift bonus is drag-only.
- GetDerivative: dclutch = DTI+0x1c * +0xe8 * (bonus > 0 ? 1 + bonus :
  bonus < 0 && gear > 2 ? 0.35 : 1) * ClutchSpeed[state]; 0 with the manual clutch.
- diffw = average front w * frontFinal and/or average rear w * rearFinal (half each when
  both) * ratio[gear]; front if split <= 0.999, rear if split >= 0.001.
- SetDifferentialAngularVelocity equalises a pair when mph (CarState+0x400 * 2.237) < 40
  or both wheels are off the ground.
- ApplyClutchTorque(T), if gear != N:
  - +0x70 += T; engine ext -= T * max(0.25, 0.25*wheelsOnGround).
  - Tg = T*ratio[gear]; rear = Tg*split*rearFinal; front = Tg*(1 - split)*frontFinal.
  - b = 0.35 if both of that axle's wheels are on the ground, else 0.5.
  - Diff type (CarState+0x43c): 1 -> f = 1 - b; 2 -> f = b; else front
    f = W1.w/(W0.w + W1.w), rear f = W2.w/(W2.w + W3.w) (0.5 if |sum| <= 1e-5);
    f < b -> 0.825, f > 1 - b -> 0.175.
  - Unlocked wheels only: W0 += front*(1 - f), W1 += front*f, W2 += rear*f,
    W3 += rear*(1 - f).
- GuessClutchTorque: +0x70 = 0. s3: Apply(GetTotalEngineTorque). s0 / s2:
  slip = clamp(engw - diffw, +-300); k = DTI+0x14, halved when the rpm-slip sign flips
  with both |values| > 2000; +0x58 = rpm slip; Apply(slip*k*clutch).
- ModifyClutchTorque (s3): Apply((engAcc - diffAcc) / denom), denom = 1/engI
  + (split <= 0.999 ? (1 - split)/W0.I * (fF*ratio)^2 * 0.5 : 0)
  + (split >= 0.001 ? split/W2.I * (rF*ratio)^2 * 0.5 : 0).
- FinalizeClutchTorque: if gear != N && s3 && wheelsOnGround > 0:
  if thr <= 0.2 || driveSlip <= 0.1: ext += clamp((diffw - engw)*DTI+0x14, +-|totalEngT|);
  else diffw = engw and SetDifferentialAngularVelocity(engw).
  (The 1st-gear ClutchPlayTable is a bss graph: skipped.)
- GetDriveWheelSlippage = average wheel+0x168 over the driven wheels.

### Shifting

- GetShiftUpRPM(g, flag) = up[g] + (flag ? +0xcc : (g == 3 ? +0x80 : +0x78)), capped at
  EI[1] (EI[2] in drag). GetShiftDownRPM(g, flag) = down[g] + (flag ? +0xd4 : +0x7c).
- SetDesiredGear (auto): +0x54 = 1. If gear <= 1: desired = 2. Else if state != 3 return.
  up = ShiftUp(gear, 0); down = ShiftDown(gear, 0).
  If gear == 2: down = thr >= 0.2 ? 0 : idle + 100, prevUp = 0;
  else prevUp = ShiftUp(gear - 1, 1) * ratio[g]/ratio[g-1] - 200.
  If rpm >= up && gear < numGears + 1: upshift only if ALL 4 wheels are on the ground
  with slip (+0x118) < 4. Else if rpm <= down && gear >= 2 && rpm <= prevUp: downshift.
- SetDesiredGear (manual): if param != gear and != desired: desired = param.
- ChooseNewShiftModifiers: +0x78, +0x7c, +0x80 = uniform [-100, 0).

## Aerodynamics

Object: [0] AeroInfo, [1] RB, [2] boost, [3] 1 - Car+0x4dc, [4] throttle,
[5] wheelsOnGround/4, [10] 1 (0 on the dyno), [11] terrain drag average
(TerrainTypeInfo+0x30 over grounded wheels, or 1).

- s = |v| / [2]; k = s * [3] * [11] * Cd (AeroInfo[0]);
  F = -v * k * (1 + (OffThrottleDragFactor 2 - 1) * (1 - thr)) * [10], at the CG.
- up = max(0, RB up.z), or 1 if [5] >= 0.5; f = max(0, vhat.fwd); m = max(0.4, f^2).
- [5] == 0: D = s*up*m*(A1 + A2), front 0.4D, rear 0.41D. Else front = s*up*m*A1,
  rear = s*up*m*A2.
- ApplyForceInFrame((0, 0, -front*[10]), FrontDownforceLocation (1,0,0)); the rear
  likewise at (-1,0,0).

## Driver input (RealDriver)

- steer +0x2a8 = -(raw - 128)/128 (positive = left); throttle +0x2b4, brake +700,
  reverse button +0x2c4, ebrake +0x2cc.
- DoThrottleBrakeReverse (v = Car+0x43c, signed forward speed):
  - ebrake timer +0x29c (1 while > 0 and |v| > 8.94).
  - reverse flag +0x2d0 = 1 if brake > 0.6 && thr < 0.5; cleared when v > -2 otherwise.
  - v > 0.5 || !flag: brake = B, revThr = 0. v < -2: brake = T, fwdThr = 0, revThr = B.
    Else brake = 0, revThr = B.
  - revThr = min(1, 2*revThr). If fwdThr + 0.2 < revThr: throttle = revThr, gear R;
    else throttle = fwdThr, auto gear.

## Skipped retail parts (no data or not free-roam)

NOS, turbo pressure, engine heat, ClutchPlayTable, JumpStabilizer and post-collision
boost graphs (bss tGraphs), road noise, damage wobble, drag / drift / burnout logic,
terrain types (everything is ROAD).

## The port (done 2026-10-22)

`3DS Files/source/carsim.c` is this document, function by function; each function there
names the retail one it comes from. `CarPhysInfo` in `cardb.c` keeps every car's raw
CarPhysicsInfo block, and `k350zCpi` (the 350Z's block) stands in when a record's is
missing or fails `cpiUsable()`.

Corrections to the notes above, found while porting:
- CarState +0x3f4 is the wheels-down count, +0x3f8 the attached-wheel count, +0x3fc the
  signed forward speed and +0x400 |v|.
- `cardb.h`'s old `dragCoef` (+0x30C) is EngineInfo+0xC, the engine inertia.

PORT (no retail counterpart, or a retail part not taken):
- The world query. Retail's CheckTerrain asks the track every substep; the port probes
  once a frame at eight points (four wheels, four body corners, one batched call) and keeps
  each answer as a plane (point + normal) that the substeps measure against. The probe
  looks at most 1 m above the CG, so a wheel never stands on the underside of a deck.
- The collision geometry. The body against the ground is the deepest of the record body
  box's four lowest corners, pushed out along the surface normal; the walls are the
  existing 2D footprint resolver (TASKS.md #150). Both are resolved with
  Contact::CollisionImpulse's numbers in 3D (bounce 0.05 wall / 0.1 ground, friction
  0.09 / 0.3, MinimumContactImpulseFactor 0.3, scrape 1 m/s, the 1.85 t mass floor, the
  0.4 spin scale). Retail's CarCollisionBody against track polygons is TASKS.md #223.
- Gram-Schmidt for the orthonormalise, and a fixed LCG for the shift modifiers.
- `carsim_place` squares the car to the probed surface, and a PleaseResetMeLater sets it
  upright where it lies.
- TuneBoostAndDampers' `(Car+0x3c)+0x11c` is read as "no wheel on the ground"; that is
  not confirmed.

Offline check (`tools/carsim_walltest.py [case]`, the 350Z block on a flat plane with a
wall at y = 0):

| case | result |
|---|---|
| rest, 5 s | no drift, no sink, held by dousing state 2 |
| launch | 0-60 mph 5.20 s, 0-100 km/h 5.47 s, 1/4 mile 13.80 s at 107.7 mph, top 157.1 mph (governor 155, top gearing 157) |
| brake | 60-0 mph 2.20 s / 26.9 m, 120-0 mph 3.93 s / 97.0 m, straight |
| corners | 30 mph full lock 1.60 g, 60 mph half lock 1.18 g, 90 mph 0.3 lock 0.72 g |
| reverse, handbrake | reverses 52.9 m; 168.5 deg handbrake turn from 50 mph |
| walls | head-on at 45 mph stops dead; 20 deg glancing keeps 48 of 55 mph; 45 deg keeps 22.5 of 45 mph; grinding with the wheel turned in stays on the wall |
| roof | upside down asks for a reset after 1.63 s |
| drop | 2 m at 30 m/s lands level (worst pitch 0.8 deg) |
| hill | parked on 8.5 deg holds still, then climbs on throttle |

On Azahar with the career save (TIBURON) the car loads its own block (1370 kg, 6 gears,
redline 6500, top 224 km/h), sits on the road with real pitch and roll, and the simulation
costs 4.1-4.7 ms a frame.
