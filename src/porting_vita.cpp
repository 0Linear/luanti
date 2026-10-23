// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "porting_vita.h"
#include "porting.h"
#include "config.h"
#include "filesys.h"
#include "log.h"
#include "settings.h"
#include "util/numeric.h"
#include "util/string.h"

#include <psp2/apputil.h>
#include <psp2/appmgr.h>
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/rng.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/io/stat.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/power.h>
#include <psp2/sysmodule.h>
#include <psp2/system_param.h>
#include <vitaGL.h>
#include <SDL2/SDL_messagebox.h>

#include <malloc.h>
#include <cstdlib>
#include <cstring>
#include <algorithm>

#ifndef VITA_NEWLIB_HEAP_MB
#define VITA_NEWLIB_HEAP_MB 224
#endif

/*
	Memory layout

	The application budget (with ATTRIBUTE2=12 "extended memory" in
	param.sfo) is split between:
	 1. The newlib heap, a single memblock reserved at process start for
	    malloc/new. All engine data (map, Lua, meshes on the CPU side) lives
	    here. Size: VITA_NEWLIB_HEAP_MB (CMake option).
	 2. vitaGL's RAM pool, which it claims from whatever is left when the GL
	    context is created, minus a 12 MiB threshold kept back by SDL.
	    Textures and vertex buffers first go to CDRAM (VRAM) and spill here.
	 3. That 12 MiB threshold: thread stacks (created after the context),
	    the network stack and system dialogs.
*/
extern "C" {
// Picked up by newlib's _init_vita_heap() before any constructor runs.
__attribute__((used)) unsigned int _newlib_heap_size_user = VITA_NEWLIB_HEAP_MB * 1024 * 1024;

// Default stack size for every pthread/std::thread. Linux uses 8 MiB of
// virtual memory, but on the Vita stacks are committed physical memory, so
// use what Android's bionic uses, which Luanti is known to work with.
__attribute__((used)) unsigned int _pthread_stack_default_user = 1 * 1024 * 1024;

// Main thread parameters, read by vita-elf-create into the process param.
// The main thread runs the renderer, the client and the main menu Lua.
__attribute__((used)) const char sceUserMainThreadName[] = "luanti_main";
__attribute__((used)) int sceUserMainThreadPriority = 0x10000100;
__attribute__((used)) int sceUserMainThreadCpuAffinityMask = SCE_KERNEL_CPU_MASK_USER_0;
__attribute__((used)) unsigned int sceUserMainThreadStackSize = 4 * 1024 * 1024;
}

namespace porting
{

namespace vita
{

// SceNet needs a memory pool of its own; 1 MiB is enough for a handful of
// UDP sockets plus cURL's TCP connections.
static constexpr size_t NET_MEMORY_SIZE = 1 * 1024 * 1024;
static void *s_net_memory = nullptr;

static void setClocks(bool max)
{
	if (max) {
		// Highest clocks the hardware officially supports.
		scePowerSetArmClockFrequency(444);
		scePowerSetBusClockFrequency(222);
		scePowerSetGpuClockFrequency(222);
		scePowerSetGpuXbarClockFrequency(166);
	} else {
		// Firmware defaults
		scePowerSetArmClockFrequency(333);
		scePowerSetBusClockFrequency(222);
		scePowerSetGpuClockFrequency(111);
		scePowerSetGpuXbarClockFrequency(111);
	}
}

static void initNetwork()
{
	if (sceSysmoduleIsLoaded(SCE_SYSMODULE_NET) != SCE_SYSMODULE_LOADED)
		sceSysmoduleLoadModule(SCE_SYSMODULE_NET);

	s_net_memory = malloc(NET_MEMORY_SIZE);
	SceNetInitParam param;
	param.memory = s_net_memory;
	param.size = NET_MEMORY_SIZE;
	param.flags = 0;
	int ret = sceNetInit(&param);
	// 0x80410110 = SCE_NET_ERROR_EBUSY: already initialized (e.g. by cURL)
	if (ret < 0 && (unsigned)ret != 0x80410110) {
		free(s_net_memory);
		s_net_memory = nullptr;
		return;
	}
	sceNetCtlInit();
}

void init()
{
	// Clocks first: everything after this (including loading builtin Lua and
	// textures) benefits from it.
	setClocks(true);

	SceAppUtilInitParam init_param;
	SceAppUtilBootParam boot_param;
	memset(&init_param, 0, sizeof(init_param));
	memset(&boot_param, 0, sizeof(boot_param));
	sceAppUtilInit(&init_param, &boot_param);

	initNetwork();

	// Pin and prioritize the main thread (renderer + client).
	// Lower number = higher priority. The default is 160.
	sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(),
		SCE_KERNEL_CPU_MASK_USER_0);
	sceKernelChangeThreadPriority(sceKernelGetThreadId(), 128);

	// Make gettext follow the system language unless the user overrides it
	// with the "language" setting.
	if (!getenv("LANGUAGE")) {
		std::string lang = getSystemLanguage();
		if (!lang.empty())
			setenv("LANGUAGE", lang.c_str(), 1);
	}

	sceIoMkdir("ux0:data", 0777);
	sceIoMkdir(USER_DIR, 0777);
}

static bool haveShaderCompiler()
{
	// vitaShaRK looks in these places for the system's Cg compiler module,
	// which every user has to extract from their own firmware.
	return fs::PathExists("ur0:data/libshacccg.suprx") ||
		fs::PathExists("ur0:data/external/libshacccg.suprx");
}

[[noreturn]] static void fatalNoShaderCompiler()
{
	// The message box is drawn through vitaGL, so bring it up first.
	vglInitExtended(0, 960, 544, 12 * 1024 * 1024, SCE_GXM_MULTISAMPLE_NONE);
	SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, PROJECT_NAME_C,
		"The shader compiler (libshacccg.suprx) is missing.\n\n"
		"Luanti needs it to render anything. Extract it from your firmware "
		"with ShaRKBR33D and restart.", nullptr);
	sceKernelExitProcess(0);
	std::abort();
}

void preVideoInit()
{
	if (!haveShaderCompiler()) {
		errorstream << "libshacccg.suprx not found" << std::endl;
		fatalNoShaderCompiler();
	}

	setClocks(g_settings->getBool("vita_overclock"));

	// Shader compiler: Luanti generates many shader variants, so prefer
	// fast code over exact IEEE semantics (the shaders do not rely on them).
	// fastprecision is off: it lowers precision of world-space math.
	vglSetupRuntimeShaderCompiler(SHARK_OPT_UNSAFE, GL_TRUE, GL_FALSE, GL_TRUE);

	// Compiled shaders are cached on the memory card when vitaGL is built
	// with HAVE_SHADER_CACHE=1 (util/vita/build.sh does that); otherwise
	// this is a no-op and shaders are recompiled on every start.
	std::string cache = porting::path_cache + DIR_DELIM "shaders";
	fs::CreateAllDirs(cache);
	vglSetShaderCachePath(cache.c_str());

	// A bigger parameter buffer avoids partial scene renders (the GPU has to
	// flush to memory mid-frame when it overflows) with long view ranges.
	u32 pb_mb = rangelim(g_settings->getU32("vita_param_buffer_size"), 4, 64);
	vglSetParamBufferSize(pb_mb * 1024 * 1024);

	// Triple buffering lets the CPU start the next frame while the previous
	// one waits for vblank, at the cost of ~2 MiB VRAM.
	vglUseTripleBuffering(g_settings->getBool("vita_triple_buffering"));

	// vitaGL's garbage collector frees GPU resources once the GPU is done
	// with them. Keep it off the render core.
	vglSetupGarbageCollector(0x10000100 + 10, SCE_KERNEL_CPU_MASK_USER_2);
}

void postVideoInit()
{
	// Pace frames with the display instead of the FPS limiter: waiting for
	// N vblanks per frame (60/N FPS) gives perfectly even frame times,
	// while sleeping to hit 30 FPS on a 60 Hz display judders.
	if (g_settings->getBool("vsync")) {
		u32 interval = rangelim(g_settings->getU32("vita_vblank_interval"), 1, 4);
		eglSwapInterval(0, interval);
	}
}

void onThreadStart(const std::string &name)
{
	/*
		Core assignment. The renderer owns core 0. Map generation, mesh
		generation and the in-process server share cores 1 and 2.
		Light, latency-sensitive threads (network, sound, HTTP) may also
		run on the system core, which keeps cores 1 and 2 free for heavy
		work.
	*/
	int mask = SCE_KERNEL_CPU_MASK_USER_1 | SCE_KERNEL_CPU_MASK_USER_2;
	int prio = 160;

	if (str_starts_with(name, "Mesh")) {
		// Directly affects how fast the world appears around the player
		prio = 140;
	} else if (str_starts_with(name, "Emerge")) {
		// Bursty and heavy; must not starve the server step
		prio = 170;
	} else if (name == "Server") {
		prio = 150;
	} else if (str_starts_with(name, "Connection") ||
			name == "OpenALSoundManager" || name == "CurlFetch") {
		mask |= SCE_KERNEL_CPU_MASK_SYSTEM;
		prio = 120;
	} else if (name == "Minimap") {
		prio = 180;
	}

	SceUID thid = sceKernelGetThreadId();
	sceKernelChangeThreadCpuAffinityMask(thid, mask);
	sceKernelChangeThreadPriority(thid, prio);
}

std::string getSystemLanguage()
{
	int lang = -1;
	if (sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_LANG, &lang) < 0)
		return "";

	switch (lang) {
	case SCE_SYSTEM_PARAM_LANG_JAPANESE:      return "ja";
	case SCE_SYSTEM_PARAM_LANG_ENGLISH_US:    return "en";
	case SCE_SYSTEM_PARAM_LANG_FRENCH:        return "fr";
	case SCE_SYSTEM_PARAM_LANG_SPANISH:       return "es";
	case SCE_SYSTEM_PARAM_LANG_GERMAN:        return "de";
	case SCE_SYSTEM_PARAM_LANG_ITALIAN:       return "it";
	case SCE_SYSTEM_PARAM_LANG_DUTCH:         return "nl";
	case SCE_SYSTEM_PARAM_LANG_PORTUGUESE_PT: return "pt";
	case SCE_SYSTEM_PARAM_LANG_RUSSIAN:       return "ru";
	case SCE_SYSTEM_PARAM_LANG_KOREAN:        return "ko";
	case SCE_SYSTEM_PARAM_LANG_CHINESE_T:     return "zh_TW";
	case SCE_SYSTEM_PARAM_LANG_CHINESE_S:     return "zh_CN";
	case SCE_SYSTEM_PARAM_LANG_FINNISH:       return "fi";
	case SCE_SYSTEM_PARAM_LANG_SWEDISH:       return "sv";
	case SCE_SYSTEM_PARAM_LANG_DANISH:        return "da";
	case SCE_SYSTEM_PARAM_LANG_NORWEGIAN:     return "nb";
	case SCE_SYSTEM_PARAM_LANG_POLISH:        return "pl";
	case SCE_SYSTEM_PARAM_LANG_PORTUGUESE_BR: return "pt_BR";
	case SCE_SYSTEM_PARAM_LANG_ENGLISH_GB:    return "en";
	case SCE_SYSTEM_PARAM_LANG_TURKISH:       return "tr";
	default:                                  return "";
	}
}

bool isEnterButtonCross()
{
	int button = SCE_SYSTEM_PARAM_ENTER_BUTTON_CROSS;
	sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_ENTER_BUTTON, &button);
	return button == SCE_SYSTEM_PARAM_ENTER_BUTTON_CROSS;
}

bool openURL(const std::string &url)
{
	// Launch the system web browser (NPXS10003) with the URL
	std::string uri = "psgm:open?titleid=NPXS10003&param=" + url;
	return sceAppMgrLaunchAppByUri(0x20000, uri.c_str()) >= 0;
}

bool secureRandom(void *buf, size_t len)
{
	// The kernel RNG returns at most 64 bytes per call
	u8 *p = static_cast<u8 *>(buf);
	while (len > 0) {
		size_t n = std::min<size_t>(len, 64);
		if (sceKernelGetRandomNumber(p, n) < 0)
			return false;
		p += n;
		len -= n;
	}
	return true;
}

size_t getFreeMemory()
{
	struct mallinfo mi = mallinfo();
	size_t heap_free = (size_t)_newlib_heap_size_user - mi.arena + mi.fordblks;

	SceKernelFreeMemorySizeInfo info;
	info.size = sizeof(info);
	if (sceKernelGetFreeMemorySize(&info) < 0)
		return heap_free;
	return heap_free + info.size_user;
}

uint32_t getMemoryBudgetMB()
{
	SceAppMgrBudgetInfo info;
	memset(&info, 0, sizeof(info));
	info.size = 0x88;
	if (sceAppMgrGetBudgetInfo(&info) < 0)
		return 365;
	return (info.total_user_rw_mem + info.total_extra_mem) >> 20;
}

void keepAwake()
{
	static u64 last = 0;
	u64 now = sceKernelGetProcessTimeWide();
	// Ticking every frame is a syscall for nothing; once a second is plenty.
	if (now - last < 1000000)
		return;
	last = now;
	sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
	sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_OLED_DIMMING);
}

} // namespace vita

} // namespace porting
