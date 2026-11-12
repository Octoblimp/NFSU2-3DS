# Retail's track-polygon collision (TASKS.md #223)

Distilled 2026-11-01 from the Speed.elf decompile dumps by a subagent, for the
port of TrackPolygonCollisionBody / GJKWafer / CollisionBodyWitness /
FindContactTime / Crowbar / Resolve. Addresses are Speed.elf (GameCube).
TASKS.md #223 has what the port has done with it and the plan for the rest.

# NFSU2 GameCube (Speed.elf): car-vs-track (wall) collision pipeline, porting spec

Sources: the scratchpad Ghidra dumps `coll223ps.c` (with addresses), `coll223b.c` and `coll223.c`, plus their annotated
copies `coll223ps_a.c` and `coll223_a.c`.
- Constants are the initial .sdata/.sdata2 values, read from Speed.elf with `tools/gc_disasm.py`.
- r13 = 0x80474DE0. "r13-0xNNNN" is a global at 0x80474DE0 - 0xNNNN.
- CodeWarrior vtables use 8-byte slots (s16 this-adjust at +0, function pointer at +4). "vt+0xNN" is the function pointer.
- Body field offsets are from the start of the CollisionBody. "RB" means the RigidBody* at body+0x68.
- Nothing has been guessed. Items not visible in the dumps are marked **UNCERTAIN** with the reason.

## 0. Global tuning knobs (r13 globals, initial values)

| global | addr | value | used by |
|---|---|---|---|
| MAX_CONTACT_RESOLUTIONS | 8046ec5c (r13-0x6184) | 20 | DoTimestepCollisions |
| MAX_CONTACTS_PER_FRAME | 8046ec60 (r13-0x6180) | 2 (per witness) | DoTimestepCollisions |
| UseOriginalWaferNormal | 8046e388 (r13-0x6a58) | 1 | FindWaferNormal |
| UseNormalCaching | 8046e384 (r13-0x6a5c) | 1 | FindWaferNormal |
| FillGJKWafer | 8046e3c8 (r13-0x6a18) | 0 | GJKWafer::Init |
| CollisionLifetime | 8046e324 (r13-0x6abc) | 0.3 s | Collision::Update |
| MinimumContactImpulseFactor | 8046e394 (r13-0x6a4c) | 0.3 | CollisionImpulse |
| MaxFrictionScrapeSpeed | 8046e3a0 (r13-0x6a40) | 1.0 | CollisionImpulse |
| MinimumTimeDifferenceForSimultaneousContact | 8046e37c (r13-0x6a64) | -5.0001 (0xc0a000d2) | ContactTimeSorting |
| WorldLoopCounter | 8046e2fc (r13-0x6ae4) | 100000 | Contact ctor (+0x28) |
| BoringAIForCameraTuningOn | 8046cea4 (r13-0x7f3c) | 0 | Car AreCollisionsEnabled |
| MaxCollisionBodyWidthX | 8046e338 (r13-0x6aa8) | runtime | MaybeCreateWitnesses sweep |
| WorldTimer | 8046fee0 (r13-0x4f00) | runtime tick, 0.00025 s per tick | lifetimes, stamps |

## 1. Timestep order

World::DoTimestep 80210be0, per physics step dt:
1. Think for all cars.
2. GlobalThinkEnd.
3. Drive.
4. CheckForLoadedTopology.
5. BeginMovement__3Car.
6. SmokeableThink.
7. **DoTimestepMove 8021078c**.
8. CheckForLoadedTopology.
9. **DoTimestepCollisions 80210838**.
10. FinishMovement__3Car.
11. Events and effects.

Also: UglyTimestepHack (8046e4f8) is set to DAT_80339630 (= 0). The constants 80339638 = 100 and 8033963c = 150 are also used in this function; their use was not examined.

**DoTimestepMove 8021078c**
- Collects bodies with +0x11 (dynamic) set, up to 182 of them.
- Calls vt+0x64 Move(dt) on each.
- Car Move 801affc4 =
  - BeginMove(dt) 801ae8a4, which snapshots the start pose:
    - +0x70..0xac = matrix (vt+0x34 GetRotation, 16 floats)
    - +0xb0 = pos (vt+0x1c)
    - +0xc0 = vel (vt+0x24)
    - +0x6c = dt
  - then Car::Move(dt), which integrates the car,
  - then UpdatePosition 801ae7bc.
- UpdatePosition 801ae7bc:
  - bbox = pos.xy ± R, where R = +0x18 is the 3D bounding radius. Set via SetBBoxMinMax, which re-sorts the CollisionBodyList.
  - **+0xfc = M_begin(+0x70) · localCentre(+0x10c) + pos_begin(+0xb0)**. This is the world bbox centre at the START of the step. It is later used for the wall-normal side test (section 6).
- There is no swept or continuous test. Collision is detected only at the end pose; see TestForContact.

**DoTimestepCollisions 80210838**
1. For every witness in CollisionBodyWitnessList (8046ff08): if `HasExpired` 801ad444, delete it (virtual dtor, arg 3).
2. For every body with +0x11 && +0x17 (dynamic and awake): `MaybeCreateWitnesses` 801af2bc.
3. For every witness: set +0x22c = 0 (per-frame resolve count), then `TestForContact(local sorted list)` 801ad7c0.
4. `while (list non-empty && iterations < MAX_CONTACT_RESOLUTIONS(20))`:
   1. head = first contact; `Resolve` 801b1e10; the head's witness +0x22c++.
   2. For A and B of the head: if +0x11 (dynamic), `RemoveContacts(list, body)` 801b1130. This frees every contact with A or B == body.
   3. Build a moved-bodies list:
      - type 1 (smokeable) with +0x148 == -1 (803394b4 = -1): Smokeable Break;
      - otherwise, if dynamic && awake: add it.
   4. `MaybeCreateWitnesses` on each moved body.
   5. For every witness touching a moved body with +0x22c < MAX_CONTACTS_PER_FRAME(2): `TestForContact` again.
      - **UNCERTAIN**: a witness whose A and B both moved appears to be tested once per moved body. The loop structure suggests the duplicate is not filtered; possible duplicate contacts.
5. If 20 is reached, the remaining contacts are freed unresolved.
6. For every Collision: `Collision::Update` 801ac5ec. If it returns false, EndCollision.
   - Update is alive while (WorldTimer - lastTick) × 0.00025 < CollisionLifetime 0.3.
7. `DeleteUnusedTrackPolygonCollisionBodies` 801b0c6c.
- The bGetTicker value is computed but unused, so there is no time budget.

## 2. Track-polygon bodies (selection, creation, lifetime)

**MaybeCreateWitnesses 801af2bc (self = dynamic body)**
1. Require `AreCollisionsEnabled(NULL)` (vt+0xb4).
2. `CreateTrackPolygonCollisionBodies` 801af514 (below).
3. Sweep-and-prune over CollisionBodyList (8046ff00), sorted on bbox maxX (+0x24). The scan window uses MaxCollisionBodyWidthX.
4. Per candidate:
   - 2D AABB overlap, margin 80331e6c = 0.
   - `!DoesWitnessExist`: bit matrix pWitnessExistanceBitField (80470830), 214 (0xd6) ids, id = +0x12.
   - `other.AreCollisionsEnabled(self)`.
   - Type 2 (static prop): `ConvertToSmokeable`, then re-check overlap.
5. `new CollisionBodyWitness(A = self, B = other)`. **Car-vs-wall is always A = car, B = TrackPolygonCollisionBody.**

**CreateTrackPolygonCollisionBodies 801af514**
1. `SetInterestBBox(topoCoord = body+0x60, bboxMin, bboxMax, velocity)` 801f1198.
   - It rebuilds only when the body bbox leaves the cached bbox.
   - New cache = bbox ± 0.1 (80336080). Each axis is extended on ONE side by v × 0.2 s (80336084), clamped to ±3.0 m (80336088). 80336078 = 0.5 and 8033607c = 0 are also used.
   - It then calls `FillCachedBBox`. **UNCERTAIN**: FillCachedBBox is not dumped; it presumably fills topo +0x3c / +0x2e0 with the TrackPolygon* list.
2. Iterate i < topo+0x2e0 over the array topo+0x3c (TrackPolygon*). Keep a polygon if:
   - poly+0x10 == 0 (no body yet),
   - its bbox (+0x14 min, +0x1c max) overlaps the body bbox, margin 80331eb0 = 0,
   - `TrackPolygonCheck` 801af8b4 passes.
   - At most 48 (0x30) are kept.
3. Bubble sort the kept list by poly+0xc (TopologyTreePolygon* address).
4. Dedup via TrackPolygonKeyTable[GetTrackPolygonKey(poly)]. If the key count is non-zero, compare every vertex against existing type-3 bodies, tolerance 80331eb4 = 0.001. If identical, skip.
5. Stop when TrackPolygonCollisionBodySlotPool (8046ff70) is full.
6. Otherwise: `new TrackPolygonCollisionBody(id, topoTree, poly+0xc)`.

**TrackPolygonCheck 801af8b4** (cheap slab and sphere reject)
- up = car matrix row 2 (+0x20..+0x28); n = poly normal (+0x64..+0x6c); d = poly+0x70.
- c = min(up·n, 1) (80331eb8 = 1); s = sqrt(1 - c²), using an rsqrt guard of 5e-11 (80331ebc) and 0.5 (80331ec0).
- dist = pos·n - d.
- h = +0xd0 (-minZ + 0.05); **if c·dist < 0 (80331ec4 = 0)**, h = +0xd4 (maxZ + 0.05).
- Pass iff `|dist| < |c|·h + s·R` (R = +0x18) **and** `TrackPolygon::GetDistanceOutside(poly, pos.xy, R + 0.1 (80331ec8)) <= R`.

**TrackPolygon::GetDistanceOutside 801f058c** / **GetAmountOutside 801f0494**
- GetAmountOutside = max over edges of the 2D cross product (edge × (p - edgeStart)).
  - Edges: v0→v1, v1→v2, v3→v0, plus v2→v3 when nverts (+9) == 4.
  - The initial value is -1 (80335f04); zero compares use 80335f08 / 80335f0c = 0.
- GetDistanceOutside:
  - If `!(flags & 2)` and GetAmountOutside <= 0 (inside the 2D footprint), return 0.
  - Otherwise return the first per-edge 2D point-segment distance below maxDist, else maxDist.

**BuildBoundingVolumes 801ae39c** (car, local space)
- min and max start at 80331df8 = 0, so the box always contains the origin.
- +0x18 = max 3D vertex radius.
- +0xd8 = max 2D radius + 0.05 (80331e08).
- +0xd4 = maxZ + 0.05; +0xd0 = -minZ + 0.05.
- +0x10c = 0.5 × (min + max).

**Lifetime**
- `DeleteUnusedTrackPolygonCollisionBodies` 801b0c6c deletes every body in TrackPolygonCollisionBodyPointerList (8046ff10) whose witness count +0x14 == 0.
- Witnesses expire via HasExpired (section 4). Bodies therefore live only while some car's bbox overlaps them or a Collision is active.

**TrackPolygon (sizeof 0x74) ctor 801f0150** (from TopologyTreePolygon tp, 32 bytes)
- Header fields:
  - +0xb = tp[2] (terrain type; `GetTerrainType` 801b0c14 returns it sign-extended).
  - +0xa = flags = tp[3].
  - +8 = 0; +0x10 = 0 (body ptr); +0xc = tp.
  - tp+6 = (this - TrackPolygonArray(r13-0x62dc, 8046eb04)) / 0x74.
- Shorts: X at tp+8 [4], Y at tp+0x10 [4], Zoff at tp+0x18 [4], Zbase at tp+4.
- Bbox:
  - +0x14 = X[tp1 & 3], +0x18 = Y[(tp1>>2) & 3] (min)
  - +0x1c = X[(tp1>>4) & 3], +0x20 = Y[(tp1>>6) & 3] (max)
- Vertices i = 0..3 at +0x24 + 16i (x, y, z, pad):
  - x = X[i]/8, y = Y[i]/8. Computed as (s16 << 13) × 2^-16 (80335ef8 = 1.52588e-5; 80335ef0 is the int→float magic).
  - z = Zbase + Zoff[i]/256. Computed as (Zoff·0x100 + Zbase·0x10000) × 2^-16.
  - All 4 vertices are always decoded.
- nverts +9 = 4 if flags & 0x10, else 3.
- **flags & 4**: v0.z = v3.z = -2048 (80335efc), v1.z = v2.z = +2048 (80335f00). This makes an infinite vertical wall.
- n (+0x64,+0x68,+0x6c) = normalize((v1 - v0) × (v2 - v0)); d (+0x70) = v0·n.
  > `fStack_68 = *(float *)(param_1 + 0x34) - *(float *)(param_1 + 0x24);` (v1 - v0) ... `bCross(&n, &e1, &e2)`
- Classification:
  - `IsWallish` 801b0c24: |n.z| < 0.1 (8033200c).
  - `IsGroundish` 801b0c48: |n.z| > 0.7 (80332010).

## 3. TrackPolygonCollisionBody ctor, GJKWafer, thickness

**TrackPolygonCollisionBody ctor 801b07b4 (id, TopologyTree* tree, TopologyTreePolygon* tpp)**
- StaticCollisionBody(id, type 3); vtable `_vt.25TrackPolygonCollisionBody` 80392f20.
- GJKWafer ctor at body+0x6c. Then:
  - +8 = &wafer (this is the GJK prim; GetGJKPoly 801d5dc4 returns +8)
  - +0x68 = tree
  - +0x60 = `AssignTrackPolygon(tree, tpp)`, and poly+0x10 = body
  - **+0x64 = (tpp[0] != 0)** (feeds the normal mode, section 6)
- TrackPolygonKeyTable[key]++.
- `wafer.Init(poly, origin = (0,0,0) (80331fc8), thickness = 1.0 (80331fcc))`.
- bbox = poly +0x14 / +0x1c. The pose is identity at the origin (PlotToCaffeine).
- The debug name is "TrackPolygon%d %s"; the index is (tpp - tree+8) >> 5.
- **UNCERTAIN**: AssignTrackPolygon is not dumped. Assumed to return the TrackPolygon built from tpp (see 801f0150).

**Static body virtuals** (vtable 80392f20, resolved from ELF; values from .sdata2)

| slot | function | behaviour |
|---|---|---|
| +0x1c GetPosition | 801b0c08 | &ZeroVector (803dd890) |
| +0x24 GetVelocity | 801d5dec | &ZeroVector |
| +0x34 GetRotation | 801d5e04 | &IdentityMatrix (803dd8a0) |
| +0x3c GetAngularVelocity | 801d5e10 | &ZeroVector |
| +0x44 GetCenterOfGravity | 801d5e1c | &ZeroVector |
| +0x4c SetPosition | 801d5e28 | no-op |
| +0x5c GetMaxBackupTime | 801d5e34 | 1000.0 (80333530) |
| +0x64 Move | 801d5e40 | no-op |
| +0x6c Backup | 801d5e44 | no-op |
| +0x74 Interpolate | 801d5e48 | pos = ZeroVector, matrix = IdentityMatrix |
| +0x7c RecomputeAuxilliaries | 801d5f1c | no-op |
| +0x84 GetCurrentVelocities | 801d5f20 | v = w = ZeroVector |
| +0x9c SetToPhysicsMode | 801d5fc8 | no-op |
| +0xa4 ConsiderImpulse | 801d5fcc | 1.0 (80333540) |
| +0xac ApplyImpulse | 801d5fd8 | no-op |
| +0xb4 AreCollisionsEnabled | 801d5fdc | `+0x15 == 0` |
| +0xc4 GetMassInverse | 801d5f98 | 0.0 (80333534) |
| +0xcc GetInertiaTensorInverse | 801d5fa4 | &ZeroMatrix (803dd8e0) |
| +0xd4 IsMoveable | 801d5fec | 0 |
| +0xe4 IsBlinking | 801d5db8 | 0 |
| +0xec GetBounciness | 801d5fb0 | 0.001 (80333538) |
| +0xf4 GetFriction | 801d5fbc | 0.001 (8033353c) |
| +0x104 GetGJKPoly | 801d5dc4 | body+8 (the wafer) |
| +0x10c / +0x114 Begin/EndCollision | 801d5dcc / 801d5ddc | body+0x16 ++ / -- |

Consequences:
- Wafer vertices are world coordinates.
- The wall's pose is the identity at the origin for every time t.
- min(GetMaxBackupTime) = the car's dt.

**GJKWafer ctor 801b9938**
- type +0x10 = 2; 9 vertex slots of 0x30 bytes.
- +0x1cc thickness = 0 (80332420); +0x1c8 thick-state = 0.

**GJKWafer::Init 801b99f0 (poly, origin, thickness)**
1. nverts = 3 if `bEqual(v2, v3, 0.001)` (80332428), else 4.
   - Uses the vertex data, NOT poly+9. **UNCERTAIN**: whether tri data always has v3 == v2. If not, a flag-0x10-clear tri would become a quad here.
2. vert[i] = v[i] - origin, w = 0 (80332450), index = i.
3. Face centroid → +0x1d0. Weights use 80332438 = 1, 8033243c = 0.33, 80332440 = 0.34, 80332444 = 0.25, 80332448 = 0.5.
4. If thickness > 0:
   - Append vert[n+i] = v[i] - thickness·n.
   - nverts ×= 2.
   - Centroid of all vertices → +0x1e0.
   - +0x1cc = thickness (1.0).
5. The FillGJKWafer block (centroid plus edge midpoints, 7/9 vertices; it uses raw, not origin-relative, vertices) is dead: the flag is 0, and the thickness branch also stores 0 to it.

**GJKWafer::GetSupportPoint 801ba1d4**
- Max dot over vertices (start -9.16e36, 8033244c).
- **If +0x1cc > 0 and +0x1c8 == 0, only the first nverts/2 (the face) are searched.**
- Output w = 0.

**Thickness switch**
- `SetTrackPolygonThickness(witness, bool force)` 801ad5bc:
  - Acts ONLY when the pair is wafer vs type-1 (smokeable).
  - With force == 0 it computes (smokeable.pos - fullCentroid(+0x1e0))·n > 0. If poly flags & 2, centroid.z is first set to the smokeable's z.
  - It writes wafer +0x1c8.
- `ResetTrackPolygonThickness` 801ad760: +0x1c8 = 0.
- **Result for cars: the wall is always the flat, zero-thickness 3- or 4-vertex face.** The 1.0 m extrusion exists only for smokeables.

## 4. Witnesses (CollisionBodyWitness)

**Layout**

| offset | field |
|---|---|
| +8 | GJKWitness (section 5) |
| +0x218 / +0x21c | GJK prims A / B |
| +0x220 | vtable |
| +0x224 | A |
| +0x228 | B |
| +0x22c | per-frame resolve count |
| +0x230 | Collision* |
| +0x234 | cached normal side (0 = unset, 1 = +n, 2 = -n) |

**ctor 801acd8c (A, B)**
- `GJKWitness::Initialize(A.GetGJKPoly(B), B.GetGJKPoly(A))` 801b87b0. **The hull is chosen once, at creation.**
- Sets the existence bit both ways; A+0x14++ and B+0x14++.

**HasExpired 801ad444**: false (keep) if:
- a Collision is active (+0x230), **or**
- (bbox overlap, margin 80331d4c = 0) && (A or B awake, +0x17) && both AreCollisionsEnabled.

**BeginCollision 801ad304 / EndCollision 801ad3ac**
- Create or delete the Collision object.
- Call body vt+0x10c / vt+0x114.

**AreCollisionsEnabled**
- Car 801b0010:
  - Requires BoringAIForCameraTuningOn == 0.
  - Type-1 special case.
  - Returns 0 against other cars when car+0x178 == 0.
  - Against walls it returns the Dynamic result.
- Dynamic 801ae6a0: `+0x15 == 0 && topo(+0x60)+8 != 0`.

**Car GetGJKPoly 801aff10 (other)**
- Default: polytope at +8, from `FindGJKPolytope`:
  1. RideInfo volume hash,
  2. then the default hash,
  3. then a fallback box (80331ee0..ef4) = min (-2.217, -0.971, -0.2), max (2.165, 0.971, 0.624).
- Returns the bbox GJKBoxPoly at +0x128 instead when ALL of:
  - driver type != 1,
  - the box exists,
  - car+0x434 == 4,
  - other is a wafer (or a car with 0x434 == 4 and driver != 1).
- **UNCERTAIN**: the meaning of driver type 1 and of car+0x434 / +0x438. They are used as 4 - x wheel counts in GetFriction; "== 4" plausibly means all wheels off the ground.

## 5. GJKWitness

**Fields**

| offset | field |
|---|---|
| +0 | dist |
| +4 | colliding (dist <= 0.001, 803323cc) |
| +8 | deep (dist <= 1e-5, 803323c8) |
| +0xac / +0xbc | closest points on A / B. Read by FindContactPointAndNormal as CollisionBodyWitness +0xb4 / +0xc4. Presumed world space (the midpoint is used as the world contact point) |
| +0xc / +0x1c | last posA / posB |
| +0x104 | GJK warm-start / simplex reset field |
| +0x108 / +0x148 | matA / matA^T |
| +0x188 / +0x1c8 | matB / matB^T |
| +0x208 | dirty |
| +0x20c | last wafer thick-state |
| +0x210 / +0x214 | prims |

**Functions**
- `Initialize` 801b87b0: stores the prims and resets.
- `UpdateWorldMatrices(matA, posA, matB, posB)` 801b8848:
  - Sets dirty **only if a position moved more than 0.001** (803323bc), so rotation-only changes do not dirty it.
  - Copies the matrix, replaces the translation with pos (w = 1, 803323c0), and stores the transpose.
  - **UNCERTAIN**: a pure rotation could leave a stale distance. That is faithful, but worth knowing.
- `RecalcDistance` 801b8ab0:
  - If not dirty, keeps the cached dist.
  - If the wafer thick-state changed, resets +0x104.
  - Calls `SCEA_GJKdistance`, then sets +4 and +8.
  - **UNCERTAIN**: SCEA_GJKdistance (the GJK core) is NOT in the dumps. Any exact convex-convex distance with closest points is equivalent.
- `IsColliding(bool* deep)` 801b8a98: returns +4, and writes +8 to *deep.

**TestForContact 801ad7c0 (witness, list)**
1. A type-4 body is tested only against type 0/1 partners; any other pair containing type 4 is skipped. Then require mutual AreCollisionsEnabled.
2. thick = (Collision active) && (WorldTimer - coll+8) × 0.00025 (80331d70) > 0.05 (80331d74). Then `SetTrackPolygonThickness(thick)` (a no-op for cars).
3. `MoveCenterOfGravity(primA, A.GetCenterOfGravity)` and the same for B.
4. `UpdateWorldMatrices(A.GetRotation, A.GetPosition, B.GetRotation, B.GetPosition)` at the **END pose**. Then RecalcDistance, then ResetTrackPolygonThickness.
5. If colliding (dist <= 0.001), and unless (both IsBlinking && both dynamic), which never happens for walls:
   - The car side stamps car+0x12c = WorldTimer, and +0x134 too if `IsYawControlInducingCollisionBody(wall)`. **UNCERTAIN**: that function is not dumped.
   - `new Contact(A, B, witness)` (from ContactSlotPool 8046e374), then `AddSorted(list, ContactTimeSorting)`.
   - Otherwise (blinking case) it only stamps +0x130.

## 6. Contact time, interpolation, normal

**Contact ctor 801b13a4**

| offset | field |
|---|---|
| +0xc | A |
| +0x10 | B |
| +0x14 | witness |
| +0x18..+0x20 | crowbar offset |
| +8 | `FindContactTime()` |
| +0x28 | WorldLoopCounter |

**FindContactTime 801b13ec.** t is the BACKUP time, measured backwards from the end of the step.
- offset = 0 (803320d4).
- hi = min(A.GetMaxBackupTime, B.GetMaxBackupTime). For car vs wall this is the car's dt (+0x6c, 801d6060).
- lo = 0; best = -1 (803320d8); result = 0.
- **Only if deep (witness GJK +8, dist <= 1e-5)**:
  1. `SetTrackPolygonThickness(1)`.
  2. Repeat 8 times:
     - `t = lo + (hi - lo)·0.5` (803320dc).
     - A.Interpolate(t), B.Interpolate(t).
     - UpdateWorldMatrices, RecalcDistance.
     - If still deep: lo = t.
     - Else if colliding: **return t** (goto reset).
     - Else (separated): hi = t; best = min(best, t).
  3. After the loop: if best <= 0 (never separated), `Crowbar(t_last)` and return t_last. t_last = dt·255/256 when every probe was deep. Otherwise return best (the smallest separated backup time = the last separated probe).
  4. `ResetTrackPolygonThickness`.
- **If colliding but not deep (1e-5 < dist <= 0.001), t = 0**: no backup, resolve at the end pose.
- Note: hi itself (the start-of-step pose) is never probed.
  > `if (*(int *)(*(int *)(param_1 + 0x14) + 0x10) != 0) { ... do { dVar9 = (hi-lo)*0.5+lo; ...} while (iVar1 < 8);`

**ContactTimeSorting 801b1310** (disassembly)
- `if |t1 - t2| >= -5.0001` → `return t1 > t2 ? 0 : 1`. This is always taken.
- The CG tie-break (GetCGDistanceFromTrackPolygon 801b11ac, called twice) is dead with the default knob.

**AddSorted 80058a20**
- Inserts new before the first existing node where cmp(new, existing) == 0, else at the tail.
- **The list is therefore sorted by backup time DESCENDING**, so the earliest real-time contact resolves first. Ties are FIFO.

**Interpolate (Dynamic) 801ae9f8 (t, &pos, &mat)**
- If dt (+0x6c) >= 1e-6 (80331e30): f = (dt - t)/dt.
  - pos = start(+0xb0) + (cur - start)·f.
  - **All 16 matrix elements are lerped individually** between +0x70 and the current matrix. There is no slerp and no re-orthonormalisation.
- Otherwise it returns the current pose (80331e34 = 1e-6).
- `InterpolateVelocity` 801aee68 does the same lerp for linear velocity (+0xc0 → current).

**FindWaferNormal 801b1628 (contact, &n, wafer, other, const bVector3* p)**
- n = poly (+0x64, +0x68, +0x6c).
- **If UseOriginalWaferNormal (1) && wafer+0x64 == 0 (tp[0] == 0): return +n.** Single-sided: always the face normal, whichever side the car is on.
- Otherwise it is two-sided:
  - If p == NULL or other is type 0 (car): **p = other+0xfc**, the car's start-of-step world bbox centre (section 1).
  - c = wafer +0x1e0 if (+0x1c8 && +0x1cc > 0), else +0x1d0 (face centroid). For cars it is always +0x1d0.
  - With UseNormalCaching (1), only type-1 others read or write witness+0x234. A car always recomputes: `(p - c)·n >= 0 (803320e0) ? +n : -n`.
  - **UNCERTAIN**: the meaning of tp[0] (the single-/two-sided flag). Only its use is visible.

**FindContactPointAndNormal 801b2164**
- point = 0.5 (8033210c) × (witness+0xb4 + witness+0xc4). This is the GJK closest-point midpoint from the LAST RecalcDistance.
  - **UNCERTAIN / faithful quirk**: that last RecalcDistance is the last bisection probe or crowbar step, not necessarily the returned t.
- normal:
  - A static: FindWaferNormal(A, B).
  - B static: -FindWaferNormal(B, A). For a car-wall pair (A = car) the normal points from the car toward the wall.
  - Otherwise: normalized closest-point delta if its length >= 1e-6 (8033211c), else the position delta. Uses rsqrt constants 80332110 = 5e-11, 80332114 = 1, 80332118 = 0.

**GetCGDistanceFromTrackPolygon 801b11ac** (disassembly; only used by the dead tie-break)
- A = wafer, B dynamic: FindWaferNormal(c, &n, A, B, NULL), flag = 1, return dot(B.pos, n).
- B = wafer: n = -FindWaferNormal(B, A), return dot(A.pos, n).
- Otherwise flag = 0, return 0.
- There is no plane-d term (as decompiled).

## 7. Crowbar, Resolve, helpers

**Crowbar 801b18e8 (t, contact)** (start confirmed by disassembly)
1. A.Interpolate(t) → posA, matA; B.Interpolate(t) → posB, matB.
2. Direction n, pointing from A toward B:
   - A wafer: FindWaferNormal(c, &n, A, B, &posB).
   - B wafer: n = -FindWaferNormal(c, B, A, &posA).
   - Smokeable (type 1/2): ∓ the other's velocity, or the position delta if speed < 55.5556 (803320f4).
   - Otherwise: normalize(posB - posA).
3. Loop, at most 20 passes:
   - `total += step` (total starts 0, 803320f0; step starts 0.005, 803320f8).
   - `if (step < 0.1) step += 0.01` (803320fc / 80332100).
   - off = n·total, with **off.z clamped to [-0.15, +0.15]** (80332104 / 80332108).
   - If A IsMoveable: posA -= off. If B IsMoveable: posB += off.
   - UpdateWorldMatrices(matA, posA, matB, posB); RecalcDistance; contact+0x18 = off.
   - `while (colliding && deep && ++i < 20)`.
4. Cumulative push: 0.005, 0.02, 0.045, 0.08, 0.125, 0.18, 0.245, 0.32, 0.405, 0.5, then +0.105 per pass, reaching 1.55 m at pass 20. The car is pushed along n (away from the wall for a single-sided face: +n).

**IsMoveable**
- Car 801d60c4: `!IsMovementDoused(car)`.
- Static: 0.

**Resolve 801b1e10**
1. `SetToPhysicsMode(other)` on A and B (car 801afea8).
2. `Backup(t)` on A and B. Dynamic 801aef50:
   - Interpolate(t) → RB+0x18 pos and RB+0x28..+0x64 matrix.
   - InterpolateVelocity(t) × mass(RB+0x150) → RB+0x68 linear momentum.
   - **Angular momentum is untouched** (it keeps its end-of-step value).
   - With no RB, it calls vt+0xfc (no-op).
3. posA = A.GetPosition, posB = B.GetPosition; `RemoteCaffeinatingUpdate(contact)`. **UNCERTAIN**: not dumped; called 3 times; presumably wakes the bodies.
4. If A IsMoveable: posA -= offset(+0x18). If A is the smokeable of a car-smokeable wall contact, keep its z. Same for B: posB += offset.
5. SetPosition (vt+0x4c; Dynamic 801d6000 writes vt+0x1c's storage) for A and B.
6. RecomputeAuxilliaries (vt+0x7c; 801af0f0) for A and B; RemoteCaffeinatingUpdate.
7. **CollisionImpulse** 801b24f4.
8. RecomputeAuxilliaries for A and B.
9. **Move(t)** for A and B. Car 801affc4 = BeginMove(t) + Car::Move(t) + UpdatePosition. The remaining t seconds are re-integrated from the corrected state; +0xfc becomes the backed-up centre.
10. RemoteCaffeinatingUpdate.

**Collision object**
- Collision ctor 801ab5f8: if a wafer is involved, the non-wafer body's +0x11c++.
- `AddImpulse` 801ab990:
  - Accumulates the impulse and its magnitude.
  - Records lastTick = WorldTimer.
  - Calls `CheckForWildCollision` 801abe94: |J| cap 40 (80331b0c), wild if >= 30 (80331b10). It only applies to car-car or car-world-object pairs, not walls.
  - Effects thresholds 50 (80331ae8) and 100 (80331af0).
  - **UNCERTAIN**: the impulse-vector argument is lost in the decompile.
- `Update` 801ac5ec: alive while (WorldTimer - lastTick) × 0.00025 (80331b50) < 0.3.

**Other helpers**
- `IsCarSmokeableWallContact` 801b2c0c: in a car-smokeable contact, returns the type-1 body if its +0x11c >= 1.
- `GetSupportingContactVerticality`:
  - Dynamic 801ae6dc: max z of normalize(pos - collisionPoint) over ground collisions, starting at 0.
  - Car 801b00f8: returns 1.0 if car+0x434 != 0.

## 8. What Resolve feeds into CollisionImpulse (801b24f4) — inputs only

1. **Contact point and normal**: FindContactPointAndNormal (section 6). For car (A) vs wall (B) the normal is the wall normal negated, pointing car → wall.
2. **Collision object**: `GetCollision(witness, &point)`. **UNCERTAIN**: not dumped; presumably BeginCollision if +0x230 == 0. Receives AddImpulse.
3. **Pose**: positions via vt+0x1c, after Backup + offset + SetPosition. The wall position is ZeroVector.
4. **Velocities**: vt+0x84 GetCurrentVelocities. Car 801af134. Wall: v = w = 0.
5. **Early out**: if the relative normal velocity >= 0 (80332128 = 0), return (separating, no impulse).
6. **Bounciness**: Contact::GetBounciness 801b2d3c. For a car (type 0) vs a type-3 pair, the **car's value alone** is used; otherwise the max of the two.
   - Car 801b0690: 0.1 (80331f74), × 0.5 (80331f78) if the wall IsWallish. **Wall: e = 0.05.** Other track polygons: 0.1.
7. **Friction**: Contact::GetFriction 801b2c80, with the same car-only rule. Car 801b0590:
   - Base μ = 0.3 (80331f5c).
   - If the polygon IsGroundish: μ = 0.3 × ((4 - car+0x434)·0.17 (80331f70) + (4 - car+0x438)·0 (80331f68) + 1 (80331f6c)).
   - Else if IsWallish: **μ = 0.3² = 0.09**.
   - Clamped to <= 1.0.
   - Static GetFriction / GetBounciness (0.001) are never used for car-vs-wall.
8. **Mass**: GetMassInverse (vt+0xc4) is Dynamic RB+0x154 for the car and 0 for the wall. GetInertiaTensorInverse (vt+0xcc) is RB+0x158, or RB+0x198 when RB+0x144 != 0; ZeroMatrix for the wall.
9. **Constants**:
   - MinimumContactImpulseFactor 0.3: minimum impulse = 0.3 / max(invMass).
   - MaxFrictionScrapeSpeed 1.0.
   - 8033212c = 1, 80332130 = -57575 (sentinel), 80332134 = 5e-11, 80332138 = 0.5, 8033213c = 1e-5.
   - 80332140 = 0.25 (smokeable wall-contact z factor).
10. **ApplyImpulse**:
    - Car vt+0xac is 801b0350 (constants 0.540541 (80331f44), 1 (80331f40), 0.4 (80331f54), 1 (80331f2c)).
    - Dynamic 801d6094 → RigidBody::ApplyImpulse.
    - Wall: no-op.
    - ConsiderImpulse returns 1.0 for both (80333548 / 80333540).

## 9. Minimal faithful port (car vs wall only)

Per physics step, after integrating every car:
1. **Snapshot** before integrating each car: matrix, pos, vel, dt (BeginMove). After integrating: bbox = pos.xy ± R, and centre0 = M0·localCentre + p0 (UpdatePosition).
2. **Candidates**:
   - Refresh the interest box only when the car bbox leaves it: pad 0.1, one-sided velocity lookahead v·0.2 clamped to 3 m.
   - Keep polygons whose 2D bbox overlaps the car bbox and that pass TrackPolygonCheck, at most 48.
   - Dedup vertex-identical polygons (1 mm).
   - Decode the polygon as in section 2: /8 xy, Zbase + Zoff/256, flag 4 makes ±2048 z, n from (v1 - v0) × (v2 - v0).
3. **Witness per (car, polygon)**:
   - The hull is fixed at creation (the bbox box when car+0x434 == 4, else the RideInfo polytope).
   - Drop the witness once the bboxes stop overlapping and no Collision is live. A Collision lives until 0.3 s after its last impulse.
4. **Detection** at the END pose: dist(carHull, flat polygon) <= 0.001 → contact.
   - The polygon is the zero-thickness 3/4-vertex face (support = max-dot over its vertices).
   - The world pose is identity.
   - Keep the 1 mm "pos-only dirty" cache if you want bit-faithful behaviour.
5. **Contact time**:
   - If dist <= 1e-5: run 8 bisection probes on the backup interval [0, dt]. Probe pose = lerp(start, end, (dt - t)/dt), with an element-wise matrix lerp. Use the rules of section 6.
   - Else t = 0.
   - If no probe separates: crowbar at t = dt·255/256 along n, using the 20-step schedule with z clamped to ±0.15, stopping when no longer deep.
6. **Normal**:
   - tp[0] == 0: the polygon face normal +n (single-sided).
   - Otherwise: sign(n·(centre0 - faceCentroid)) · n.
   - The impulse uses -that (car → wall). The crowbar pushes the car along +that.
7. **Order**:
   - Sort contacts by t descending; FIFO on ties.
   - Resolve the head; drop every contact of that car.
   - Re-create witnesses and re-test (end pose) those of the car whose per-frame count is < 2.
   - Cap 20 resolutions per step; discard the rest.
8. **Resolve**:
   1. Set the car pos and orientation to the probe pose at t. Set linear momentum = lerp(v0, v1, f)·m. Leave angular momentum as is.
   2. Subtract the crowbar offset from the car pos.
   3. Run CollisionImpulse (already ported). Inputs: point = midpoint of the GJK closest points at the last evaluated pose; e = 0.05, μ = 0.09 for wallish (|n.z| < 0.1) polygons; the wall has zero mass inverse and zero velocity.
   4. Re-integrate the car for the remaining t seconds (BeginMove(t) + Car::Move(t) + UpdatePosition).
9. **Not needed for car-vs-wall**: polygon thickness and extrusion (smokeable only), normal caching (smokeable only), FillGJKWafer, the CG tie-break, CheckForWildCollision and the Static body virtuals beyond the table in section 3.

### Open UNCERTAIN list (not dumped or semantics unknown)
- SCEA_GJKdistance (GJK core)
- FillCachedBBox
- AssignTrackPolygon
- GetCollision
- RemoteCaffeinatingUpdate
- IsYawControlInducingCollisionBody
- IsMovementDoused
- the meaning of tp[0], driver type 1, car+0x434/+0x438 and type 4
- a possible duplicate re-test of a witness whose A and B both moved
- the AddImpulse vector argument
- GJKWafer tri/quad being decided by v2 == v3 rather than poly+9
