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
#include <SDL2/SDL_events.h>
#include <SDL2/SDL_timer.h>
#include <fstream>

#include <malloc.h>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <atomic>
#include <sstream>

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
	 2. vitaGL's RAM pool, which it claims from whatever is left when it is
	    initialized, minus SYSTEM_RESERVE. Textures and vertex buffers first
	    go to CDRAM (VRAM) and spill here.
	 3. SYSTEM_RESERVE: memory blocks the system allocates on our behalf
	    after that point: stacks of the ~14 threads Luanti starts, LuaJIT's
	    machine code areas, the Cg compiler module and system dialogs.
	    (SDL would only keep 12 MiB, which is not enough for all of that,
	    so we initialize vitaGL ourselves before SDL does.)
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
__attribute__((used)) char sceUserMainThreadName[] = "luanti_main";
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

// See "Memory layout" above
static constexpr int SYSTEM_RESERVE = 32 * 1024 * 1024;
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

	main_thread_id = sceKernelGetThreadId();
	startWatchdog();
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

	// MSAA is cheap-ish on a tile based GPU since resolving happens on chip,
	// but it still costs fill rate and memory, so it's opt-in.
	SceGxmMultisampleMode msaa = SCE_GXM_MULTISAMPLE_NONE;
	if (g_settings->get("antialiasing") == "fsaa")
		msaa = g_settings->getU16("fsaa") >= 4 ? SCE_GXM_MULTISAMPLE_4X :
			SCE_GXM_MULTISAMPLE_2X;

	// Same as SDL's vglInitExtended() call, which then becomes a no-op,
	// except for the amount of memory left to the system.
	sceSysmoduleLoadModule(SCE_SYSMODULE_IME);
	// Common dialog memory: keep all of it (vitaGL's private
	// SCE_KERNEL_MAX_MAIN_CDIALOG_MEM_SIZE) for the IME and message boxes.
	constexpr int CDIALOG_MEM_SIZE = 0x8C6000;
	vglInitWithCustomThreshold(0, 960, 544, SYSTEM_RESERVE, 0, 0,
		CDIALOG_MEM_SIZE, msaa);

	infostream << "Vita: free memory after vitaGL init: "
		<< (getFreeMemory() >> 20) << " MiB" << std::endl;
}

/*
	Testing aid for emulators that can't deliver controller input (Vita3K's
	plain SDL frontend): plays back gamepad events from the file named by
	the vita_input_script setting. One event per line:
	  <ms since start> axis <SDL axis 0-5> <value -32768..32767>
	  <ms since start> button <SDL button 0-20> <0|1>
	Lines starting with # are ignored.
*/
static int inputScriptThread(SceSize, void *argp)
{
	const std::string path = *static_cast<std::string **>(argp)[0];
	delete static_cast<std::string **>(argp)[0];
	std::ifstream is(path);
	if (!is.good()) {
		errorstream << "vita_input_script: can't open " << path << std::endl;
		return 0;
	}
	const u32 start = SDL_GetTicks();
	std::string line;
	while (std::getline(is, line)) {
		if (line.empty() || line[0] == '#')
			continue;
		std::istringstream ls(line);
		u32 at;
		std::string kind;
		int id, value;
		if (!(ls >> at >> kind >> id >> value))
			continue;
		while (SDL_GetTicks() - start < at)
			sceKernelDelayThread(10 * 1000);
		SDL_Event ev{};
		if (kind == "axis") {
			ev.type = SDL_CONTROLLERAXISMOTION;
			ev.caxis.which = 0;
			ev.caxis.axis = id;
			ev.caxis.value = value;
		} else if (kind == "button") {
			ev.type = value ? SDL_CONTROLLERBUTTONDOWN : SDL_CONTROLLERBUTTONUP;
			ev.cbutton.which = 0;
			ev.cbutton.button = id;
			ev.cbutton.state = value ? SDL_PRESSED : SDL_RELEASED;
		} else {
			continue;
		}
		SDL_PushEvent(&ev);
		infostream << "vita_input_script: " << line << std::endl;
	}
	return 0;
}

static void startInputScript()
{
	std::string path;
	if (!g_settings->getNoEx("vita_input_script", path) || path.empty())
		return;
	SceUID thid = sceKernelCreateThread("luanti_input_script", inputScriptThread,
			0x10000100, 64 * 1024, 0, SCE_KERNEL_CPU_MASK_USER_1 | SCE_KERNEL_CPU_MASK_USER_2,
			nullptr);
	if (thid < 0)
		return;
	std::string *arg = new std::string(path);
	sceKernelStartThread(thid, sizeof(arg), &arg);
}

void postVideoInit()
{
	startInputScript();

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
		Core assignment. The main thread (rendering, client) owns core 0;
		every other thread runs on cores 1 and 2. The scheduler is strictly
		priority based, so a busy thread must never share a core with the
		main thread, and no thread may have a higher priority than it.
		(The system core is not available to applications.)
	*/
	const int mask = SCE_KERNEL_CPU_MASK_USER_1 | SCE_KERNEL_CPU_MASK_USER_2;
	// All workers get the same priority: with strict priority scheduling and
	// no priority inheritance, a busy higher priority worker would starve a
	// lower priority one holding a lock that the main thread needs (see
	// yieldCpu()). That stalled the game while loading.
	const int prio = 160;

	SceUID thid = sceKernelGetThreadId();
	int ret = sceKernelChangeThreadCpuAffinityMask(thid, mask);
	if (ret < 0)
		warningstream << "Thread " << name << ": setting the CPU affinity failed: 0x"
			<< std::hex << ret << std::dec << std::endl;
	ret = sceKernelChangeThreadPriority(thid, prio);
	if (ret < 0)
		warningstream << "Thread " << name << ": setting the priority failed: 0x"
			<< std::hex << ret << std::dec << std::endl;
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

namespace {
	constexpr u32 CHECKPOINT_HISTORY = 16;
	std::atomic<const char *> checkpoints[CHECKPOINT_HISTORY];
	std::atomic<u32> checkpoint_count{0};

	int watchdogThread(SceSize, void *)
	{
		u32 last_count = checkpoint_count.load();
		int stalled_s = 0;
		bool reported = false;
		while (true) {
			sceKernelDelayThread(1000 * 1000);
			const u32 count = checkpoint_count.load();
			// Only the game records checkpoints, not the main menu
			if (count == 0)
				continue;
			if (count != last_count) {
				if (reported)
					errorstream << "Watchdog: main thread is running again" << std::endl;
				last_count = count;
				stalled_s = 0;
				reported = false;
				continue;
			}
			if (++stalled_s < 10 || reported)
				continue;
			reported = true;
			std::ostringstream os;
			os << "Watchdog: main thread stalled for 10 s, last checkpoints (oldest first):";
			for (u32 i = 0; i < CHECKPOINT_HISTORY; i++) {
				const char *c = checkpoints[(count + i) % CHECKPOINT_HISTORY].load();
				if (c)
					os << "\n  " << c;
			}
			errorstream << os.str() << std::endl;
		}
		return 0;
	}
}

static SceUID main_thread_id = -1;

void checkpoint(const char *where)
{
	// Only the main thread's progress matters (shared code calls this too)
	if (sceKernelGetThreadId() != main_thread_id)
		return;
	const u32 n = checkpoint_count.fetch_add(1, std::memory_order_relaxed);
	checkpoints[n % CHECKPOINT_HISTORY].store(where, std::memory_order_relaxed);
}

void startWatchdog()
{
	SceUID thid = sceKernelCreateThread("luanti_watchdog", watchdogThread,
			0x10000100, 64 * 1024, 0, SCE_KERNEL_CPU_MASK_USER_1 | SCE_KERNEL_CPU_MASK_USER_2,
			nullptr);
	if (thid >= 0)
		sceKernelStartThread(thid, 0, nullptr);
}

void yieldCpu()
{
	sceKernelDelayThread(100);
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
