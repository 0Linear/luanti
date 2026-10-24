// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

/*
	VITA_CHECKPOINT("what"): records that the main thread got here, for the
	stall watchdog of the PlayStation Vita build (see porting_vita.h).
	Does nothing on other platforms.
*/
#ifdef __vita__
namespace porting::vita {
void checkpoint(const char *where);
}
#define VITA_CHECKPOINT_STR2(x) #x
#define VITA_CHECKPOINT_STR(x) VITA_CHECKPOINT_STR2(x)
#define VITA_CHECKPOINT(what) \
	porting::vita::checkpoint(__FILE__ ":" VITA_CHECKPOINT_STR(__LINE__) " " what)
#else
#define VITA_CHECKPOINT(what) do {} while (0)
#endif
