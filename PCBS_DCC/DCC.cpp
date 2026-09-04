#include <windows.h>
#include <cstdint>
#include <tlhelp32.h>

typedef void* MonoDomain;
typedef void* MonoAssembly;
typedef void* MonoImage;
typedef void* MonoClass;
typedef void* MonoClassField;
typedef void* MonoMethod;
typedef void* MonoObject;
typedef void* MonoException;

typedef MonoDomain* (__cdecl* mono_get_root_domain_t)();
typedef MonoDomain* (__cdecl* mono_thread_attach_t)(MonoDomain*);
typedef void (__cdecl* mono_assembly_foreach_callback_t)(MonoAssembly*, void*);
typedef void (__cdecl* mono_assembly_foreach_t)(mono_assembly_foreach_callback_t, void*);
typedef MonoImage* (__cdecl* mono_assembly_get_image_t)(MonoAssembly*);
typedef const char* (__cdecl* mono_image_get_name_t)(MonoImage*);
typedef MonoClass* (__cdecl* mono_class_from_name_t)(MonoImage*, const char*, const char*);
typedef MonoMethod* (__cdecl* mono_class_get_method_from_name_t)(MonoClass*, const char*, int);
typedef MonoObject* (__cdecl* mono_runtime_invoke_t)(MonoMethod*, void*, void**, MonoException**);
typedef MonoClassField* (__cdecl* mono_class_get_field_from_name_t)(MonoClass*, const char*);
typedef void (__cdecl* mono_field_get_value_t)(MonoObject*, MonoClassField*, void*);
typedef int32_t (__cdecl* mono_field_get_offset_t)(MonoClassField*);

struct MonoFunctions {
    mono_get_root_domain_t getRoot;
    mono_thread_attach_t attach;
    mono_assembly_foreach_t assemblyForeach;
    mono_assembly_get_image_t assemblyGetImage;
    mono_image_get_name_t imageGetName;
    mono_class_from_name_t classFromName;
    mono_class_get_method_from_name_t getMethod;
    mono_runtime_invoke_t runtimeInvoke;
    mono_class_get_field_from_name_t fieldFromName;
    mono_field_get_value_t fieldGetValue;
    mono_field_get_offset_t fieldGetOffset;
};

struct CachedObjects {
    MonoImage* gameAssembly;
    MonoClass* workshopClass;
    MonoClassField* carryingField;
    int32_t fieldOffset;
    MonoImage* unityImage;
    MonoClass* componentClass;
    MonoClass* objectClass;
    MonoMethod* getGameObject;
    MonoMethod* destroyMethod;
    MonoClass* gameControllerClass;
    MonoMethod* getPausedMethod;
    MonoMethod* getGameControllerMethod;
    bool initialized;
};

static MonoFunctions g_mono;
static CachedObjects g_cache = {0};
static HWND g_gameWindow = nullptr;
static DWORD g_gameProcessId = 0;
static HANDLE g_processHandle = nullptr;
static HANDLE g_stopEvent = nullptr;

struct SearchData {
    mono_assembly_get_image_t getImage;
    mono_image_get_name_t getName;
    MonoImage* result;
};

static void __cdecl AssemblyCallback(MonoAssembly* assembly, void* userdata) {
    SearchData* d = (SearchData*)userdata;
    MonoImage* image = d->getImage(assembly);
    if (!image) return;
    const char* name = d->getName(image);
    if (!name) return;
    if (_stricmp(name, "Assembly-CSharp-firstpass") == 0)
        d->result = image;
}

static void __cdecl UnityAssemblyCallback(MonoAssembly* assembly, void* userdata) {
    SearchData* d = (SearchData*)userdata;
    MonoImage* image = d->getImage(assembly);
    if (!image) return;
    const char* name = d->getName(image);
    if (!name) return;
    if (_stricmp(name, "UnityEngine") == 0)
        d->result = image;
}

static DWORD GetProcessIdByName(const char* processName) {
    DWORD pid = 0;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32 entry;
    entry.dwSize = sizeof(PROCESSENTRY32);
    if (Process32First(snapshot, &entry)) {
        do {
            if (_stricmp(entry.szExeFile, processName) == 0) {
                pid = entry.th32ProcessID;
                break;
            }
        } while (Process32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return pid;
}

static BOOL CALLBACK FindGameWindowCallback(HWND hwnd, LPARAM lParam) {
    DWORD processId;
    GetWindowThreadProcessId(hwnd, &processId);
    if (processId == g_gameProcessId && IsWindowVisible(hwnd)) {
        char windowTitle[256];
        GetWindowTextA(hwnd, windowTitle, sizeof(windowTitle));
        if (strlen(windowTitle) > 0) {
            *(HWND*)lParam = hwnd;
            return FALSE;
        }
    }
    return TRUE;
}

static HWND FindGameWindow() {
    g_gameProcessId = GetProcessIdByName("PCBS.exe");
    if (g_gameProcessId == 0) return nullptr;
    HWND hwnd = nullptr;
    EnumWindows(FindGameWindowCallback, (LPARAM)&hwnd);
    return hwnd ? hwnd : FindWindowA(nullptr, "PC Building Simulator");
}

static inline bool IsGameActive() {
    if (!g_gameWindow) return false;
    if (!IsWindow(g_gameWindow) || !IsWindowVisible(g_gameWindow) || IsIconic(g_gameWindow))
        return false;
    return GetForegroundWindow() == g_gameWindow;
}

static inline bool IsGameRunning() {
    if (!g_processHandle) return false;
    DWORD exitCode;
    return GetExitCodeProcess(g_processHandle, &exitCode) && exitCode == STILL_ACTIVE;
}

static inline bool IsGamePaused() {
    if (!g_cache.initialized || !g_cache.getPausedMethod) return false;
    MonoException* exception = nullptr;
    MonoObject* gameController = g_mono.runtimeInvoke(g_cache.getGameControllerMethod, nullptr, nullptr, &exception);
    if (exception || !gameController) return false;
    exception = nullptr;
    MonoObject* pausedResult = g_mono.runtimeInvoke(g_cache.getPausedMethod, gameController, nullptr, &exception);
    if (exception || !pausedResult) return false;
    return *(bool*)pausedResult;
}

bool InitializeMono() {
    g_gameWindow = FindGameWindow();
    if (g_gameProcessId) {
        g_processHandle = OpenProcess(PROCESS_QUERY_INFORMATION | SYNCHRONIZE, FALSE, g_gameProcessId);
    }
    
    HMODULE mono = GetModuleHandleA("mono.dll");
    if (!mono) return false;
    
    g_mono.getRoot = (mono_get_root_domain_t)GetProcAddress(mono, "mono_get_root_domain");
    g_mono.attach = (mono_thread_attach_t)GetProcAddress(mono, "mono_thread_attach");
    g_mono.assemblyForeach = (mono_assembly_foreach_t)GetProcAddress(mono, "mono_assembly_foreach");
    g_mono.assemblyGetImage = (mono_assembly_get_image_t)GetProcAddress(mono, "mono_assembly_get_image");
    g_mono.imageGetName = (mono_image_get_name_t)GetProcAddress(mono, "mono_image_get_name");
    g_mono.classFromName = (mono_class_from_name_t)GetProcAddress(mono, "mono_class_from_name");
    g_mono.getMethod = (mono_class_get_method_from_name_t)GetProcAddress(mono, "mono_class_get_method_from_name");
    g_mono.runtimeInvoke = (mono_runtime_invoke_t)GetProcAddress(mono, "mono_runtime_invoke");
    g_mono.fieldFromName = (mono_class_get_field_from_name_t)GetProcAddress(mono, "mono_class_get_field_from_name");
    g_mono.fieldGetValue = (mono_field_get_value_t)GetProcAddress(mono, "mono_field_get_value");
    g_mono.fieldGetOffset = (mono_field_get_offset_t)GetProcAddress(mono, "mono_field_get_offset");
    
    if (!g_mono.getRoot || !g_mono.attach || !g_mono.assemblyForeach ||
        !g_mono.assemblyGetImage || !g_mono.imageGetName || !g_mono.classFromName ||
        !g_mono.getMethod || !g_mono.runtimeInvoke || !g_mono.fieldFromName ||
        !g_mono.fieldGetValue || !g_mono.fieldGetOffset)
        return false;
    
    MonoDomain* domain = g_mono.getRoot();
    if (!domain) return false;
    g_mono.attach(domain);
    
    SearchData search{g_mono.assemblyGetImage, g_mono.imageGetName, nullptr};
    g_mono.assemblyForeach(AssemblyCallback, &search);
    g_cache.gameAssembly = search.result;
    if (!g_cache.gameAssembly) return false;
    
    g_cache.workshopClass = g_mono.classFromName(g_cache.gameAssembly, "", "WorkshopController");
    if (!g_cache.workshopClass) return false;
    
    g_cache.carryingField = g_mono.fieldFromName(g_cache.workshopClass, "m_carryingCase");
    if (!g_cache.carryingField) return false;
    
    g_cache.fieldOffset = g_mono.fieldGetOffset(g_cache.carryingField);
    if (g_cache.fieldOffset != 0x48) return false;
    
    SearchData unitySearch{g_mono.assemblyGetImage, g_mono.imageGetName, nullptr};
    g_mono.assemblyForeach(UnityAssemblyCallback, &unitySearch);
    g_cache.unityImage = unitySearch.result;
    if (!g_cache.unityImage) return false;
    
    g_cache.componentClass = g_mono.classFromName(g_cache.unityImage, "UnityEngine", "Component");
    if (!g_cache.componentClass) return false;
    
    g_cache.getGameObject = g_mono.getMethod(g_cache.componentClass, "get_gameObject", 0);
    if (!g_cache.getGameObject) return false;
    
    g_cache.objectClass = g_mono.classFromName(g_cache.unityImage, "UnityEngine", "Object");
    if (!g_cache.objectClass) return false;
    
    g_cache.destroyMethod = g_mono.getMethod(g_cache.objectClass, "Destroy", 1);
    if (!g_cache.destroyMethod) return false;
    
    g_cache.gameControllerClass = g_mono.classFromName(g_cache.gameAssembly, "", "GameController");
    if (g_cache.gameControllerClass) {
        g_cache.getGameControllerMethod = g_mono.getMethod(g_cache.gameControllerClass, "Get", 0);
        if (g_cache.getGameControllerMethod)
            g_cache.getPausedMethod = g_mono.getMethod(g_cache.gameControllerClass, "GetPaused", 0);
    }
    
    g_cache.initialized = true;
    return true;
}

bool RemoveCurrentCase() {
    if (!g_cache.initialized || !IsGameActive() || IsGamePaused()) return false;
    
    MonoMethod* getController = g_mono.getMethod(g_cache.workshopClass, "Get", 0);
    if (!getController) return false;
    
    MonoException* exception = nullptr;
    MonoObject* controller = g_mono.runtimeInvoke(getController, nullptr, nullptr, &exception);
    if (exception || !controller) return false;
    
    MonoObject* carryingCase = nullptr;
    g_mono.fieldGetValue(controller, g_cache.carryingField, &carryingCase);
    if (!carryingCase) return true;
    
    uintptr_t fieldAddress = (uintptr_t)controller + (uintptr_t)g_cache.fieldOffset;
    
    exception = nullptr;
    MonoObject* gameObject = g_mono.runtimeInvoke(g_cache.getGameObject, carryingCase, nullptr, &exception);
    if (exception || !gameObject) return false;
    
    void* destroyArgs[1] = { gameObject };
    exception = nullptr;
    g_mono.runtimeInvoke(g_cache.destroyMethod, nullptr, destroyArgs, &exception);
    if (exception) return false;
    
    *(uintptr_t*)fieldAddress = 0;
    return true;
}

DWORD WINAPI MainThread(LPVOID) {
    InitializeMono();
    
    bool lastF5State = false;
    DWORD lastCheckTime = GetTickCount();
    
    HANDLE waitHandles[2] = { g_stopEvent, g_processHandle };
    
    while (true) {
        // Wait for events with 20ms timeout - yields the CPU
        DWORD waitResult = WaitForMultipleObjects(2, waitHandles, FALSE, 20);
        
        // If stop event or process exit, break immediately
        if (waitResult == WAIT_OBJECT_0 || waitResult == WAIT_OBJECT_0 + 1) {
            break;
        }
        
        // Periodic game running check (fallback)
        DWORD currentTime = GetTickCount();
        if (currentTime - lastCheckTime >= 1000) {
            lastCheckTime = currentTime;
            if (!g_processHandle || WaitForSingleObject(g_processHandle, 0) == WAIT_OBJECT_0) {
                break;
            }
        }
        
        // Check F5 key
        bool currentF5State = (GetAsyncKeyState(VK_F5) & 0x8000) != 0;
        if (currentF5State && !lastF5State && g_cache.initialized) {
            RemoveCurrentCase();
        }
        lastF5State = currentF5State;
    }
    
    // Clean up
    if (g_processHandle) {
        CloseHandle(g_processHandle);
        g_processHandle = nullptr;
    }
    
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        
        // Create stop event
        g_stopEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
        
        // Create worker thread
        HANDLE thread = CreateThread(nullptr, 0, MainThread, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
    }
    else if (reason == DLL_PROCESS_DETACH) {
        // Signal the thread to stop
        if (g_stopEvent) {
            SetEvent(g_stopEvent);
        }
        // Don't wait - avoid deadlock
    }
    return TRUE;
}