// Bleach Bypass (TASKS.md #326): SPEED2.EXE's g_BleachByPassEnable
// (DAT_00870d00, the display block's +0x50), the visual treatment's grade
// (FUN_005c68d0). See bleach.c.
//
// For each eye, bleach_draw() at the end of the view: after the horizon fog,
// before trails_post() (Over Bright reads the graded frame).
#pragma once

#include <3ds.h>
#include <citro3d.h>
#include <stdbool.h>

void bleach_set_log(void (*sink)(const char* msg));

// Grades `eyeTarget` (a 240x400 RGBA8 eye, bound, its 3-D drawn) in place
// and leaves it bound, the depth test and blend as the world's passes expect
// them. `haze` and `tint` (NULL for none) are the rain haze's and the screen
// tint's quad colours, which the PC lays over the frame before the grade:
// drawn here first, the caller then leaves them out of its 2D. False when
// nothing was drawn (no capture buffer or shader).
bool bleach_draw(C3D_RenderTarget* eyeTarget, const u8* haze, const u8* tint);
