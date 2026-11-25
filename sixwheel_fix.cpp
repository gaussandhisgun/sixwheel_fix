#if 0
set -e
SRC=$(basename "$0")
OUT=${SRC%.cpp}.asi
TOOL=""
MACH=""
for c in i686-w64-mingw32-g++ g++; do
  if command -v "$c" >/dev/null 2>&1; then
    m=$("$c" -dumpmachine 2>/dev/null || true)
    case "$m" in
      i686-*-mingw*|i686-*-windows*) TOOL="$c"; MACH="$m"; break ;;
    esac
  fi
done
if [ -z "$TOOL" ]; then
  echo "[sixwheel] 32-bit MinGW-w64 toolchain not found (i686-w64-mingw32-g++)."
  echo "[sixwheel] On MSYS2: pacman -S mingw-w64-i686-gcc, then from the MINGW32 shell:"
  echo "[sixwheel]   bash $SRC"
  exit 1
fi
echo "[sixwheel] building $OUT with $TOOL ($MACH)"
exec "$TOOL" -std=c++17 -O2 -Wall -shared -static-libgcc -static-libstdc++ -fno-exceptions -fno-rtti -o "$OUT" "$SRC"
#endif
#if 0
: <<'POLYGLOT_EOF'
#endif

// ============================================================================
// sixwheel.asi - wheel upgrades on the middle axle of 6-wheel vehicles
// GTA San Andreas 1.0 US/EU (HOODLUM), SA-MP / Arizona RP build, plain ASI.
// No plugin-sdk, no SAMPFUNCS, no MoonLoader dependencies.
//
// PROBLEM
//   A wheel upgrade bought in a mod shop (or applied by a server client-side)
//   only swaps the wheel atomics of the FOUR standard wheel nodes. The
//   middle-axle dummies (wheel_rm_dummy / wheel_lm_dummy on Linerunner,
//   Roadtrain, Tanker, Cement Truck, DFT-30 and any mod-added 6-wheeler)
//   keep the stock wheel.
//
// HOW IT WORKS (no hardcoded vehicle list):
//   SA applies wheel mods as per-node "replacement upgrades":
//     CVehicle::AddReplacementUpgrade(modelIndex, nodeId)      clones the
//       upgrade wheel atomic into the wheel node frame;
//     CVehicle::RemoveReplacementUpgrade(nodeId) / RemoveAllUpgrades take it
//       off again;
//     CVehicle::SetupUpgradesAfterLoad restores them after load/stream-in.
//   Vanilla only ever targets eCarNodes RF=2, LF=5, RB=4, LB=7 and never
//   touches the middle nodes RM=3 / LM=6 (m_aCarNodes @ CAutomobile+0x648).
//   This ASI redirects the E8 call sites of 5 vanilla functions to wrappers
//   (uafguard/profwatch call-site technique: no trampolines, no disassembler;
//   E9 tail-jmp sites are intentionally left alone - they push no return
//   address). Wrappers call the original by absolute address, then SYNC the
//   middle nodes with the standard wheels through the very same vanilla API:
//       RM <- RF (fallback RB),   LM <- LF (fallback LB)
//   A middle node is only touched when the vehicle really has one, so the
//   fix works on any 6-wheeler model, vanilla or mod-added, and stays
//   silent on ordinary 4-wheelers.
//
//   Safety rails per call: the vehicle's model info must be a
//   CVehicleModelInfo with m_nVehicleType (at +0x3C) AUTOMOBILE(0) or
//   MTRUCK(1); the modelinfo pointer table is resolved exactly like
//   plugin-sdk does (from the instruction immediates at 0x40CD67/0x40CD58),
//   which is why fastman92limitAdjuster-extended model counts are honored
//   and why NO vanilla model id range is assumed anywhere.
//
// BUILD (self-compiling polyglot, syncwatch scheme):
//   Linux : bash sixwheel.cpp
//   MSYS2 : bash sixwheel.cpp   (MINGW32 shell)
//   Manual: i686-w64-mingw32-g++ -std=c++17 -O2 -Wall -shared -static-libgcc
//           -static-libstdc++ -fno-exceptions -fno-rtti -o sixwheel.asi sixwheel.cpp
//           32-bit toolchain is mandatory (the game is x86).
// INSTALL: sixwheel.asi next to the game exe / other .asi modules.
// FILES (created in the game dir):
//   sixwheel.log - recreated every launch, flushed line-by-line
//   sixwheel.ini - created on first run
// ============================================================================

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>
#include <tlhelp32.h>

// ---------------------------------------------------------------------------
// Vanilla targets (SA 1.0 VA; used image-base-relative at runtime)
// ---------------------------------------------------------------------------
static const uint32_t VA_BASE_10 = 0x400000;

static const uint32_t VA_ADD_REP     = 0x6D3830; // CVehicle::AddReplacementUpgrade
static const uint32_t VA_REM_REP     = 0x6D39E0; // CVehicle::RemoveReplacementUpgrade
static const uint32_t VA_REM_ALL     = 0x6D3AB0; // CVehicle::RemoveAllUpgrades
static const uint32_t VA_SETUP_AFTER = 0x6E3400; // CVehicle::SetupUpgradesAfterLoad
static const uint32_t VA_CREATE_REP  = 0x6D3700; // CVehicle::CreateReplacementAtomic
static const uint32_t VA_GET_REP     = 0x6D3A50; // CVehicle::GetReplacementUpgrade (called, not hooked)

// CStreaming::RequestModelStream instruction immediates (limit-adjuster safe,
// plugin-sdk CModelInfo.cpp trick: ms_modelInfoPtrs = *(CBaseModelInfo***)
// 0x40CD67, ms_modelInfoCount = *(const int*)0x40CD58). The loader processes
// these as relocations, so runtime reads are ASLR- and fastman92-safe.
static const uint32_t VA_MI_PTRS_IMM = 0x40CD67;
static const uint32_t VA_MI_CNT_IMM  = 0x40CD58;

// eCarNodes (plugin-sdk, gta-reversed): wheel node indices
static const int CAR_WHEEL_RF = 2;   // front right
static const int CAR_WHEEL_RM = 3;   // MIDDLE right (wheel_rm_dummy)
static const int CAR_WHEEL_RB = 4;   // rear right
static const int CAR_WHEEL_LF = 5;   // front left
static const int CAR_WHEEL_LM = 6;   // MIDDLE left  (wheel_lm_dummy)
static const int CAR_WHEEL_LB = 7;   // rear left
static const int CAR_NUM_NODES = CAR_WHEEL_LB + 1 + 0; // only wheel nodes needed below

// Offsets (plugin-sdk VALIDATE_OFFSET):
static const uint32_t OFF_CAR_NODES = 0x648;    // CAutomobile: RwFrame* m_aCarNodes[]
static const uint32_t OFF_MODEL_ID  = 0x22;     // CEntity: u16 m_nModelIndex
static const uint32_t OFF_VMODEL_TYPE = 0x3C;   // CVehicleModelInfo: eVehicleType
static const int VTYPE_AUTOMOBILE = 0;
static const int VTYPE_MTRUCK     = 1;

// ---------------------------------------------------------------------------
// config + log
// ---------------------------------------------------------------------------
static char  g_cfgPath[MAX_PATH];
static char  g_logPath[MAX_PATH];
static FILE* g_log = NULL;
static bool  g_enabled = true;
static bool  g_verbose = false;
static int   g_rescanSec = 5;
static int   g_rescanMin = 10;
static int   g_maxSites = 4096;

static void logf(const char* fmt, ...)
{
    if (!g_log) return;
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap; va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

static int cfgInt(const char* sec, const char* key, int def)
{
    return GetPrivateProfileIntA(sec, key, def, g_cfgPath);
}

static void writeDefaultIni()
{
    if (GetFileAttributesA(g_cfgPath) != INVALID_FILE_ATTRIBUTES) return;
    FILE* f = fopen(g_cfgPath, "w");
    if (!f) return;
    fprintf(f, "[main]\n");
    fprintf(f, "; master switch, 0 = module loads but installs no hooks\n");
    fprintf(f, "enabled = 1\n");
    fprintf(f, "; verbose: log every module scan and section inspection\n");
    fprintf(f, "verbose = 0\n");
    fprintf(f, "\n[hooks]\n");
    fprintf(f, "; quiet rescan for modules that load late (samp.dll etc), 0 minutes = off\n");
    fprintf(f, "rescan_sec = 5\n");
    fprintf(f, "rescan_minutes = 10\n");
    fprintf(f, "\n[limits]\n");
    fprintf(f, "; all-time cap of redirected call sites (not a vehicle list)\n");
    fprintf(f, "max_sites = 4096\n");
    fclose(f);
}

static void initPaths()
{
    char exe[MAX_PATH]; exe[0] = 0;
    if (!GetModuleFileNameA(NULL, exe, MAX_PATH)) exe[0] = 0;
    char* slash = strrchr(exe, '\\');
    if (slash) *slash = 0;
    if (exe[0]) {
        _snprintf(g_cfgPath, MAX_PATH - 1, "%s\\sixwheel.ini", exe);
        _snprintf(g_logPath, MAX_PATH - 1, "%s\\sixwheel.log", exe);
    } else {
        strcpy(g_cfgPath, "sixwheel.ini");
        strcpy(g_logPath, "sixwheel.log");
    }
    g_cfgPath[MAX_PATH - 1] = 0;
    g_logPath[MAX_PATH - 1] = 0;
}

// ---------------------------------------------------------------------------
// vanilla prototypes (thiscall emulated as fastcall with dummy edx;
// MinGW 32-bit: 1st arg -> ecx, 2nd -> edx, rest on the stack, callee-clean)
// ---------------------------------------------------------------------------
typedef void* (__fastcall *fn_Atomic_Rep)(void* self, void* edx, void* model, void* frame, int flags, int bDamaged, int bIsWheel);
typedef void  (__fastcall *fn_Void_Int2)(void* self, void* edx, int modelIndex, int nodeId);
typedef void  (__fastcall *fn_Void_Int1)(void* self, void* edx, int nodeId);
typedef void  (__fastcall *fn_Void_Void)(void* self, void* edx);
typedef int   (__fastcall *fn_Int_Int1)(void* self, void* edx, int nodeId);

static fn_Atomic_Rep ORIG_CreateRep  = NULL;
static fn_Void_Int2  ORIG_AddRep     = NULL;
static fn_Void_Int1  ORIG_RemRep     = NULL;
static fn_Void_Void  ORIG_RemAll     = NULL;
static fn_Void_Void  ORIG_SetupAfter = NULL;
static fn_Int_Int1   FN_GetRep       = NULL; // called directly by us

static uintptr_t g_base = 0;
static HMODULE   g_hSelf = NULL;
static void**    g_miPtrs = NULL;   // CModelInfo::ms_modelInfoPtrs (runtime)
static int       g_miCount = 0;     // CModelInfo::ms_modelInfoCount  (runtime)
static int       g_depth = 0;       // reentrancy guard for the sync machinery
static int       g_sitesPatched = 0;
static int       g_syncCount = 0;

// ---------------------------------------------------------------------------
// guarded helpers
// ---------------------------------------------------------------------------
static inline void* vadr(uint32_t va)
{
    return (void*)(g_base + (uintptr_t)(va - VA_BASE_10));
}

static bool readable(const void* p, size_t n)
{
    return p && IsBadReadPtr(p, n) == 0;
}

// A middle node is only touched when the vehicle really has one,
// hence no hardcoded 6-wheeler list anywhere.
static bool isSyncTarget(void* veh)
{
    if (!veh || !g_miPtrs || !g_miCount) return false;
    if (IsBadReadPtr((const char*)veh + OFF_MODEL_ID, 2)) return false;
    const uint16_t modelIdx = *(const uint16_t*)((const char*)veh + OFF_MODEL_ID);
    if (modelIdx >= g_miCount) return false;                 // runtime count, no vanilla cap
    void* mi = g_miPtrs[modelIdx];
    if (!mi || !readable((const char*)mi + OFF_VMODEL_TYPE, 4)) return false;
    const int vtype = *(const int*)((const char*)mi + OFF_VMODEL_TYPE);
    if (vtype != VTYPE_AUTOMOBILE && vtype != VTYPE_MTRUCK) return false;
    if (!readable((const char*)veh + OFF_CAR_NODES, 4 * (CAR_NUM_NODES))) return false;
    return true;                                             // mid frames may still be NULL
}

static void syncSide(void* veh, int srcNode, int dstNode)
{
    void** nodes = (void**)((uintptr_t)veh + OFF_CAR_NODES);
    if (!nodes[dstNode]) return;                 // model has no middle dummy
    if (!nodes[srcNode]) return;                 // source axle does not exist
    if (!readable(nodes[dstNode], 4)) return;    // unmapped/gone, skip loudly-later
    if (!readable(nodes[srcNode], 4)) return;

    const int want = FN_GetRep(veh, (void*)0, srcNode);
    const int have = FN_GetRep(veh, (void*)0, dstNode);
    if (want == have) return;                    // already in sync (-1 == -1 too)

    if (have != -1) {
        logf("[WHEEL] veh=0x%08X node=%d remove old mid model=%d", (unsigned)(uintptr_t)veh, dstNode, have);
        ORIG_RemRep(veh, (void*)0, dstNode);
    }
    if (want != -1) {
        logf("[WHEEL] veh=0x%08X model=%d -> node=%d (mirrors node=%d)", (unsigned)(uintptr_t)veh, want, dstNode, srcNode);
        ORIG_AddRep(veh, (void*)0, want, dstNode);
    }
    g_syncCount++;
    logf("[WHEEL] veh=0x%08X sync done (fix #%d)", (unsigned)(uintptr_t)veh, g_syncCount);
}

static void syncMiddleWheels(void* veh)
{
    if (!g_enabled) return;
    if (g_depth > 0) return;                  // our own mirrored calls, stop recursion
    if (!isSyncTarget(veh)) return;
    g_depth++;
    void** nodes = (void**)((uintptr_t)veh + OFF_CAR_NODES);
    if (nodes[CAR_WHEEL_RM]) {
        if      (nodes[CAR_WHEEL_RF] && readable(nodes[CAR_WHEEL_RF], 4)) syncSide(veh, CAR_WHEEL_RF, CAR_WHEEL_RM);
        else if (nodes[CAR_WHEEL_RB] && readable(nodes[CAR_WHEEL_RB], 4)) syncSide(veh, CAR_WHEEL_RB, CAR_WHEEL_RM);
    }
    if (nodes[CAR_WHEEL_LM]) {
        if      (nodes[CAR_WHEEL_LF] && readable(nodes[CAR_WHEEL_LF], 4)) syncSide(veh, CAR_WHEEL_LF, CAR_WHEEL_LM);
        else if (nodes[CAR_WHEEL_LB] && readable(nodes[CAR_WHEEL_LB], 4)) syncSide(veh, CAR_WHEEL_LB, CAR_WHEEL_LM);
    }
    g_depth--;
}

// ---------------------------------------------------------------------------
// call-site redirection (uafguard technique: E8 rel32 only; wrappers call
// the original by absolute address, then keep the middle nodes in sync)
// ---------------------------------------------------------------------------
static void patchSitePtr(uint8_t* site, void* newTarget)
{
    DWORD oldp = 0;
    if (!VirtualProtect(site + 1, 4, PAGE_EXECUTE_READWRITE, &oldp)) {
        logf("[ERR] VirtualProtect failed at 0x%08X (%lu)", (unsigned)(uintptr_t)site, GetLastError());
        return;
    }
    const int32_t rel = (int32_t)((uintptr_t)newTarget - ((uintptr_t)site + 5));
    // the E8 opcode byte is byte-identical in both old and new states, so a
    // racing thread sees either the old or the new target, never garbage
    *(volatile uint32_t*)(site + 1) = (uint32_t)rel;
    DWORD tmp = 0;
    VirtualProtect(site + 1, 4, oldp, &tmp);
}

struct HookDesc {
    const char* name;
    uint32_t    va;       // vanilla target VA (1.0)
    void*       wrapper;
};

static HookDesc g_hooks[5];

static int scanModule(const char* modName, HMODULE hmod, void* selfBase)
{
    if ((HMODULE)selfBase == hmod) return 0;  // never patch inside ourselves
    const uint8_t* image = (const uint8_t*)hmod;
    if (!readable(image, 0x40)) {
        if (g_verbose) logf("[SCAN] %s: header unreadable, skipped", modName);
        return 0;
    }
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)image;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        logf("[ERR] %s: bad DOS signature, skipped", modName);
        return 0;
    }
    if (IsBadReadPtr(image + dos->e_lfanew, sizeof(IMAGE_NT_HEADERS))) {
        logf("[ERR] %s: NT headers unreadable, skipped", modName);
        return 0;
    }
    const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(image + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        logf("[ERR] %s: bad NT signature, skipped", modName);
        return 0;
    }
    if (nt->FileHeader.NumberOfSections > 64) {
        logf("[ERR] %s: insane section count %u, skipped", modName, nt->FileHeader.NumberOfSections);
        return 0;
    }
    const IMAGE_OPTIONAL_HEADER32* opt = &nt->OptionalHeader;
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    int patched = 0;
    for (int s = 0; s < (int)nt->FileHeader.NumberOfSections; s++, sec++) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        const uint32_t rva = sec->VirtualAddress;
        const uint32_t vsize = sec->Misc.VirtualSize;
        if (vsize < 6) continue;
        if (rva >= opt->SizeOfImage || vsize > opt->SizeOfImage - rva) {
            logf("[ERR] %s: section %d spans outside image (rva=0x%X size=0x%X), skipped",
                 modName, s, rva, vsize);
            continue;
        }
        const uint8_t* p = image + rva;
        if (IsBadReadPtr(p, vsize)) {
            logf("[ERR] %s: section %d unreadable, skipped", modName, s);
            continue;
        }
        for (uint32_t i = 0; i + 5 <= vsize; i++) {
            if (p[i] != 0xE8 && p[i] != 0xE9) continue;
            const uint8_t* site = p + i;
            const int32_t rel = *(const int32_t*)(site + 1);
            const uintptr_t target = (uintptr_t)site + 5 + (uintptr_t)(int64_t)rel;
            for (int h = 0; h < 5; h++) {
                const uintptr_t want = g_base + (uintptr_t)(g_hooks[h].va - VA_BASE_10);
                if (target != want) continue;
                const uintptr_t wrap = (uintptr_t)g_hooks[h].wrapper;
                const int32_t curRel = (int32_t)(wrap - ((uintptr_t)site + 5));
                if (*(const int32_t*)(site + 1) == curRel) break; // already ours
                if (g_sitesPatched >= g_maxSites) {
                    logf("[ERR] site cap %d reached, not patching more", g_maxSites);
                    return patched;
                }
                if (p[i] == 0xE9) {
                    // profwatch lesson: a tail call pushes no return address,
                    // a post-original wrapper cannot run, so E9 stays vanilla
                    if (g_verbose) logf("[SCAN] %s: E9 tail-jmp to %s at 0x%08X not patched by design",
                                        modName, g_hooks[h].name, (unsigned)(uintptr_t)site);
                    continue;
                }
                patchSitePtr((uint8_t*)site, (void*)wrap);
                patched++;
                g_sitesPatched++;
                logf("[HOOK] %s: %s site 0x%08X redirected", modName, g_hooks[h].name, (unsigned)(uintptr_t)site);
            }
        }
    }
    return patched;
}

struct ModList {
    char    names[128][MAX_PATH];
    HMODULE mods[128];
    int     n;
};

static void collectModules(ModList* ml)
{
    ml->n = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) {
        logf("[ERR] CreateToolhelp32Snapshot failed (%lu)", GetLastError());
        return;
    }
    MODULEENTRY32 me; memset(&me, 0, sizeof(me)); me.dwSize = sizeof(me);
    if (Module32First(snap, &me)) {
        do {
            if (ml->n >= 128) { logf("[ERR] module list cap 128 reached"); break; }
            strncpy(ml->names[ml->n], me.szModule, MAX_PATH - 1);
            ml->names[ml->n][MAX_PATH - 1] = 0;
            ml->mods[ml->n] = (HMODULE)me.modBaseAddr; // field name fix from profwatch v1.1
            ml->n++;
        } while (Module32Next(snap, &me));
    } else {
        logf("[ERR] Module32First failed (%lu)", GetLastError());
    }
    CloseHandle(snap);
}

static void scanAllModules()
{
    ModList ml; collectModules(&ml);
    for (int i = 0; i < ml.n; i++) {
        int n = scanModule(ml.names[i], ml.mods[i], g_hSelf);
        if (g_verbose && n == 0) logf("[SCAN] %s: 0 sites", ml.names[i]);
    }
}

// ---------------------------------------------------------------------------
// wrappers (thiscall ABI == fastcall with dummy edx)
// ---------------------------------------------------------------------------
static void __fastcall w_AddRep(void* self, void* edx, int modelIndex, int nodeId)
{
    ORIG_AddRep(self, edx, modelIndex, nodeId);
    syncMiddleWheels(self);
}

static void __fastcall w_RemRep(void* self, void* edx, int nodeId)
{
    ORIG_RemRep(self, edx, nodeId);
    syncMiddleWheels(self);
}

static void __fastcall w_RemAll(void* self, void* edx)
{
    ORIG_RemAll(self, edx);
    syncMiddleWheels(self);
}

static void __fastcall w_SetupAfter(void* self, void* edx)
{
    ORIG_SetupAfter(self, edx);
    syncMiddleWheels(self);
}

static void* __fastcall w_CreateRep(void* self, void* edx, void* model, void* frame, int flags, int bDamaged, int bIsWheel)
{
    void* r = ORIG_CreateRep(self, edx, model, frame, flags, bDamaged, bIsWheel);
    syncMiddleWheels(self);
    return r;
}

// ---------------------------------------------------------------------------
// modelinfo table resolution (limit-adjuster safe, plugin-sdk pattern)
// ---------------------------------------------------------------------------
static void resolveModelInfoTable()
{
    // the bytes AT 0x40CD67 are the 4-byte immediate of the instruction that
    // starts right there - exactly what plugin-sdk dereferences; the loader
    // relocation makes the read back ASLR/limit-adjuster correct
    void** ptrs = NULL;
    int cnt = 0;
    if (readable(vadr(VA_MI_PTRS_IMM), 4)) ptrs = *(void***)(void*)vadr(VA_MI_PTRS_IMM);
    if (readable(vadr(VA_MI_CNT_IMM), 4))  cnt  = *(const int*)vadr(VA_MI_CNT_IMM);
    g_miPtrs = ptrs;
    g_miCount = cnt;
    logf("[INIT] modelinfo array=0x%08X count=%d (resolved from 0x40CD67/0x40CD58)", (unsigned)(uintptr_t)ptrs, cnt);
    if (!ptrs || cnt <= 0) {
        logf("[ERR] modelinfo table resolution failed, sync gates will refuse all vehicles");
    }
}

// ---------------------------------------------------------------------------
// worker: quiet rescans for late-loaded modules (syncwatch policy)
// ---------------------------------------------------------------------------
static DWORD WINAPI worker(LPVOID)
{
    const ULONGLONG start  = GetTickCount64();
    const ULONGLONG period = (ULONGLONG)g_rescanSec * 1000ULL;
    const ULONGLONG total  = (ULONGLONG)g_rescanMin * 60000ULL;
    if (!period || !total) { logf("[SCAN-T] rescans disabled by config"); return 0; }
    while (GetTickCount64() - start < total) {
        Sleep((DWORD)period);
        if (!g_enabled) continue;
        scanAllModules();
    }
    logf("[SCAN-T] rescan window (%d min) is over, quiet", g_rescanMin);
    return 0;
}

// ---------------------------------------------------------------------------
static void parseConfig()
{
    g_enabled   = cfgInt("main", "enabled", 1) != 0;
    g_verbose   = cfgInt("main", "verbose", 0) != 0;
    g_rescanSec = cfgInt("hooks", "rescan_sec", 5);
    g_rescanMin = cfgInt("hooks", "rescan_minutes", 10);
    g_maxSites  = cfgInt("limits", "max_sites", 4096);
    if (g_rescanSec < 0) g_rescanSec = 0;
}

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID)
{
    if (reason != DLL_PROCESS_ATTACH) return TRUE;

    g_hSelf = hInst;
    initPaths();

    // log is recreated at every launch (user request, same as syncwatch)
    g_log = fopen(g_logPath, "w");
    logf("[INIT] sixwheel.asi starting");

    parseConfig();
    writeDefaultIni();

    g_base = (uintptr_t)GetModuleHandleA(NULL);
    logf("[INIT] main image base=0x%08X, enabled=%d", (unsigned)g_base, g_enabled ? 1 : 0);

    if (!g_base || g_base == (uintptr_t)hInst) {
        logf("[ERR] main image base looks wrong, aborting hook setup");
        return TRUE;
    }

    if (!g_enabled) {
        logf("[INIT] disabled by sixwheel.ini, exiting without hooks");
        return TRUE;
    }

    ORIG_CreateRep  = (fn_Atomic_Rep)vadr(VA_CREATE_REP);
    ORIG_AddRep     = (fn_Void_Int2)vadr(VA_ADD_REP);
    ORIG_RemRep     = (fn_Void_Int1)vadr(VA_REM_REP);
    ORIG_RemAll     = (fn_Void_Void)vadr(VA_REM_ALL);
    ORIG_SetupAfter = (fn_Void_Void)vadr(VA_SETUP_AFTER);
    FN_GetRep       = (fn_Int_Int1)vadr(VA_GET_REP);

    g_hooks[0].name = "AddReplacementUpgrade";    g_hooks[0].va = VA_ADD_REP;     g_hooks[0].wrapper = (void*)&w_AddRep;
    g_hooks[1].name = "RemoveReplacementUpgrade"; g_hooks[1].va = VA_REM_REP;     g_hooks[1].wrapper = (void*)&w_RemRep;
    g_hooks[2].name = "RemoveAllUpgrades";        g_hooks[2].va = VA_REM_ALL;     g_hooks[2].wrapper = (void*)&w_RemAll;
    g_hooks[3].name = "SetupUpgradesAfterLoad";   g_hooks[3].va = VA_SETUP_AFTER; g_hooks[3].wrapper = (void*)&w_SetupAfter;
    g_hooks[4].name = "CreateReplacementAtomic";  g_hooks[4].va = VA_CREATE_REP;  g_hooks[4].wrapper = (void*)&w_CreateRep;

    for (int h = 0; h < 5; h++) {
        if (!readable(vadr(g_hooks[h].va), 4)) {
            logf("[ERR] target %s at VA 0x%X unreadable - wrong exe version?", g_hooks[h].name, g_hooks[h].va);
        }
    }

    // any unresolved original = wrong exe; wrappers would fire into NULL
    if (!ORIG_CreateRep || !ORIG_AddRep || !ORIG_RemRep || !ORIG_RemAll || !ORIG_SetupAfter || !FN_GetRep) {
        logf("[ERR] at least one vanilla function unresolved, NO hooks installed (nothing will be mirrored)");
        return TRUE;
    }

    resolveModelInfoTable();
    scanAllModules();

    if (g_sitesPatched == 0) {
        logf("[ERR] not a single call site found for the 5 targets - exe mismatch or indirect calls only; NOTHING will be mirrored");
    } else {
        logf("[INIT] %d call site(s) redirected", g_sitesPatched);
    }

    HANDLE th = CreateThread(NULL, 0, worker, NULL, 0, NULL);
    if (th) CloseHandle(th);
    else logf("[ERR] CreateThread failed (%lu), late-loaded modules will not be scanned", GetLastError());

    return TRUE;
}

#if 0
POLYGLOT_EOF
#endif
