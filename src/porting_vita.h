// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#ifndef __vita__
#error This file may only be included on the PlayStation Vita
#endif

#include <string>
#include <cstddef>
#include <cstdint>

/*
	PlayStation Vita platform layer.

	Hardware summary (what everything in here is tuned for):
	- 4x ARM Cortex-A9 @ 444 MHz max; core 3 is shared with the OS, so
	  applications get three full cores (USER_0..USER_2).
	- 512 MiB LPDDR2 of which an application gets ~365 MiB with the
	  extended memory attribute, plus 128 MiB CDRAM (VRAM).
	- PowerVR SGX543MP4+ (tile based deferred renderer), driven through
	  vitaGL which translates OpenGL ES 2.0 + GLSL ES 1.00 to sceGxm + Cg.
*/

namespace porting
{

namespace vita
{

// Directory layout
constexpr const char *SHARE_DIR = "app0:";
constexpr const char *USER_DIR = "ux0:data/luanti";

/// Called first thing in main(): clocks, network, system modules, paths.
void init();

/// Called right before the video device is created (settings are loaded).
/// Configures vitaGL (memory pools, shader compiler, caches).
void preVideoInit();

/// Called when a Luanti thread starts. Pins it to a core and adjusts its
/// priority based on its role.
void onThreadStart(const std::string &name);

/// Language code of the system language (e.g. "de"), empty if unknown.
std::string getSystemLanguage();

/// True if the system "enter" button is cross (Western layout).
bool isEnterButtonCross();

/// Opens a URL in the system web browser.
bool openURL(const std::string &url);

/// Fills buf with len cryptographically secure random bytes.
bool secureRandom(void *buf, size_t len);

/// Free main memory (bytes) currently available to the application,
/// both newlib heap and system memory.
size_t getFreeMemory();

/// Total memory budget (MiB) of the application.
uint32_t getMemoryBudgetMB();

/// Keeps the screen and the system awake while the game is running.
void keepAwake();

} // namespace vita

} // namespace porting
