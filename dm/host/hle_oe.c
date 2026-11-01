// nn::oe (application operating environment): on the real device the console state is queried through the am/oe services; the host gives fixed state directly.
// oe::Initialize is in hle_sdkinit.c, not here.
// Environment switches: DM_OPMODE=0 handheld / 1 console (default: the profile's op_mode, console unless set; console corresponds to the console-mode render profile); DM_LANG=language code (default en-US; the iOS app sets it from the IPA's DMLanguage, main.m).
#include "hle.h"

// The operation mode is runtime state (the front end can change it: iOS overheat downgrade switches to handheld): the single source is here, taken once from DM_OPMODE at startup, the environment variable is not read again afterwards
#include <stdatomic.h>
static _Atomic int g_opmode = -1;
int dm_opmode(void) {
    int m = g_opmode;
    if (m < 0) { const char *e = getenv("DM_OPMODE"); int v = e ? atoi(e) : DM_GAME_OPMODE; int exp = -1; atomic_compare_exchange_strong(&g_opmode, &exp, v); m = g_opmode; }
    return m;
}
// ---------- System notification queue (AppletMessage): front end state changes → fetched by the game every frame with TryPopNotificationMessage ----------
// Message numbers the host sends: 15 focus change, 30 operation mode change, 31 performance mode change
enum { MSG_FOCUS = 15, MSG_OPMODE = 30, MSG_PERFMODE = 31 };
#define MQ 32
static pthread_mutex_t g_mq_lock = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_mq_cond = PTHREAD_COND_INITIALIZER;   // cond: wakes the blocking PopNotificationMessage
static uint32_t g_mq[MQ]; static int g_mqh, g_mqn;
static _Atomic int g_ntf_opmode, g_ntf_perfmode;          // only pushed after the game turns it on with Set*NotificationEnabled
// Notification event (Hades main+0x3ed18c polls it every frame and pops one message each time it fires): the SDK's version returns the event oe::Initialize would have
// created, and the host's oe::Initialize is a no-op, so TryWaitSystemEvent asserted on it. A host-owned auto-clear SystemEvent instead, which follows the queue as the am service's
// does: signalled while a notification is queued, cleared when the last one is popped. Monster Hunter Rise's MessageSuperviseThread (main+0x6bc8fc4) waits on it with WaitAny,
// which does not clear it, then pops until the queue is empty; when the event was only ever signalled, the first focus change on iOS left it set and the thread spun on a core
// (995 ms/s, 5-6 W on an iPhone). The count is read and the event set or cleared under g_mq_lock, so a push racing a pop cannot leave it cleared over a queued notification
// (lock order g_mq_lock → G → g_se_lock; nothing takes g_mq_lock while holding G)
static uint8_t g_mq_ev[0x80] __attribute__((aligned(16)));
static _Atomic int g_mq_ev_on;
static void mq_ev_sync(void) {
    if (!atomic_load(&g_mq_ev_on)) return;
    pthread_mutex_lock(&g_mq_lock);
    if (g_mqn) hle_signal_system_event(g_mq_ev); else hle_clear_system_event(g_mq_ev);
    pthread_mutex_unlock(&g_mq_lock);
}
static void mq_push_(uint32_t m);
static void mq_push(uint32_t m) { mq_push_(m); mq_ev_sync(); }
static void mq_push_(uint32_t m) {
    pthread_mutex_lock(&g_mq_lock);
    // 15/30/31 are invalidation notifications meaning "query the current state", not events carrying historical values.
    // While one of the same type has not been taken yet, the first queue position is kept: a consumer loop only reads the latest state, repeated notifications write back the same thing, and with the same size there is no further resize.
    // Only these three state notifications are merged; future per-occurrence events (such as Resume/Exit) must not use this rule.
    // Legitimate producers only send these three, so at most three items are pending, and any number of focus losses/restores no longer hits the 32-slot limit; after a pop, new changes are enqueued as usual.
    if (m == MSG_FOCUS || m == MSG_OPMODE || m == MSG_PERFMODE)
        for (int i = 0; i < g_mqn; i++)
            if (g_mq[(g_mqh + i) % MQ] == m) { pthread_mutex_unlock(&g_mq_lock); return; }
    if (g_mqn == MQ) die("oe notification queue overflow (is the game not popping?)");
    g_mq[(g_mqh + g_mqn++) % MQ] = m;
    pthread_cond_broadcast(&g_mq_cond);
    pthread_mutex_unlock(&g_mq_lock);
}
void dm_set_opmode(int m) {   // called by the front end (overheat downgrade etc.): change the state and push 30 / 31 according to the switches the game has turned on
    int old = dm_opmode(); if (old == m) return;
    g_opmode = m;
    LOG("oe operation mode %d → %d", old, m);
    if (g_ntf_opmode) mq_push(MSG_OPMODE);
    if (g_ntf_perfmode) mq_push(MSG_PERFMODE);
}
// FocusState: 1 InFocus, 2 OutOfFocus, 3 Background. The front end calls dm_focus_set (iOS lifecycle / Mac window losing focus)
static _Atomic int g_focus = 1; static _Atomic uint64_t g_focus_in_ns;   // the front end's state; time of its most recent entry into InFocus
// Focus handling mode the game set (-1: none). The SDK's oe::SetFocusHandlingMode (SDK 18 sdk+0x3c5770) turns it into am's SetFocusHandlingMode(notify, ?, suspend) and
// SetOutOfFocusSuspendingEnabled: 0 → (0, 0, 1), 1 → (1, 1, 0), 2 → (1, 0, 1), 3 → (0, 0, 1) + out-of-focus suspending. Only 1 and 2 ask for focus notifications; under 0 and 3
// the system suspends the game out of focus and it never sees message 15 (the host's suspend is the guest clock freeze and the graphics gate). Super Mario Galaxy 2 sets 3, and
// its Wii layer's pause on focus 2 (main+0x663ba0) never finished: the main loop waits for the GX thread to pause (main+0x5ce870), which waits for work the paused CPU never sends
static _Atomic int g_focus_mode = -1;
static int focus_notifies(void) { int m = atomic_load(&g_focus_mode); return m != 0 && m != 3; }   // no mode set: notify, as before
// Library applets the game has open (dm_applet_enter / dm_applet_leave, DM_APPLET in hle.h). On Horizon an applet the application starts takes its focus: am sends message 15
// with OutOfFocus when it opens (libnx: AppletFocusState_OutOfFocus, "LibraryApplet open") and 15 with InFocus when it closes, and the application's Show call returns once it
// has closed. The host's stand-ins answer on the caller's thread, so the game sees that pair around the call. Mario Kart 8 Deluxe applies the controller applet's answer only
// after the InFocus notification (its notification thread main+0x8ea560 sets the flag main+0x87b5d0 takes each frame); with none its 2P character select never took input
// (GitHub issue #550). The game sees the front end's state, or 2 while an applet is open over an application in focus; a change of what it sees is one notification 15
static int g_applets; static _Atomic int g_seen = 1; static pthread_mutex_t g_seen_lock = PTHREAD_MUTEX_INITIALIZER;
static void focus_sync(int applets) {   // applets: +1 an applet opens, -1 one closes, 0 the front end's state changed
    pthread_mutex_lock(&g_seen_lock);
    g_applets += applets;
    if (g_applets < 0) die("dm_applet_leave without dm_applet_enter");
    int w = atomic_load(&g_focus), s = w == 1 && g_applets ? 2 : w, changed = atomic_exchange(&g_seen, s) != s;
    pthread_mutex_unlock(&g_seen_lock);
    if (!changed) return;   // pushed outside the lock: 15 only says "query the state again", so pushes from two threads need no order
    LOG("oe focus state → %d%s", s, applets > 0 ? " (library applet open)" : applets < 0 ? " (library applet closed)" : "");
    if (focus_notifies()) mq_push(MSG_FOCUS);
}
void dm_applet_enter(const char *name) { LOG("oe library applet: %s", name); focus_sync(1); }
void dm_applet_leave(void) { focus_sync(-1); }
int dm_focus_now(void) { return atomic_load(&g_focus); }   // the front end's state, not an applet's: the display link follows the window
uint64_t dm_focus_in_since(void) { return atomic_load(&g_focus_in_ns); }   // graphics layer display link watchdog: stalls are only computed while active, counted from activation (nvn_mtl.m mtl_present_wait)
void dm_focus_set(int st) {
    if (st < 1 || st > 3) die("dm_focus_set: invalid state %d", st);
    if (st == 1) atomic_store(&g_focus_in_ns, now_ns());
    if (atomic_exchange(&g_focus, st) == st) return;
    focus_sync(0);
}
static int opmode(void) { return dm_opmode(); }

// Screenshot / video recording switches: the host has no system screenshot service, no-op
HLE_FORCE("_ZN2nn2oe17FinishStartupLogoEv", "dismiss the system boot screen (through the am application proxy); the host has no boot screen", void, h_oe_FinishLogo, void) {}   // Metroid Dread calls it at main+0xadfed0
// am retains the requested inactivity extension even when auto-sleep is disabled.
// SDK 12.3.9 maps the normal setter to mode 1, Unsafe to mode 2, and false to 0.
// Xenoblade Chronicles 3 reads it after the opening movie (sdk+0x3391f0).
static _Atomic int g_inactivity_mode;
HLE_FORCE("_ZN2nn2oe44SetUserInactivityDetectionTimeExtendedUnsafeEb", "inactivity extension mode 2 or 0; the host never turns the screen off", void, h_oe_InactivityExt, unsigned on) { g_inactivity_mode = (on & 1) ? 2 : 0; }
HLE_FORCE("_ZN2nn2oe38SetUserInactivityDetectionTimeExtendedEb", "inactivity extension mode 1 or 0; the host never turns the screen off", void, h_oe_InactivityExtSafe, unsigned on) { g_inactivity_mode = on & 1; }
HLE_FORCE("_ZN2nn2oe43GetUserInactivityDetectionTimeExtensionModeEv", "inactivity extension mode requested by the application", int, h_oe_InactivityMode, void) { return g_inactivity_mode; }
HLE_FORCE("_ZN2nn2oe15EnableRecordingEv", "video recording switch; the host has no system recording", void, h_oe_EnableRec, void) {}
// Applet resource user ID (ARUID, the number am assigns to this process): the host has no am, and the SDK's own getter returns 0. SDK 20's OpenAudioRenderer (the CPU rendering + manual execution path, sdk+0x221730)
//   aborts when it gets 0 (The Legend of Zelda: Breath of the Wild 1.9.0); SDK 16 did not check. Report a fixed non-zero ID, used only as an identifier
HLE_FORCE("_ZN2nn6applet23GetAppletResourceUserIdEv", "applet resource user ID; the host returns a fixed non-zero value", uint64_t, h_applet_Aruid, void) { return 0x80; }
// Launch parameter (present only when another program launched this one with one; returns bool): the host never passes one
HLE_FORCE("_ZN2nn2oe21TryPopLaunchParameterEPmPvm", "launch parameter: none", unsigned, h_oe_PopLaunchParam, size_t *out, void *buf, size_t n) { (void)buf; (void)n; if (out) *out = 0; return 0; }
// Tears of the Kingdom (SDK 20.5.22): the index of the program that launched this one in a multi-program application. The SDK asks the applet proxy, which the host never opens
// (null read in applet::GetPreviousProgramIndex right after the window setup); -1 = launched from the home menu
HLE_FORCE("_ZN2nn2oe23GetPreviousProgramIndexEv", "previous program index: launched from HOME, -1", int, h_oe_PrevProgram, void) { return -1; }
// Launch another program of a multi-program application (Octopath Traveler 0 imports it); on hardware it does not return. The host has only this program: logged, and the game carries on
HLE_FORCE("_ZN2nn2oe14ExecuteProgramEiPKvm", "launch another program of the application: the host runs only this one (logged)", void, h_oe_ExecProgram, int index, const void *param, size_t n) {
    (void)param; LOG("[RED] oe::ExecuteProgram(%d, %zu bytes of parameters): the host runs only this program; the game carries on", index, n);
}
HLE_FORCE("_ZN2nn2oe16DisableRecordingEv", "video recording switch; the host has no system recording", void, h_oe_DisableRec, void) {}
// Silence the game's sound in system video captures (through am; FINAL FANTASY RESONANCE imports both): the host records nothing
HLE_FORCE("_ZN2nn2oe13MuteRecordingEv", "mute the sound of system video recording; the host has no system recording", void, h_oe_MuteRec, void) {}
HLE_FORCE("_ZN2nn2oe15UnmuteRecordingEv", "unmute the sound of system video recording; the host has no system recording", void, h_oe_UnmuteRec, void) {}
HLE_FORCE("_ZN2nn2oe16EnableScreenShotEv", "screenshot switch; the host has no system screenshot", void, h_oe_EnableShot, void) {}
HLE_FORCE("_ZN2nn2oe17DisableScreenShotEv", "screenshot switch; the host has no system screenshot", void, h_oe_DisableShot, void) {}
// Super Mario Odyssey: HOME button blocking around its own menus, screenshot orientation, and the auto-sleep query (all through am)
HLE_FORCE("_ZN2nn2oe23BeginBlockingHomeButtonEv", "temporarily block the HOME button (through am); the host has no HOME button", void, h_oe_BlockHome, void) {}
HLE_FORCE("_ZN2nn2oe21EndBlockingHomeButtonEv", "restore the HOME button (through am)", void, h_oe_UnblockHome, void) {}
HLE_FORCE("_ZN2nn2oe29SetScreenShotImageOrientationENS_5album16ImageOrientationE", "screenshot orientation (through am); the host has no system screenshot", void, h_oe_ShotOrient, int o) { (void)o; }
// DRAGON QUEST XI S: auto-sleep disabled sections (through am). The host never sleeps the device; the query reports whether a section is open, as the applet does
static _Atomic int g_nosleep;
HLE_FORCE("_ZN2nn2oe29BeginAutoSleepDisabledSectionEv", "open an auto-sleep disabled section (through am); the host never auto-sleeps, counted for the query", void, h_oe_NoSleepBegin, void) { g_nosleep++; }
HLE_FORCE("_ZN2nn2oe27EndAutoSleepDisabledSectionEv", "close an auto-sleep disabled section (through am)", void, h_oe_NoSleepEnd, void) { if (g_nosleep > 0) g_nosleep--; }
HLE_FORCE("_ZN2nn2oe19IsAutoSleepDisabledEv", "whether an auto-sleep disabled section is open", unsigned, h_oe_NoSleepQ, void) { return g_nosleep > 0; }
HLE_FORCE("_ZN2nn2oe37IsUserInactivityDetectionTimeExtendedEv", "whether the application requested an inactivity extension", unsigned, h_oe_InactivityExtQ, void) { return g_inactivity_mode != 0; }
// Hyrule Warriors: Age of Calamity (SDK 9.6.0, main+0x8d1238): the copyright frame buffer, an overlay the system draws only into screenshots and recordings. The SDK hands the buffer to am as transfer memory
// (svcCreateTransferMemory, then a null applet proxy). The host has no system capture, so it keeps nothing
HLE_FORCE("_ZN2nn2oe30InitializeCopyrightFrameBufferEPvm", "copyright overlay buffer (for screenshots / recording, through am); the host has no system screenshot", void, h_oe_CopyrightInit, void *buf, size_t n) { (void)buf; (void)n; }
HLE_FORCE("_ZN2nn2oe17SetCopyrightImageEPKvmiiiiNS0_16WindowOriginModeE", "copyright overlay image; the host has no system screenshot", void, h_oe_CopyrightImage, const void *p, size_t n, int x, int y, int w, int h, int origin) { (void)p; (void)n; (void)x; (void)y; (void)w; (void)h; (void)origin; }
HLE_FORCE("_ZN2nn2oe22SetCopyrightVisibilityEb", "copyright overlay visibility; the host has no system screenshot", void, h_oe_CopyrightVisible, int on) { (void)on; }

// OperationMode: 0 Handheld, 1 Console; PerformanceMode: 0 Normal, 1 Boost (one-to-one with the operation mode)
HLE_FORCE("_ZN2nn2oe16GetOperationModeEv", "operation mode query; a fixed value from DM_OPMODE", int, h_oe_GetOpMode, void) { return opmode(); }
// Profile perf_mode boost: Boost whatever the operation mode, a combination a Switch 1 never reports, so a profile choice and not a default. Rhythm Heaven Groove keeps one flag per mode
//   (main+0xa764 / main+0xa784): console means a TV, which gets a latency calibration (main+0x20d0, scene script main+0x4c0820), and Boost alone picks its 1920x1080 draw size
//   (table main+0xada6d0, else 1280x720). Handheld with Boost: no TV calibration on a phone, full-size drawing
HLE_FORCE("_ZN2nn2oe18GetPerformanceModeEv", "performance mode query; same as the operation mode (profile perf_mode boost: Boost)", int, h_oe_GetPerfMode, void) { return DM_GAME_PERF_BOOST ? 1 : opmode(); }
// SDK 11 calls of Monster Hunter Rise: the CPU overclock query reads an apm session the host never opened (null read in sdk+0x35258 at the start of nnMain, main+0x6bc8650);
// the default display resolution is the TV's (1080p docked) or the panel's (720p handheld) and only changes with the operation mode
// Taiko Risshiden V DX (SDK 12.3.3) turns it on in nnMain (main+0x2e1e4c): the setter goes through the same apm session (null read at sdk+0x362b8). The host does not scale clocks;
// the query reports what was set, as apm does
static unsigned char g_cpu_oc;
HLE_FORCE("_ZN2nn2oe21IsCpuOverclockEnabledEv", "CPU overclock state (through apm): what SetCpuOverclockEnabled set, off at start", unsigned, h_oe_CpuOc, void) { return g_cpu_oc; }
HLE_FORCE("_ZN2nn2oe22SetCpuOverclockEnabledEb", "CPU overclock switch (through apm); the host does not scale clocks", void, h_oe_SetCpuOc, unsigned on) { g_cpu_oc = on & 0xff; }
HLE_FORCE("_ZN2nn2oe27GetDefaultDisplayResolutionEPiS1_", "default display resolution: 1920x1080 docked, 1280x720 handheld", void, h_oe_DefRes, int *w, int *h) {
    int con = opmode() == 1; if (w) *w = con ? 1920 : 1280; if (h) *h = con ? 1080 : 720;
}
HLE_FORCE("_ZN2nn2oe38GetDefaultDisplayResolutionChangeEventEPNS_2os11SystemEventE", "default display resolution change event: bound, never signaled (the operation mode notification carries the change)",
          void, h_oe_DefResEv, void *ev) { hle_bind_system_event(ev, 0); }
HLE_FORCE("_ZN2nn2oe40SendServerMaintenanceOverlayNotificationENS_4time9PosixTimeES2_", "server maintenance overlay (through am); the host has no system overlay", void, h_oe_MaintOverlay, int64_t a, int64_t b) { (void)a; (void)b; }
// apm performance configurations: Normal 0x00010000 (CPU 1020 / GPU 384 / EMC 1600 MHz, the handheld default), Boost 0x00010001 (GPU 768 MHz, the docked default)
HLE_FORCE("_ZN2nn2oe27GetPerformanceConfigurationENS0_15PerformanceModeE", "query the CPU/GPU clock config (through apm); reports the default for the mode", int, h_oe_GetPerfCfg, int mode) { return mode == 1 ? 0x00010001 : 0x00010000; }
HLE_FORCE("_ZN2nn2oe24SetAlbumImageOrientationENS_5album16ImageOrientationE", "screenshot orientation (through am); the host has no system screenshot", void, h_oe_AlbumOrient, int o) { (void)o; }
// The Great Ace Attorney Chronicles (SDK 10), first thing in nnMain (main+0x6b188c): the SDK's version asks am through the applet proxy the host never made (null read at sdk+0x338070)
HLE_FORCE("_ZN2nn2oe34DisableAlbumImageTakenNotificationEv", "screenshot-taken notifications (through am); the host takes no system screenshots", void, h_oe_AlbumNotifyOff, void) {}
// Enable uses the same am application proxy (SDK 13.3.5 sdk+0x36ec50); the host produces no system-album notifications.
// Xenoblade Chronicles 3 (SDK 12.3.9) enables this at main+0x127fac4;
// the SDK otherwise dereferences the absent am SelfController at sdk+0x33b0f0.
// As with Disable, there is no producer: the host has no Horizon album captures.
HLE_FORCE("_ZN2nn2oe33EnableAlbumImageTakenNotificationEv", "screenshot-taken notifications (through am); the host takes no system screenshots", void, h_oe_AlbumNotifyOn, void) {}
HLE_FORCE("_ZN2nn2oe24IsInStoreDemoModeEnabledEv", "retail store demo mode (through am); never", unsigned, h_oe_StoreDemo, void) { return 0; }
HLE_FORCE("_ZN2nn2oe27SetPerformanceConfigurationENS0_15PerformanceModeEi", "set the CPU/GPU clock config; the host does not scale clocks", void, h_oe_SetPerfCfg, int mode, int cfg) {
    LOG("oe::SetPerformanceConfiguration mode=%d cfg=%#x (ignored)", mode, cfg);
}

// Scripted state changes (verify lifecycle / overheat downgrade without a window): DM_OE_SCRIPT="focus=3@10,focus=1@20,opmode=0@30" (second N); bg=1 / bg=0 run the whole background round trip (see script_bg)
// Time counts from the game's first notification fetch. Coexists with front end calls.
#include <unistd.h>
// bg=1 / bg=0: the whole "to background / back to foreground" sequence, in the same order as the scene lifecycle in front_ios.m (resign active -> enter background; enter foreground -> become active), minus the audio session reset iOS does on returning.
// Used to reproduce "after going to the background and back" problems on the Mac (audio take-over and resume, guest clock freeze, graphics suspend); to imitate the process being suspended as well, send dmhost SIGSTOP / SIGCONT between the two items
static void script_bg(int on) {
    if (on) { dm_focus_set(2); dm_focus_set(3); dm_audio_suspend(1); guest_clock_freeze_for(1, 1); dm_gfx_suspend(1); }
    else { dm_gfx_suspend(0); guest_clock_freeze_for(1, 0); dm_audio_suspend(0); dm_focus_set(2); dm_focus_set(1); }
}
static void *oe_script(void *arg) {
    char *buf = arg; uint64_t t0 = now_ns();
    for (char *save, *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        char key[16]; int v; double at;
        if (sscanf(tok, "%15[a-z]=%d@%lf", key, &v, &at) != 3) die("DM_OE_SCRIPT entry '%s' should be key=value@seconds", tok);
        uint64_t due = t0 + (uint64_t)(at * 1e9), now = now_ns();
        if (due > now) usleep((useconds_t)((due - now) / 1000));
        if (!strcmp(key, "focus")) dm_focus_set(v);
        else if (!strcmp(key, "opmode")) dm_set_opmode(v);
        else if (!strcmp(key, "bg")) script_bg(v);
        else die("DM_OE_SCRIPT unknown key '%s' (focus / opmode / bg)", key);
    }
    return NULL;
}
static void oe_script_start(void) {
    const char *e = getenv("DM_OE_SCRIPT"); if (!e) return;
    pthread_t th; if (pthread_create(&th, NULL, oe_script, strdup(e))) die("DM_OE_SCRIPT thread");
    pthread_detach(th);
}
static pthread_once_t g_oe_once = PTHREAD_ONCE_INIT;

HLE_FORCE("_ZN2nn2oe20GetCurrentFocusStateEv", "foreground / background state; the front end's dm_focus_set, out of focus while a library applet is open", int, h_oe_GetFocus, void) { return atomic_load(&g_seen); }
HLE_FORCE("_ZN2nn2oe20SetFocusHandlingModeENS0_17FocusHandlingModeE", "focus handling mode; 1 and 2 get focus notifications, 0 and 3 none (see g_focus_mode)", void, h_oe_SetFocusMode, int m) {
    atomic_store(&g_focus_mode, m); LOG("oe::SetFocusHandlingMode %d (%s)", m, focus_notifies() ? "focus notifications" : "suspended out of focus, no focus notifications");
}

// Notification: returns bool (w0), the host has no system notifications
HLE_FORCE("_ZN2nn2oe25TryPopNotificationMessageEPj", "system notification queue; front end state changes are queued", unsigned, h_oe_TryPopMsg, uint32_t *msg) {
    pthread_once(&g_oe_once, oe_script_start);
    pthread_mutex_lock(&g_mq_lock);
    int ok = g_mqn > 0;
    if (ok) { *msg = g_mq[g_mqh]; g_mqh = (g_mqh + 1) % MQ; g_mqn--; }
    pthread_mutex_unlock(&g_mq_lock);
    if (ok) LOG("oe notification popped by the game: %u", *msg);   // state changes are rare, printing each one does not flood the log
    mq_ev_sync();   // signalled again while notifications remain, cleared when the queue is empty (also a signal left behind by a pop that found it empty)
    return ok;
}
// The blocking form (Link's Awakening pops on a thread of its own, main+0xfa1ff8): the SDK's version waits on the system event oe::Initialize would have created,
// and the host's oe::Initialize is a no-op, so it aborted on an uninitialized event. Wait for the host queue instead, then take the message the Try path's way
HLE_FORCE("_ZN2nn2oe22PopNotificationMessageEv", "system notification queue (blocking); waits on the host queue", uint32_t, h_oe_PopMsg, void) {
    pthread_once(&g_oe_once, oe_script_start);   // DM_OE_SCRIPT also for games that only pop this way (Mario Kart 8 Deluxe): the wait below reaches the Try path only once a notification is queued
    uint32_t m = 0;
    for (;;) {
        pthread_mutex_lock(&g_mq_lock);
        while (!g_mqn) {
            dm_thread_native_wait(1);
            pthread_cond_wait(&g_mq_cond, &g_mq_lock);
            dm_thread_native_wait(0);
        }
        pthread_mutex_unlock(&g_mq_lock);
        if (h_oe_TryPopMsg(&m)) return m;   // another thread may have taken it in between: wait again
    }
}
HLE_FORCE("_ZN2nn2oe27GetNotificationMessageEventEv", "notification event: the host's auto-clear SystemEvent, signalled while the queue holds notifications", void *, h_oe_GetMsgEv, void) {
    pthread_once(&g_oe_once, oe_script_start);   // DM_OE_SCRIPT also for games that wait on the event and pop only once it fires (Monster Hunter Rise)
    if (!atomic_load(&g_mq_ev_on)) { hle_bind_system_event(g_mq_ev, 1); atomic_store(&g_mq_ev_on, 1); mq_ev_sync(); }   // bound on the game's thread (it runs SDK code)
    return g_mq_ev;
}
// bool arguments: Switch callers do not extend them, declared as unsigned and only the low 8 bits are looked at
HLE_FORCE("_ZN2nn2oe28SetResumeNotificationEnabledEb", "resume notification switch; the host expresses resume as focus notification 15", void, h_oe_SetResumeNtf, unsigned on) { (void)on; }
HLE_FORCE("_ZN2nn2oe42SetOperationModeChangedNotificationEnabledEb", "operation mode change notification switch", void, h_oe_SetOpModeNtf, unsigned on) { g_ntf_opmode = on & 0xff; }
HLE_FORCE("_ZN2nn2oe44SetPerformanceModeChangedNotificationEnabledEb", "performance mode change notification switch", void, h_oe_SetPerfModeNtf, unsigned on) { g_ntf_perfmode = on & 0xff; }

// nn::settings::LanguageCode = struct { char string[8]; }, 8 bytes returned by value in x0
HLE_FORCE("_ZN2nn2oe18GetDesiredLanguageEv", "system language query; value from DM_LANG", uint64_t, h_oe_GetLang, void) {
    const char *e = getenv("DM_LANG");
    char code[8] = {0};
    strncpy(code, e && *e ? e : "en-US", sizeof code - 1);
    uint64_t v; memcpy(&v, code, 8);
    return v;
}

// The console's region (Animal Crossing: New Horizons 3.0.3): the SDK asks settings::system::GetRegionCode, which reaches set:sys. A region that fits the language the game is given:
// 0 Japan, 1 the Americas, 2 Europe, 4 Hong Kong / Taiwan / Korea (nn::settings::system::RegionCode; 3 Australia and 5 China are left out, no language points at them alone)
HLE_FORCE("_ZN2nn2oe13GetRegionCodeEv", "console region (set:sys); from DM_LANG", int, h_oe_GetRegion, void) {
    const char *e = getenv("DM_LANG"); if (!e || !*e) e = "en-US";
    int r = !strncmp(e, "ja", 2) ? 0 : !strncmp(e, "ko", 2) || !strncmp(e, "zh", 2) ? 4 : !strcmp(e, "en-US") || !strcmp(e, "fr-CA") || !strcmp(e, "es-419") || !strcmp(e, "pt-BR") ? 1 : 2;
    static int once; if (!once++) LOG("oe::GetRegionCode: %d for language %s", r, e);
    return r;
}

// nn::util::Uuid 16 bytes: a fixed local value (only used to tell devices apart locally, networking is cut)
HLE_FORCE("_ZN2nn2oe17GetPseudoDeviceIdEPNS_4util4UuidE", "pseudo device ID; a fixed host value", void, h_oe_GetDevId, uint8_t *uuid) {
    static const uint8_t k[16] = {0x78, 0x62, 0x6c, 0x00, 0x53, 0x57, 0x4e, 0x41, 0x54, 0x49, 0x56, 0x45, 0x31, 0x30, 0x30, 0x31};
    memcpy(uuid, k, 16);
}

// Projection aspect gate (OBS_PROJ_ASPECT, a profile's patches.py): at the entry of a perspective projection's matrix update (Animal Crossing: New Horizons 3.0.3, sead's
// PerspectiveProjection at main+0x37e5968: x0 the projection, its aspect at +0xb0), an aspect of 16:9 becomes the screen's width / height. Every perspective projection goes through
// it whoever set its aspect, the game's cameras and its layouts alike; one of another shape (a square icon render) keeps it, and a corrected one is no longer 16:9, so it is not
// corrected twice. The field itself changes, so what else reads it (culling) sees the wider view too. The game then draws a wider view into the same 16:9 picture, squeezed by
// 16:9 / aspect, and the front end stretches the picture over the whole screen (front_ios.m, nvn_mtl.m present_viewport; profile screen_fill): the 3D view comes out wider at its
// own proportions and the layouts at theirs, centred. 0 from the front end (the Mac; a screen not wider than 16:9: an iPad) leaves every projection as it is
#include "front.h"
#include <string.h>
#include <math.h>
static void proj_aspect(uint64_t *r, int off) {
    static double a = -1; static int fixed;
    if (a < 0) {
        const char *e = getenv("DM_SCREEN_ASPECT");   // a test value (the Mac, whose front end keeps 16:9): width / height, e.g. 2.1674
        a = e && atof(e) > 0 ? atof(e) : front_screen_aspect();
        if (a <= 16.0 / 9 + 1e-3) a = 0;
        LOG("front end: projection aspect gate: %s", a ? "16:9 perspective projections take the screen's aspect" : "the game keeps its 16:9");
        if (a) LOG("front end: projection aspect %.4f", a);
    }
    static int off_ = -1; if (off_ < 0) off_ = getenv("DM_PROJ_ASPECT_OFF") ? atoi(getenv("DM_PROJ_ASPECT_OFF")) : 0;   // diagnostic: =1 leaves perspective projections at 16:9
    if (!a || !r[0] || off_) return;
    float *f = (float *)(uintptr_t)(r[0] + (unsigned)off);
    static int trace = -1, traced; if (trace < 0) trace = getenv("DM_PROJ_TRACE") && atoi(getenv("DM_PROJ_TRACE"));   // DM_PROJ_TRACE=1: the first hits, with the aspect found
    if (trace && traced < 40) { LOG("[proj] aspect gate: %#llx aspect %.4f", (unsigned long long)r[0], *f); traced++; }
    if (fabsf(*f / (16.0f / 9) - 1) > 0.01f) return;
    *f = (float)a;
    if (fixed < 8) { LOG("front end: projection %#llx: aspect 16:9 -> %.4f", (unsigned long long)r[0], a); fixed++; }
}
// A projection that holds a finished matrix (Super Mario Bros. Wonder's course camera: a class whose matrix getter copies a stored 4x4 out, main+0x165360, with near / far /
// fovy / aspect kept beside it for the getters): the gate at that copy (OBS_PROJ_MATRIX; parameter: the aspect field's offset in its low 12 bits, the matrix's above them) scales
// the matrix's x scale m[0][0] by 16:9 / the screen's aspect when the aspect field reads 16:9, and sets the field to the screen's: the same wider view as the aspect gate gives a
// projection that builds its own matrix. A camera that writes its matrix again writes 16:9 again and is corrected again; one that does not is not corrected twice
static void proj_matrix(uint64_t *r, int param) {
    double a = front_screen_aspect();
    const char *e = getenv("DM_SCREEN_ASPECT"); if (e && atof(e) > 0) a = atof(e);
    if (a <= 16.0 / 9 + 1e-3 || !r[0]) return;
    float *asp = (float *)(uintptr_t)(r[0] + (unsigned)(param & 0xfff)), *m = (float *)(uintptr_t)(r[0] + (unsigned)(param >> 12));
    static int trace = -1, traced; if (trace < 0) trace = getenv("DM_PROJ_TRACE") && atoi(getenv("DM_PROJ_TRACE"));
    if (trace && traced < 12) {   // each object once: the four fields after the matrix, then the matrix's 16 floats
        static uint64_t seen[12]; int i = 0; while (i < traced && seen[i] != r[0]) i++;
        if (i == traced) {
            seen[traced++] = r[0]; const float *x = asp - 3;
            LOG("[proj] matrix gate: %#llx fields %.4f %.4f %.4f %.4f", (unsigned long long)r[0], x[0], x[1], x[2], x[3]);
            for (int k = 0; k < 4; k++) LOG("[proj]   %9.4f %9.4f %9.4f %9.4f", m[k * 4], m[k * 4 + 1], m[k * 4 + 2], m[k * 4 + 3]);
        }
    }
    static int all = -1; if (all < 0) all = getenv("DM_PROJ_MATRIX_ALL") && atoi(getenv("DM_PROJ_MATRIX_ALL"));
    if (all) {   // experiment (DM_PROJ_MATRIX_ALL=1): scale row 0 (clip x) of every stored matrix once per write, remembered per object by the m[0] it left
        static uint64_t obj[64]; static float left[64]; int i = 0, freei = -1;
        for (; i < 64 && obj[i] != r[0]; i++) if (!obj[i] && freei < 0) freei = i;
        if (i < 64 && m[0] == left[i]) return;
        if (i == 64) { if (freei < 0) return; i = freei; obj[i] = r[0]; }
        float k = (float)(16.0 / 9 / a); for (int c = 0; c < 4; c++) m[c] *= k;
        left[i] = m[0]; return;
    }
    if (fabsf(*asp / (16.0f / 9) - 1) > 0.01f) return;
    m[0] *= (float)(16.0 / 9 / a); *asp = (float)a;
    static int fixed;
    if (fixed < 8) { LOG("front end: projection %#llx (stored matrix): x scale x%.4f for aspect %.4f", (unsigned long long)r[0], 16.0 / 9 / a, a); fixed++; }
}
// A function given a finished row-major projection matrix (Super Mario Bros. Wonder's courses: main+0x16baa0 builds their uniform block from the camera at [x2 + 0x18],
// whose orthographic 4x4 projection is at +0x160): the gate at its entry (OBS_PROJ_ROW0; parameter: the register in bits 0-4, a pointer offset / 8 in bits 5-11 (0: the
// register holds the base itself, otherwise the base is the pointer at register + that offset), the matrix's offset from the base / 16 in bits 12-23) scales row 0, the one
// that makes clip x, by 16:9 / the screen's aspect when the matrix is 16:9 (|m11 / m00| within 1 % of 16:9) and a projection: last row (0, 0, ±1, 0) for a perspective,
// (0, 0, 0, 1) for an orthographic one. The scaled matrix is no longer 16:9, so one kept from frame to frame is not scaled twice
static void proj_row0(uint64_t *r, int param) {
    int reg = param & 31; unsigned ptr = (unsigned)(param >> 5 & 0x7f) * 8, off = (unsigned)(param >> 12 & 0xfff) * 16;
    double a = front_screen_aspect();
    const char *e = getenv("DM_SCREEN_ASPECT"); if (e && atof(e) > 0) a = atof(e);
    if (a <= 16.0 / 9 + 1e-3 || reg < 0 || reg > 29 || !r[reg]) return;
    uint64_t base = r[reg]; if (ptr) { base = *(const uint64_t *)(uintptr_t)(base + ptr); if (!base) return; }
    float *m = (float *)(uintptr_t)(base + off);
    static int trace = -1, traced; if (trace < 0) trace = getenv("DM_PROJ_TRACE") ? atoi(getenv("DM_PROJ_TRACE")) : 0;   // 2: orthographic matrices only, with the object they sit in
    if (trace && traced < 6 && (trace < 2 || m[15] == 1)) { traced++; for (int k = 0; k < 4; k++) LOG("[proj] row0 gate %p row %d: %9.4f %9.4f %9.4f %9.4f", (void *)m, k, m[k * 4], m[k * 4 + 1], m[k * 4 + 2], m[k * 4 + 3]);
        if (trace > 1 && m[15] == 1) { const float *b = (const float *)(uintptr_t)base; for (int k = 0; k < 0x280 / 16; k++) LOG("[proj]   base +%#05x %10.4f %10.4f %10.4f %10.4f", k * 16, b[k * 4], b[k * 4 + 1], b[k * 4 + 2], b[k * 4 + 3]); } }
    int persp = m[12] == 0 && m[13] == 0 && fabsf(m[14]) == 1 && m[15] == 0, ortho = m[12] == 0 && m[13] == 0 && m[14] == 0 && m[15] == 1;
    if (!(persp || ortho) || m[0] == 0 || fabsf(fabsf(m[5] / m[0]) / (16.0f / 9) - 1) > 0.01f) return;
    float k = (float)(16.0 / 9 / a); for (int c = 0; c < 4; c++) m[c] *= k;
    static int fixed;
    if (fixed < 8) { LOG("front end: projection matrix %p: row 0 x%.4f for aspect %.4f", (void *)m, (double)k, a); fixed++; }
}
// sead's OrthoProjection as a 3D camera takes it (Super Mario Bros. Wonder's courses: the camera update at main+0x119d80 is given the projection in x1, and takes its matrix,
// its culling frustum and its aspect from it): the gate at that update's entry (OBS_PROJ_ORTHO; parameter: the box's offset in the object, top / bottom / left / right floats,
// in bits 0-11, the register holding the object in bits 12-16, the offset of its matrix-dirty flag byte in bits 17-23) widens a 16:9 box about its centre to the screen's
// aspect and marks the matrix dirty, so the update makes the matrix, its inverse, the frustum planes and the aspect from the wider view. Only there: the same class also
// draws full-screen passes (a 1920 x 1080 box) and layouts, which must keep covering the 16:9 picture. A widened box is no longer 16:9: one kept from frame to frame is not
// widened twice, one the game sets again is widened again
static void proj_ortho(uint64_t *r, int param) {
    double a = front_screen_aspect();
    const char *e = getenv("DM_SCREEN_ASPECT"); if (e && atof(e) > 0) a = atof(e);
    unsigned off = (unsigned)param & 0xfff, reg = (unsigned)param >> 12 & 31, dirty = (unsigned)param >> 17 & 0x7f;
    if (a <= 16.0 / 9 + 1e-3 || reg > 29 || !r[reg]) return;
    float *b = (float *)(uintptr_t)(r[reg] + off), t = b[0], bt = b[1], l = b[2], rt = b[3];
    if (!(t > bt) || !(rt > l) || fabsf((rt - l) / (t - bt) / (16.0f / 9) - 1) > 0.01f) return;
    float c = (l + rt) / 2, h = (rt - l) / 2 * (float)(a / (16.0 / 9));
    b[2] = c - h; b[3] = c + h;
    if (dirty) *(uint8_t *)(uintptr_t)(r[reg] + dirty) = 1;
    static int fixed;
    if (fixed < 8) { LOG("front end: orthographic projection %#llx: box %.4f..%.4f -> %.4f..%.4f for aspect %.4f", (unsigned long long)r[reg], l, rt, b[2], b[3], a); fixed++; }
}
__attribute__((constructor)) static void proj_aspect_attach(void) {
    gate_obs_set(OBS_PROJ_ASPECT, proj_aspect); gate_obs_set(OBS_PROJ_MATRIX, proj_matrix); gate_obs_set(OBS_PROJ_ROW0, proj_row0); gate_obs_set(OBS_PROJ_ORTHO, proj_ortho);
}

// Projection matrix scan (diagnostic, DM_PROJ_SCAN=<seconds>[,<seconds>…]): at those times after start, the guest address space and the modules' images are searched for
// 4x4 float matrices shaped like a 16:9 perspective projection (|m11 / m00| within 1.70–1.86, last row or column (0, 0, ±1, 0)), each logged with the nearest pointer before it
// that names a module's read-only data (an object's vtable) within 0x400 bytes. For finding where a game keeps the projection of the view a screen-fill gate has to reach
// (Super Mario Bros. Wonder's courses)
#include <TargetConditionals.h>
#if !TARGET_OS_IPHONE   // the Mac host only: iOS has no mach_vm.h
#include <pthread.h>
#include <unistd.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
static int vt_of(const uint8_t *at, uint64_t lo, char *out, size_t cap) {   // the nearest vtable-like pointer at or below `at`, within 0x400 bytes
    for (const uint8_t *p = (const uint8_t *)((uintptr_t)at & ~7ull); p + 0x400 > at && (uint64_t)(uintptr_t)p >= lo; p -= 8) {
        uint64_t v; memcpy(&v, p, 8); module_t *m = module_of((void *)(uintptr_t)v);
        if (m && v - (uint64_t)(uintptr_t)m->base >= m->ro_off && v - (uint64_t)(uintptr_t)m->base < m->size) {
            snprintf(out, cap, "%s+%#llx at -%#lx", m->name, (unsigned long long)(v - (uint64_t)(uintptr_t)m->base), (long)(at - p)); return 1;
        }
    }
    snprintf(out, cap, "no vtable within 0x400"); return 0;
}
static int scan_range(uint64_t lo, uint64_t hi, int hits) {
    mach_vm_address_t a = lo;
    while (a < hi && hits < 60) {
        mach_vm_size_t sz = 0; vm_region_basic_info_data_64_t info; mach_msg_type_number_t n = VM_REGION_BASIC_INFO_COUNT_64; mach_port_t obj;
        if (mach_vm_region(mach_task_self(), &a, &sz, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &n, &obj) != KERN_SUCCESS || a >= hi) break;
        if (info.protection & VM_PROT_READ) {
            const float *f = (const float *)(uintptr_t)a, *end = (const float *)(uintptr_t)(a + sz) - 16;
            for (; f < end && hits < 60; f++) {
                static int pn = -1; static float pa, pb; static int pk;   // DM_PROJ_SCAN_PAIR=<a>,<b>,<k>: every place a float ≈ a has a float ≈ b k floats after it
                if (pn < 0) { const char *e = getenv("DM_PROJ_SCAN_PAIR"); pn = e && sscanf(e, "%f,%f,%d", &pa, &pb, &pk) == 3; }
                if (pn) {
                    if (fabsf(f[0] - pa) <= fabsf(pa) * 1e-4f && fabsf(f[pk] - pb) <= fabsf(pb) * 1e-4f) {
                        char vt[96]; vt_of((const uint8_t *)f, a, vt, sizeof vt);
                        LOG("[projscan] pair at %#llx; %s", (unsigned long long)(uintptr_t)f, vt);
                        for (int r = -4; r < 8; r++) LOG("[projscan]   %+4d  %10.5f %10.5f %10.5f %10.5f", r * 4, f[r * 4], f[r * 4 + 1], f[r * 4 + 2], f[r * 4 + 3]);
                        hits++;
                    }
                    continue;
                }
                static int any = -1; if (any < 0) any = getenv("DM_PROJ_SCAN_ANY") && atoi(getenv("DM_PROJ_SCAN_ANY"));   // =1: every perspective / orthographic shape at any ratio
                int rowm = f[12] == 0 && f[13] == 0 && fabsf(f[14]) == 1 && f[15] == 0, colm = f[3] == 0 && f[7] == 0 && fabsf(f[11]) == 1 && f[15] == 0;
                int ortho = any && f[12] == 0 && f[13] == 0 && f[14] == 0 && f[15] == 1 && f[10] != 0 && f[0] != 0 && f[5] != 0 && f[1] == 0 && f[4] == 0;
                if (ortho) rowm = 1;
                if (!rowm && !colm) continue;
                int fin = 1; for (int k = 0; k < 16 && fin; k++) fin = isfinite(f[k]);   // NaN passes every comparison below
                float m00 = fabsf(f[0]), m11 = fabsf(f[5]);
                if (!fin || !(any ? m00 > 0 && m11 > 0 && m11 / m00 > 0.2f && m11 / m00 < 5 : m00 >= 0.05f && m11 >= 0.05f && m11 / m00 >= 1.70f && m11 / m00 <= 1.86f)) continue;
                // a projection's shape: no x / y shear terms, and depth terms in the z row (row-major: m22, m23; column-major: m22, m32)
                if (!ortho && (fabsf(f[1]) > 1e-3f || fabsf(f[4]) > 1e-3f || f[10] == 0 || (rowm ? f[11] : f[14]) == 0 || fabsf(rowm ? f[2] : f[8]) > 1 || fabsf(rowm ? f[6] : f[9]) > 1)) continue;
                static float seenv[64][3]; static int seenc[64], nseen;   // one line per distinct matrix, with how many copies were found
                int j = 0; while (j < nseen && !(seenv[j][0] == f[0] && seenv[j][1] == f[5] && seenv[j][2] == f[10])) j++;
                if (j < nseen) { seenc[j]++; continue; }
                if (nseen < 64) { seenv[nseen][0] = f[0]; seenv[nseen][1] = f[5]; seenv[nseen][2] = f[10]; seenc[nseen++] = 1; }
                char vt[96]; vt_of((const uint8_t *)f, a, vt, sizeof vt);
                LOG("[projscan] %#llx %s m00 %.4f m11 %.4f ratio %.4f row2 %.4f %.4f %.4f %.4f; %s", (unsigned long long)(uintptr_t)f, ortho ? "orthographic" : rowm ? "row-major" : "column-major",
                    f[0], f[5], m11 / m00, f[8], f[9], f[10], f[11], vt);
                LOG("[projscan]   before %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f | after %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f", f[-8], f[-7], f[-6], f[-5], f[-4], f[-3], f[-2], f[-1],
                    f[16], f[17], f[18], f[19], f[20], f[21], f[22], f[23]);
                hits++; f += 15;
            }
        }
        a += sz;
    }
    return hits;
}
static void *proj_scan(void *arg) {
    const char *spec = arg; double prev = 0;
    for (const char *p = spec; p && *p; p = strchr(p, ',') ? strchr(p, ',') + 1 : NULL) {
        double t = atof(p); if (t > prev) usleep((useconds_t)((t - prev) * 1e6)); prev = t;
        LOG("[projscan] scan at %.0f s", t);
        int hits = scan_range(0x7000000000ull, 0x7100000000ull, 0);
        for (int i = 0; i < g_nmod; i++) hits = scan_range((uint64_t)(uintptr_t)g_mod[i].base, (uint64_t)(uintptr_t)g_mod[i].base + g_mod[i].size, hits);
        LOG("[projscan] %d distinct matrices", hits);
    }
    return NULL;
}
__attribute__((constructor)) static void proj_scan_start(void) {
    const char *e = getenv("DM_PROJ_SCAN"); if (!e || !*e) return;
    pthread_t t; pthread_create(&t, NULL, proj_scan, (void *)e); pthread_detach(t);
}
#endif
