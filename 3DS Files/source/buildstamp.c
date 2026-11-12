// The build's own date and time, for the log's first lines (#327: a 3DS ran an
// older build than the one handed over, and nothing in its log said so).
// tools/build_cia.ps1 touches this file before every make, so it is compiled
// each time; another file's __DATE__ only changes when that file does.

#include "buildstamp.h"

const char g_buildStamp[] = __DATE__ " " __TIME__;
