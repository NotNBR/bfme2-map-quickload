// GPL-3.0-only. bfme2-map-quickload: faster map loading for BFME2 1.06 / ROTWK 2.01
// (https://github.com/NotNBR/bfme2-map-quickload).
//
// While BFME2 1.06 loads a map, most of the time goes into its terrain
// background textures: TerrainTextureClass::updateFlatDXT1 (RVA 0xEF4A3,
// Open-BFME-2 TerrainTex.cpp) composites each texture on the CPU, then
// D3DXLoadSurfaceFromMemory compresses it to DXT1 and D3DXFilterTexture
// builds its mipmaps, one texture after another on the main thread (85% of
// loading Brightwater Fold at Medium, nearly all of it inside D3DX).
//
// This DLL redirects game.dat's two import slots for those functions. Called
// from updateFlatDXT1 while the map loads, the pair becomes a job: worker
// threads run the same two D3DX calls with the same input on a staging copy
// of the texture in D3DPOOL_SCRATCH, made on a Direct3D device of this
// DLL's own (multithreaded, on a window that is never shown), so workers never
// touch the game's device, which is not thread-safe: DXVK crashed on
// Greywater Marches when they did. The main thread copies the finished levels
// into the game's texture. Same functions, same input, so the same bytes.
// Finished jobs are copied in at the next hook call, the rest when the
// launcher calls mwFlush at the end of the map load; from then on, and for
// any other caller or texture, the calls go straight to D3DX. That is the
// launcher build, loading.dll, for tools that load the DLL into the game
// themselves (bfme2-map-studio's test games: a debugger loads it at the first
// engine frame and calls mwFlush when the new-game pass ends, BFME2 RVA
// 0x248767, or at ROTWK's load-screen destruction, RVA 0x225524).
//
// Built with MW_STANDALONE, the same code is dinput8.dll for the normal game
// (copied into the game folder; the game imports DirectInput8Create from
// dinput8.dll, which this forwards to the system's).
// There is no launcher, so the DLL defers while the game shows its loading
// screen (GameLogic +0x120) and finishes the textures itself in the tactical
// view's update (W3DView::updateView, RVA 0x85B36) once the screen is gone,
// before the first frame draws the map; every later map load does the same.
// It also cuts the five seconds the game shows its title logo after loading
// (the 5000 of `beginTime + 5000 > timeGetTime()`, RVA 0x23965F, becomes 0;
// the fade-in after it still runs). In other processes, such as the
// launcher, it only forwards DirectInput8Create.
//
// Build: python build.py (Visual C++ .NET 2003; see README.md).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string.h>
#include <stdlib.h>

extern "C" void *_ReturnAddress(void);
#pragma intrinsic(_ReturnAddress)

typedef unsigned char U8;
typedef unsigned int U32;

// Separate engine layouts. ROTWK addresses were checked against the local
// 2.1.2614.37001 engine (SHA-256 5481de75...ba68a26), then in live skirmishes.
// Select only when image size, both call sites/thunks and import bindings
// agree; the standalone build also checks camera-update bytes. A version
// string alone is not a code fingerprint.
struct Engine {
    U32 imageSize, loadSlot, filterSlot, loadReturn, filterReturn, logic, viewUpdate, titleLogo;
};
static const Engine engines[] = {
    {0xADA000, 0x7BAA14, 0x7BAA18, 0xEF5FC, 0xEF622, 0x9FE78C, 0x85B36, 0x23965D},
    {0xAD3000, 0x7D0A1C, 0x7D0A20, 0xEEFBD, 0xEEFE3, 0x9E412C, 0x8546A, 0x245CB1}
};
static const Engine *engine;

struct LoadingState {
    U32 installed, active, workers, deferred, copied, synchronous, failed, peakQueue;
    U32 lastError, flushes;
};
extern "C" __declspec(dllexport) LoadingState mwLoading = {0};

typedef HRESULT (__stdcall *LoadSurfaceFn)(void*, const void*, const RECT*, const void*, U32, U32, const void*,
                                          const RECT*, U32, U32);
typedef HRESULT (__stdcall *FilterTextureFn)(void*, const void*, U32, U32);
static LoadSurfaceFn loadSurface;
static FilterTextureFn filterTexture;
static U8 *base;

// COM through vtable slots (d3d9.h is not part of this toolchain).
static U32 *vt(void *p) { return *(U32**)p; }
typedef U32 (__stdcall *UnknownFn)(void*);
static void addRef(void *p) { ((UnknownFn)vt(p)[1])(p); }
static void release(void *p) { ((UnknownFn)vt(p)[2])(p); }
typedef HRESULT (__stdcall *GetDeviceFn)(void*, void**);
typedef HRESULT (__stdcall *SurfaceDescFn)(void*, U32*);                     // IDirect3DSurface9::GetDesc (12)
typedef U32 (__stdcall *LevelCountFn)(void*);                                 // IDirect3DBaseTexture9::GetLevelCount (13)
typedef HRESULT (__stdcall *LevelDescFn)(void*, U32, U32*);                   // IDirect3DTexture9::GetLevelDesc (17)
typedef HRESULT (__stdcall *SurfaceLevelFn)(void*, U32, void**);              // GetSurfaceLevel (18)
struct Locked { int pitch; void *bits; };
typedef HRESULT (__stdcall *LockFn)(void*, U32, Locked*, const RECT*, U32);   // LockRect (19)
typedef HRESULT (__stdcall *UnlockFn)(void*, U32);                            // UnlockRect (20)
typedef HRESULT (__stdcall *CreateTextureFn)(void*, U32, U32, U32, U32, U32, U32, void**, void*);   // device (23)
typedef HRESULT (__stdcall *GetDirect3DFn)(void*, void**);                    // IDirect3DDevice9::GetDirect3D (6)
typedef HRESULT (__stdcall *CreateDeviceFn)(void*, U32, U32, HWND, U32, U32*, void**);   // IDirect3D9::CreateDevice (16)
// D3DSURFACE_DESC: Format, Type, Usage, Pool, MultiSampleType, MultiSampleQuality, Width, Height.
enum { FORMAT, TYPE, USAGE, POOL, WIDTH = 6, HEIGHT = 7 };
static const U32 POOL_MANAGED = 1, POOL_SCRATCH = 3;
static const U32 DXT1 = 0x31545844, DXT3 = 0x33545844, DXT5 = 0x35545844;

struct Job {
    void *texture, *staging;
    U8 *pixels;
    U32 format, pitch, loadFilter, colorKey, mipFilter;
    RECT rect;
    HRESULT result;
    Job *next;
};

static CRITICAL_SECTION lock;
static HANDLE queued, slots, idle;          // jobs to take; free queue slots; no job outstanding
static Job *todo, *finished;
static volatile LONG outstanding;
static Job *pending;                        // compression seen, mipmaps not yet: one texture at a time
static bool deferring();                    // whether this call should become a job
static void *ownDevice;                     // the staging textures' device
static HWND ownWindow;

// The device the staging textures are made on: created once, from the game's
// own Direct3D object, windowed on a window of ours that is never shown, with
// D3DCREATE_MULTITHREADED. 0 when it cannot be made (then nothing is deferred).
static void *stagingDevice(void *gameDevice) {
    if (ownDevice || mwLoading.lastError == 0xDEADD3D9) return ownDevice;
    void *d3d = 0;
    if (FAILED(((GetDirect3DFn)vt(gameDevice)[6])(gameDevice, &d3d)) || !d3d) { mwLoading.lastError = 0xDEADD3D9; return 0; }
    ownWindow = CreateWindowExA(0, "STATIC", "mapworkshop loading", WS_POPUP, 0, 0, 16, 16, 0, 0, 0, 0);
    // D3DPRESENT_PARAMETERS: 16 x 16 back buffer, D3DSWAPEFFECT_DISCARD, windowed.
    U32 params[14] = {16, 16, 0, 1, 0, 0, 1, (U32)ownWindow, 1, 0, 0, 0, 0, 0};
    HRESULT made = ((CreateDeviceFn)vt(d3d)[16])(d3d, 0, 1, ownWindow,     // D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL
                                                 0x20 | 0x4 | 0x2,          // software vertex processing, multithreaded, FPU preserve
                                                 params, &ownDevice);
    release(d3d);
    if (FAILED(made)) { ownDevice = 0; mwLoading.lastError = 0xDEADD3D9; }
    return ownDevice;
}

static void push(Job **list, Job *job) {
    EnterCriticalSection(&lock);
    job->next = *list; *list = job;
    LeaveCriticalSection(&lock);
}

static Job *take(Job **list) {
    EnterCriticalSection(&lock);
    Job *job = *list;
    if (job) *list = job->next;
    LeaveCriticalSection(&lock);
    return job;
}

static DWORD WINAPI worker(void *) {
    for (;;) {
        WaitForSingleObject(queued, INFINITE);
        Job *job = take(&todo);
        if (!job) continue;
        void *surface = 0;
        job->result = ((SurfaceLevelFn)vt(job->staging)[18])(job->staging, 0, &surface);
        if (SUCCEEDED(job->result)) {
            job->result = loadSurface(surface, 0, 0, job->pixels, job->format, job->pitch, 0, &job->rect,
                                      job->loadFilter, job->colorKey);
            release(surface);
        }
        if (SUCCEEDED(job->result)) job->result = filterTexture(job->staging, 0, 0, job->mipFilter);
        free(job->pixels); job->pixels = 0;
        push(&finished, job);
        ReleaseSemaphore(slots, 1, 0);
        if (InterlockedDecrement(&outstanding) == 0) SetEvent(idle);
    }
    return 0;
}

static void start() {
    if (mwLoading.workers) return;
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    U32 n = info.dwNumberOfProcessors > 1 ? info.dwNumberOfProcessors - 1 : 1;
    if (n > 15) n = 15;
    InitializeCriticalSection(&lock);
    queued = CreateSemaphoreA(0, 0, 0x7FFFFFFF, 0);
    slots = CreateSemaphoreA(0, 2 * n, 2 * n, 0);
    idle = CreateEventA(0, TRUE, TRUE, 0);
    for (U32 i = 0; i < n; i++) CloseHandle(CreateThread(0, 0, worker, 0, 0, 0));
    mwLoading.workers = n;
}

static U32 blockRows(U32 format, U32 height) {
    return format == DXT1 || format == DXT3 || format == DXT5 ? (height + 3) / 4 : height;
}

// Main thread: copy each finished staging texture into the game's texture.
static void drain() {
    if (!mwLoading.workers) return;            // no lock or lists before the first job
    Job *job;
    while ((job = take(&finished)) != 0) {
        if (SUCCEEDED(job->result)) {
            U32 levels = ((LevelCountFn)vt(job->texture)[13])(job->texture);
            for (U32 level = 0; level < levels; level++) {
                U32 desc[8];
                Locked from, to;
                if (FAILED(((LevelDescFn)vt(job->texture)[17])(job->texture, level, desc))) break;
                if (FAILED(((LockFn)vt(job->staging)[19])(job->staging, level, &from, 0, 0x10))) break;  // READONLY
                if (SUCCEEDED(((LockFn)vt(job->texture)[19])(job->texture, level, &to, 0, 0))) {
                    U32 bytes = from.pitch < to.pitch ? from.pitch : to.pitch;
                    U32 rows = blockRows(desc[FORMAT], desc[HEIGHT]);
                    for (U32 y = 0; y < rows; y++)
                        memcpy((U8*)to.bits + y * to.pitch, (U8*)from.bits + y * from.pitch, bytes);
                    ((UnlockFn)vt(job->texture)[20])(job->texture, level);
                }
                ((UnlockFn)vt(job->staging)[20])(job->staging, level);
            }
            mwLoading.copied++;
        } else {
            mwLoading.failed++; mwLoading.lastError = job->result;
        }
        release(job->staging);
        release(job->texture);
        free(job);
    }
}

static HRESULT __stdcall loadSurfaceHook(void *dest, const void *destPalette, const RECT *destRect, const void *src,
                                         U32 format, U32 pitch, const void *srcPalette, const RECT *srcRect,
                                         U32 filter, U32 colorKey) {
    if (deferring() && (U8*)_ReturnAddress() == base + engine->loadReturn && !destPalette && !destRect && !srcPalette &&
        srcRect && !pending) {
        U32 desc[8];
        if (SUCCEEDED(((SurfaceDescFn)vt(dest)[12])(dest, desc)) && desc[POOL] == POOL_MANAGED) {
            drain();
            U32 rows = srcRect->bottom - srcRect->top;
            Job *job = (Job*)calloc(1, sizeof(Job));
            U8 *pixels = job ? (U8*)malloc(pitch * rows) : 0;
            if (pixels) {
                const U8 *first = (const U8*)src + srcRect->top * pitch;
                memcpy(pixels, first, pitch * rows);
                job->pixels = pixels; job->format = format; job->pitch = pitch;
                job->loadFilter = filter; job->colorKey = colorKey;
                job->rect = *srcRect; job->rect.top = 0; job->rect.bottom = rows;
                pending = job;
                return 0;
            }
            free(job);
        }
    }
    return loadSurface(dest, destPalette, destRect, src, format, pitch, srcPalette, srcRect, filter, colorKey);
}

static HRESULT runNow(Job *job, void *texture) {
    void *surface = 0;
    HRESULT result = ((SurfaceLevelFn)vt(texture)[18])(texture, 0, &surface);
    if (SUCCEEDED(result)) {
        result = loadSurface(surface, 0, 0, job->pixels, job->format, job->pitch, 0, &job->rect, job->loadFilter,
                             job->colorKey);
        release(surface);
    }
    mwLoading.synchronous++;
    return result;
}

static HRESULT __stdcall filterTextureHook(void *texture, const void *palette, U32 level, U32 filter) {
    Job *job = pending;
    if (!job || (U8*)_ReturnAddress() != base + engine->filterReturn || palette || level)
        return filterTexture(texture, palette, level, filter);
    pending = 0;
    U32 desc[8], levels = ((LevelCountFn)vt(texture)[13])(texture);
    void *gameDevice = 0, *device = 0, *staging = 0;
    HRESULT made = ((LevelDescFn)vt(texture)[17])(texture, 0, desc);
    if (SUCCEEDED(made)) made = ((GetDeviceFn)vt(texture)[3])(texture, &gameDevice);
    if (SUCCEEDED(made)) {
        device = stagingDevice(gameDevice);
        release(gameDevice);
        made = device ? ((CreateTextureFn)vt(device)[23])(device, desc[WIDTH], desc[HEIGHT], levels, 0, desc[FORMAT],
                                                          POOL_SCRATCH, &staging, 0) : E_FAIL;
    }
    if (FAILED(made) || !staging) {             // no staging copy: the game's own order, on this thread
        HRESULT result = runNow(job, texture);
        free(job->pixels); free(job);
        return SUCCEEDED(result) ? filterTexture(texture, palette, level, filter) : result;
    }
    start();
    addRef(texture);
    job->texture = texture; job->staging = staging; job->mipFilter = filter;
    while (WaitForSingleObject(slots, 50) == WAIT_TIMEOUT) drain();
    if (InterlockedIncrement(&outstanding) == 1) ResetEvent(idle);
    if ((U32)outstanding > mwLoading.peakQueue) mwLoading.peakQueue = outstanding;
    push(&todo, job);
    ReleaseSemaphore(queued, 1, 0);
    mwLoading.deferred++;
    drain();
    return 0;
}

// Main thread: wait for the workers, copy the rest in, drop the staging device.
static void finish() {
    mwLoading.flushes++;
    if (mwLoading.workers)
        while (outstanding > 0) { WaitForSingleObject(idle, 50); drain(); }
    drain();
    if (ownDevice) { release(ownDevice); ownDevice = 0; }
    if (ownWindow) { DestroyWindow(ownWindow); ownWindow = 0; }
}

// Called by the launcher on the main thread at the end of the map load.
extern "C" __declspec(dllexport) U32 __cdecl mwFlush() {
    finish();
    mwLoading.active = 0;
    return mwLoading.copied;
}

#ifdef MW_STANDALONE
static bool loadingNow() {                  // the game shows its loading screen
    void *logic = *(void**)(base + engine->logic);                  // TheGameLogic
    return logic && *(void**)((U8*)logic + 0x120) != 0;              // its LoadScreen
}
static bool deferring() { return mwLoading.active && loadingNow(); }

static void *updateOriginal;
typedef void (__fastcall *ViewFn)(void*, void*);
static void __fastcall updateHook(void *view, void *edx) {
    if (mwLoading.workers && (outstanding > 0 || finished) && !loadingNow()) finish();
    ((ViewFn)updateOriginal)(view, edx);
}

// Replace `length` bytes at rva (checked against `expected`) with a jump to
// `replacement`; returns a trampoline running those bytes, then the rest.
static void *hook(U32 rva, void *replacement, const U8 *expected, int length) {
    U8 *src = base + rva;
    if (memcmp(src, expected, length)) return 0;
    U8 *stub = (U8*)VirtualAlloc(0, length + 5, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!stub) return 0;
    memcpy(stub, src, length);
    stub[length] = 0xe9;
    *(U32*)(stub + length + 1) = (U32)(src + length) - (U32)(stub + length + 5);
    DWORD old;
    if (!VirtualProtect(src, length, PAGE_EXECUTE_READWRITE, &old)) return 0;
    src[0] = 0xe9;
    *(U32*)(src + 1) = (U32)replacement - (U32)(src + 5);
    for (int i = 5; i < length; i++) src[i] = 0x90;
    VirtualProtect(src, length, old, &old);
    FlushInstructionCache(GetCurrentProcess(), src, length);
    return stub;
}

static void skipTitleLogo() {
    static const U8 expected[] = {0x8D, 0xB8, 0x88, 0x13, 0x00, 0x00};    // lea edi, [eax+5000]
    U8 *at = base + engine->titleLogo;
    DWORD old;
    if (memcmp(at, expected, 6) || !VirtualProtect(at, 6, PAGE_EXECUTE_READWRITE, &old)) return;
    memset(at + 2, 0, 4);
    VirtualProtect(at, 6, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, 6);
}
#else
static bool deferring() { return mwLoading.active != 0; }
#endif

static bool patchSlot(U32 rva, void *hook, void **original) {
    void **slot = (void**)(base + rva);
    DWORD old;
    if (!VirtualProtect(slot, 4, PAGE_READWRITE, &old)) return false;
    *original = *slot;
    *slot = hook;
    VirtualProtect(slot, 4, old, &old);
    return true;
}

// Verify a relative CALL to the import thunk, and the thunk's exact IAT slot.
static bool callsSlot(U32 after, U32 slot, U32 imageSize) {
    if (after < 5 || after >= imageSize || slot > imageSize - 4) return false;
    U8 *call = base + after - 5;
    if (*call != 0xe8) return false;
    U32 thunk = after + *(U32*)(call + 1);
    if (thunk > imageSize - 6) return false;
    U8 *at = base + thunk;
    return at[0] == 0xff && at[1] == 0x25 && *(U32*)(at + 2) == (U32)(base + slot);
}

static const Engine *identify(HMODULE d3dx) {
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 || dos->e_lfanew > 0x1000) return 0;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != 0x10b) return 0;
    for (U32 i = 0; i < sizeof(engines)/sizeof(engines[0]); i++) {
        const Engine *candidate = engines + i;
        if (nt->OptionalHeader.SizeOfImage != candidate->imageSize ||
            !callsSlot(candidate->loadReturn, candidate->loadSlot, candidate->imageSize) ||
            !callsSlot(candidate->filterReturn, candidate->filterSlot, candidate->imageSize)) continue;
        if (*(void**)(base + candidate->loadSlot) == (void*)GetProcAddress(d3dx, "D3DXLoadSurfaceFromMemory") &&
            *(void**)(base + candidate->filterSlot) == (void*)GetProcAddress(d3dx, "D3DXFilterTexture"))
            return candidate;
    }
    return 0;
}

// Hook a recognized engine; false (and nothing changed) in other processes.
static bool install() {
    if (mwLoading.installed) return true;
    base = (U8*)GetModuleHandleA(0);
    HMODULE d3dx = GetModuleHandleA("d3dx9_27.dll");
    if (base != (U8*)0x400000 || !d3dx) return false;
    engine = identify(d3dx);
    if (!engine) return false;
#ifdef MW_STANDALONE
    // Without the end-of-load hook nothing may be deferred: it comes first.
    static const U8 updateBytes[] = {0x81, 0xC1, 0xB4, 0, 0, 0};          // add ecx, 0B4h
    if (!updateOriginal) updateOriginal = hook(engine->viewUpdate, (void*)updateHook, updateBytes, 6);
    if (!updateOriginal) return false;
    skipTitleLogo();
#endif
    if (!patchSlot(engine->loadSlot, (void*)loadSurfaceHook, (void**)&loadSurface) ||
        !patchSlot(engine->filterSlot, (void*)filterTextureHook, (void**)&filterTexture))
        return false;
    mwLoading.installed = mwLoading.active = 1;
    return true;
}

#ifdef MW_STANDALONE
// dinput8.dll's one function the game uses, forwarded to the system's. The
// hooks go in here too if the import slots were not bound yet at DllMain.
typedef HRESULT (WINAPI *DirectInput8CreateFn)(HINSTANCE, DWORD, const void*, void**, void*);
extern "C" HRESULT WINAPI DirectInput8Create(HINSTANCE instance, DWORD version, const void *iid, void **out,
                                             void *outer) {
    static DirectInput8CreateFn real;
    install();
    if (!real) {
        char path[MAX_PATH + 16];
        UINT n = GetSystemDirectoryA(path, MAX_PATH);
        if (n && n < MAX_PATH) {
            lstrcatA(path, "\\dinput8.dll");
            HMODULE system = LoadLibraryA(path);
            if (system) real = (DirectInput8CreateFn)GetProcAddress(system, "DirectInput8Create");
        }
    }
    return real ? real(instance, version, iid, out, outer) : E_FAIL;
}
#endif

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(instance);
#ifdef MW_STANDALONE
    install();
    return TRUE;                            // dinput8.dll must always load
#else
    return install() ? TRUE : FALSE;
#endif
}
