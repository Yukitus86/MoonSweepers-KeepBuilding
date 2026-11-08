/*
 * KeepBuilding - Moon Sweepers mod
 *
 * Keeps the selected building on the hammer after placing it, so it can be placed
 * again and again without reopening the build menu.
 *
 * Loaded as a dwmapi.dll proxy (every export jumps to C:\Windows\System32\dwmapi.dll).
 * When GameAssembly.dll is loaded it hooks il2cpp_init there, then talks to the
 * IL2CPP runtime through GameAssembly.dll's public il2cpp_* exports. No imports are
 * patched, so it runs next to BepInEx (Doorstop) and other proxy mods.
 *
 * In the game, MoonDepotBuilder.MachineBuilt runs after every successful build and ends
 * the placement (CancelPlacement) or jumps to the conveyor tool (BeginConveyor).
 * Foundation modules skip it and take a "keep placing" branch inside ShowPlan instead.
 * The hook below gives all other buildings that same branch.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <share.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>

#include "dwmapi_exports.h"

#define MOD_VERSION "1.0.1"

/* ------------------------------------------------------------------------ */
/* dwmapi forwarding                                                          */
/* ------------------------------------------------------------------------ */

void *g_real[REAL_EXPORT_COUNT]; /* read by dwmapi_thunks.asm */

static void load_real_dll(void)
{
    wchar_t path[MAX_PATH];
    UINT len = GetSystemDirectoryW(path, MAX_PATH);
    if (len == 0 || len > MAX_PATH - 12) return;
    wcscat_s(path, MAX_PATH, L"\\dwmapi.dll");
    HMODULE m = LoadLibraryW(path);
    if (!m) return;
    for (int i = 0; i < REAL_EXPORT_COUNT; i++) {
        const char *n = g_real_exports[i].name;
        g_real[i] = (void *)GetProcAddress(m, n ? n : MAKEINTRESOURCEA(g_real_exports[i].ordinal));
    }
}

/* ------------------------------------------------------------------------ */
/* config + log                                                               */
/* ------------------------------------------------------------------------ */

static wchar_t g_dir[MAX_PATH];
static wchar_t g_ini[MAX_PATH];
static FILE *g_log;
static CRITICAL_SECTION g_log_cs;

static void logf_(const char *fmt, ...)
{
    if (!g_log) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    EnterCriticalSection(&g_log_cs);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
    LeaveCriticalSection(&g_log_cs);
}

static int ini_int(const wchar_t *key, int def)
{
    return (int)GetPrivateProfileIntW(L"KeepBuilding", key, def, g_ini);
}

/* ------------------------------------------------------------------------ */
/* il2cpp API                                                                 */
/* ------------------------------------------------------------------------ */

typedef void Il2CppDomain, Il2CppAssembly, Il2CppImage, Il2CppClass, FieldInfo, MethodInfo, Il2CppObject;

#define IL2CPP_API(X)                                                                       \
    X(Il2CppDomain *, il2cpp_domain_get, (void))                                            \
    X(const Il2CppAssembly **, il2cpp_domain_get_assemblies, (Il2CppDomain *, size_t *))    \
    X(const Il2CppImage *, il2cpp_assembly_get_image, (const Il2CppAssembly *))             \
    X(const char *, il2cpp_image_get_name, (const Il2CppImage *))                           \
    X(Il2CppClass *, il2cpp_class_from_name, (const Il2CppImage *, const char *, const char *)) \
    X(const char *, il2cpp_class_get_name, (Il2CppClass *))                                 \
    X(const MethodInfo *, il2cpp_class_get_method_from_name, (Il2CppClass *, const char *, int)) \
    X(FieldInfo *, il2cpp_class_get_field_from_name, (Il2CppClass *, const char *))         \
    X(size_t, il2cpp_field_get_offset, (FieldInfo *))

#define DECL(ret, name, args) static ret(*p_##name) args;
IL2CPP_API(DECL)
#undef DECL

static int resolve_il2cpp(void)
{
    HMODULE ga = GetModuleHandleW(L"GameAssembly.dll");
    if (!ga) return 0;
    int ok = 1;
#define RES(ret, name, args)                                       \
    p_##name = (ret(*) args)(void *)GetProcAddress(ga, #name);     \
    if (!p_##name) { logf_("missing export %s", #name); ok = 0; }
    IL2CPP_API(RES)
#undef RES
    return ok;
}

static const Il2CppImage *find_image(const char *name)
{
    size_t na = 0;
    const Il2CppAssembly **as = p_il2cpp_domain_get_assemblies(p_il2cpp_domain_get(), &na);
    for (size_t a = 0; a < na; a++) {
        const Il2CppImage *i = p_il2cpp_assembly_get_image(as[a]);
        if (_stricmp(p_il2cpp_image_get_name(i), name) == 0) return i;
    }
    return NULL;
}

static int field_off(Il2CppClass *k, const char *name, size_t *out)
{
    FieldInfo *f = p_il2cpp_class_get_field_from_name(k, name);
    if (!f) { logf_("Field %s.%s not found.", p_il2cpp_class_get_name(k), name); return 0; }
    *out = p_il2cpp_field_get_offset(f);
    return 1;
}

static void *method_ptr(Il2CppClass *k, const char *name, int args)
{
    const MethodInfo *m = p_il2cpp_class_get_method_from_name(k, name, args);
    void *p = m ? *(void **)m : NULL; /* MethodInfo starts with methodPointer */
    if (!p) logf_("Method %s.%s not found.", p_il2cpp_class_get_name(k), name);
    return p;
}

/* ------------------------------------------------------------------------ */
/* inline hook                                                                */
/*                                                                            */
/* MachineBuilt is called directly from game code, so unlike a MonoBehaviour  */
/* message it can't be hooked through its MethodInfo. The target starts with  */
/* a 5-byte jump to a stub within +-2 GB, which holds the absolute jump to    */
/* the hook and a trampoline with the overwritten instructions.               */
/* ------------------------------------------------------------------------ */

/* Length of one x64 prologue instruction, 0 if unknown. Only recognises
   instructions without RIP-relative operands, so they can be moved as they are. */
static size_t insn_len(const uint8_t *p)
{
    if (p[0] >= 0x50 && p[0] <= 0x57) return 1;                 /* push reg */
    if ((p[0] == 0x40 || p[0] == 0x41) && p[1] >= 0x50 && p[1] <= 0x57) return 2; /* push r8-r15 */
    if (p[0] == 0x48 || p[0] == 0x4C) {
        if (p[1] == 0x89 && (p[2] & 0xC7) == 0x44 && p[3] == 0x24) return 5; /* mov [rsp+d8],reg */
        if (p[0] == 0x48 && p[1] == 0x8B && p[2] == 0xC4) return 3;          /* mov rax,rsp */
        if (p[0] == 0x48 && p[1] == 0x83 && p[2] == 0xEC) return 4;          /* sub rsp,imm8 */
        if (p[0] == 0x48 && p[1] == 0x81 && p[2] == 0xEC) return 7;          /* sub rsp,imm32 */
    }
    return 0;
}

static uint8_t *alloc_near(uint8_t *target)
{
    const uintptr_t step = 0x10000, reach = 0x7FF00000;
    uintptr_t origin = (uintptr_t)target & ~(step - 1);
    for (uintptr_t d = step; d < reach; d += step) {
        for (int up = 0; up < 2; up++) {
            uintptr_t a = up ? origin + d : origin - d;
            MEMORY_BASIC_INFORMATION mi;
            if (!VirtualQuery((void *)a, &mi, sizeof mi) || mi.State != MEM_FREE) continue;
            void *p = VirtualAlloc((void *)a, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
            if (p) return (uint8_t *)p;
        }
    }
    return NULL;
}

static uint8_t *write_abs_jmp(uint8_t *at, const void *to)
{
    static const uint8_t jmp[6] = {0xFF, 0x25, 0, 0, 0, 0}; /* jmp qword ptr [rip+0] */
    memcpy(at, jmp, sizeof jmp);
    memcpy(at + sizeof jmp, &to, sizeof to);
    return at + sizeof jmp + sizeof to;
}

static void *install_hook(void *target, void *hook)
{
    uint8_t *t = (uint8_t *)target;
    size_t n = 0;
    while (n < 5) {
        size_t l = insn_len(t + n);
        if (!l) {
            logf_("Unknown code at %p: %02X %02X %02X %02X %02X (game updated?)", t, t[n], t[n + 1], t[n + 2],
                  t[n + 3], t[n + 4]);
            return NULL;
        }
        n += l;
    }

    uint8_t *stub = alloc_near(t);
    if (!stub) { logf_("No free memory near the game code."); return NULL; }
    uint8_t *tramp = write_abs_jmp(stub, hook);
    memcpy(tramp, t, n);
    write_abs_jmp(tramp + n, t + n);
    DWORD old;
    VirtualProtect(stub, 4096, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), stub, 4096);

    if (!VirtualProtect(t, n, PAGE_EXECUTE_READWRITE, &old)) { logf_("Could not unprotect game code."); return NULL; }
    int32_t rel = (int32_t)(stub - (t + 5));
    t[0] = 0xE9;
    memcpy(t + 1, &rel, sizeof rel);
    memset(t + 5, 0x90, n - 5);
    VirtualProtect(t, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), t, n);
    return tramp;
}

/* ------------------------------------------------------------------------ */
/* keep placing                                                               */
/* ------------------------------------------------------------------------ */

#define AT(obj, off, T) ((T *)((char *)(obj) + (off)))

typedef void (*MachineBuilt_t)(Il2CppObject *, Il2CppObject *, const MethodInfo *);
typedef void (*EndMachineSnap_t)(Il2CppObject *, const MethodInfo *);
typedef int (*FrameCount_t)(const MethodInfo *);

static MachineBuilt_t o_machine_built;
static EndMachineSnap_t p_end_machine_snap;
static FrameCount_t p_frame_count;

static size_t o_placing, o_valid, o_looks_valid, o_next_check, o_check, o_plan, o_last_valid, o_shown,
    o_suppressed, o_ports;

static void h_machine_built(Il2CppObject *self, Il2CppObject *def, const MethodInfo *mi)
{
    static int logged;
    /* Read here so the .ini can be changed while the game is running. */
    int enabled = ini_int(L"Enabled", 1) != 0;
    int to_conveyor = ini_int(L"ConveyorAfterMachine", 0) != 0;

    Il2CppObject *ports = def ? *AT(def, o_ports, Il2CppObject *) : NULL;
    int has_ports = ports && *AT(ports, 0x18, size_t) != 0; /* Il2CppArray::max_length */

    if (!enabled || !def || !*AT(self, o_placing, uint8_t) || (has_ports && to_conveyor)) {
        o_machine_built(self, def, mi);
        return;
    }

    /* Clears the belt preview and machine snap, as CancelPlacement would. */
    p_end_machine_snap(self, NULL);

    /* The checks were made before this building existed. Dropping all of them (not
       only the current one, lastValid can also be handed to TryBuild*) makes the game
       check the spot again, now with the new building on it. */
    *AT(self, o_check, void *) = NULL;
    *AT(self, o_plan, void *) = NULL;
    *AT(self, o_last_valid, void *) = NULL;
    *AT(self, o_shown, void *) = NULL;
    *AT(self, o_valid, uint8_t) = 0;
    *AT(self, o_looks_valid, uint8_t) = 0;
    *AT(self, o_next_check, float) = 0.0f;
    /* Same as the game's own branch: ignore the rest of this click. */
    *AT(self, o_suppressed, int) = p_frame_count(NULL) + 2;

    if (logged < 5) {
        logged++;
        logf_("Building placed, selection kept%s.", has_ports ? " (machine with conveyor ports)" : "");
    }
}

static void setup_hook(void)
{
    const Il2CppImage *game = find_image("Assembly-CSharp.dll");
    const Il2CppImage *core = find_image("UnityEngine.CoreModule.dll");
    if (!game || !core) { logf_("Game assemblies not found - mod inactive."); return; }

    Il2CppClass *k_builder = p_il2cpp_class_from_name(game, "MoonCleaners", "MoonDepotBuilder");
    Il2CppClass *k_def = p_il2cpp_class_from_name(game, "MoonCleaners", "MoonDepotBuildDefinition");
    Il2CppClass *k_time = p_il2cpp_class_from_name(core, "UnityEngine", "Time");
    if (!k_builder || !k_def || !k_time) {
        logf_("Game classes not found (game updated?) - mod inactive.");
        return;
    }

    int ok = 1;
    ok &= field_off(k_builder, "placing", &o_placing);
    ok &= field_off(k_builder, "valid", &o_valid);
    ok &= field_off(k_builder, "looksValid", &o_looks_valid);
    ok &= field_off(k_builder, "nextCheck", &o_next_check);
    ok &= field_off(k_builder, "placementCheck", &o_check);
    ok &= field_off(k_builder, "structurePlan", &o_plan);
    ok &= field_off(k_builder, "lastValid", &o_last_valid);
    ok &= field_off(k_builder, "shown", &o_shown);
    ok &= field_off(k_builder, "suppressedUntil", &o_suppressed);
    ok &= field_off(k_def, "conveyorPorts", &o_ports);
    void *built = method_ptr(k_builder, "MachineBuilt", 1);
    p_end_machine_snap = (EndMachineSnap_t)method_ptr(k_builder, "EndMachineSnap", 0);
    p_frame_count = (FrameCount_t)method_ptr(k_time, "get_frameCount", 0);
    if (!ok || !built || !p_end_machine_snap || !p_frame_count) {
        logf_("Game code not found (game updated?) - mod inactive.");
        return;
    }

    o_machine_built = (MachineBuilt_t)install_hook(built, (void *)h_machine_built);
    if (!o_machine_built) { logf_("Hook failed - mod inactive."); return; }
    logf_("Hook installed (MachineBuilt at %p).", built);
}

/* ------------------------------------------------------------------------ */
/* startup: catch il2cpp_init                                                 */
/* ------------------------------------------------------------------------ */

typedef int (*il2cpp_init_t)(const char *);
static il2cpp_init_t o_il2cpp_init;

static int h_il2cpp_init(const char *domain)
{
    int r = o_il2cpp_init(domain);
    if (resolve_il2cpp()) setup_hook();
    else logf_("IL2CPP API incomplete - mod inactive.");
    return r;
}

/* Finds an export by name without GetProcAddress (safe inside the loader callback). */
static void *find_export(BYTE *b, const char *func)
{
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(b + ((IMAGE_DOS_HEADER *)b)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dir.VirtualAddress) return NULL;
    IMAGE_EXPORT_DIRECTORY *ed = (IMAGE_EXPORT_DIRECTORY *)(b + dir.VirtualAddress);
    DWORD *names = (DWORD *)(b + ed->AddressOfNames);
    WORD *ords = (WORD *)(b + ed->AddressOfNameOrdinals);
    DWORD *funcs = (DWORD *)(b + ed->AddressOfFunctions);
    for (DWORD i = 0; i < ed->NumberOfNames; i++) {
        if (strcmp((char *)(b + names[i]), func) != 0) continue;
        DWORD rva = funcs[ords[i]];
        if (rva >= dir.VirtualAddress && rva < dir.VirtualAddress + dir.Size) return NULL; /* forwarder */
        return b + rva;
    }
    return NULL;
}

static volatile LONG g_init_hooked;

/* Called once GameAssembly.dll is mapped, before any of its code runs. */
static void hook_il2cpp_init(BYTE *game_assembly)
{
    if (InterlockedExchange(&g_init_hooked, 1)) return;
    void *init = find_export(game_assembly, "il2cpp_init");
    if (!init) { logf_("il2cpp_init not found in GameAssembly.dll - mod inactive."); return; }
    o_il2cpp_init = (il2cpp_init_t)install_hook(init, (void *)h_il2cpp_init);
    if (o_il2cpp_init) logf_("Hooked il2cpp_init in GameAssembly.dll.");
    else logf_("Could not hook il2cpp_init - mod inactive.");
}

/* Loader notifications (documented ntdll API, also implemented by Wine/Proton). */
typedef struct {
    USHORT Length, MaximumLength;
    PWSTR Buffer;
} LdrUnicodeString;
typedef struct {
    ULONG Flags;
    const LdrUnicodeString *FullDllName;
    const LdrUnicodeString *BaseDllName;
    PVOID DllBase;
    ULONG SizeOfImage;
} LdrDllLoadedData;
#define LDR_DLL_NOTIFICATION_REASON_LOADED 1
typedef VOID(CALLBACK *LdrDllNotification_t)(ULONG, const LdrDllLoadedData *, PVOID);
typedef LONG(NTAPI *LdrRegisterDllNotification_t)(ULONG, LdrDllNotification_t, PVOID, PVOID *);

static VOID CALLBACK on_dll_loaded(ULONG reason, const LdrDllLoadedData *d, PVOID ctx)
{
    (void)ctx;
    if (reason != LDR_DLL_NOTIFICATION_REASON_LOADED || !d || !d->BaseDllName || !d->BaseDllName->Buffer) return;
    static const wchar_t want[] = L"GameAssembly.dll";
    size_t len = d->BaseDllName->Length / sizeof(wchar_t);
    if (len != wcslen(want) || _wcsnicmp(d->BaseDllName->Buffer, want, len) != 0) return;
    hook_il2cpp_init((BYTE *)d->DllBase);
}

/* Unlike patching UnityPlayer's GetProcAddress import, this leaves every import
   untouched, so loaders that patch imports themselves (BepInEx's Doorstop) still work. */
static void watch_for_game_assembly(void)
{
    HMODULE ga = GetModuleHandleW(L"GameAssembly.dll");
    if (ga) { hook_il2cpp_init((BYTE *)ga); return; }
    LdrRegisterDllNotification_t reg =
        (LdrRegisterDllNotification_t)(void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "LdrRegisterDllNotification");
    PVOID cookie;
    if (!reg || reg(0, on_dll_loaded, NULL, &cookie) != 0)
        logf_("Could not watch for GameAssembly.dll - mod inactive.");
}

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(self);
    InitializeCriticalSection(&g_log_cs);
    load_real_dll();

    GetModuleFileNameW(NULL, g_dir, MAX_PATH);
    wchar_t *slash = wcsrchr(g_dir, L'\\');
    if (slash) slash[1] = 0;
    swprintf_s(g_ini, MAX_PATH, L"%sKeepBuilding.ini", g_dir);
    if (ini_int(L"Log", 1)) {
        wchar_t lp[MAX_PATH];
        swprintf_s(lp, MAX_PATH, L"%sKeepBuilding.log", g_dir);
        g_log = _wfsopen(lp, L"w", _SH_DENYWR);
    }

    int enabled = ini_int(L"Enabled", 1);
    logf_("KeepBuilding %s loaded. Enabled=%d ConveyorAfterMachine=%d", MOD_VERSION, enabled,
          ini_int(L"ConveyorAfterMachine", 0));
    if (!enabled) return TRUE;

    watch_for_game_assembly();
    return TRUE;
}
