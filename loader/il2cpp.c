#include <stdlib.h>
#include <string.h>

#include "loader.h"

#define IL2CPP_FUNCTIONS(X) \
    X(void *, domain_get, (void)) \
    X(void *, domain_assembly_open, (void *, const char *)) \
    X(void *, assembly_get_image, (void *)) \
    X(void *, class_from_name, (void *, const char *, const char *)) \
    X(void *, class_get_method_from_name, (void *, const char *, int)) \
    X(void *, class_get_field_from_name, (void *, const char *)) \
    X(size_t, field_get_offset, (void *)) \
    X(void *, object_get_class, (void *)) \
    X(void *, class_from_type, (const void *)) \
    X(const void *, method_get_param, (const void *, uint32_t)) \
    X(void *, object_new, (void *)) \
    X(void *, runtime_invoke, (const void *, void *, void **, void **)) \
    X(void *, string_new, (const char *)) \
    X(void *, array_new, (void *, uintptr_t)) \
    X(int, class_array_element_size, (const void *)) \
    X(void *, class_get_element_class, (void *)) \
    X(GcHandle, gchandle_new, (void *, uint8_t)) \
    X(void *, gchandle_get_target, (GcHandle)) \
    X(void, gchandle_free, (GcHandle)) \
    X(void, gc_wbarrier_set_field, (void *, void **, void *)) \
    X(void, format_exception, (const void *, char *, int))

#define DECLARE(ret, name, args) static ret(*il2cpp_##name) args;
IL2CPP_FUNCTIONS(DECLARE)

int invokeError;

int il2cppLoad(HMODULE module)
{
    int missing = 0;
#define RESOLVE(ret, name, args) \
    il2cpp_##name = (ret(*) args)(void *)GetProcAddress(module, "il2cpp_" #name); \
    if (!il2cpp_##name) { \
        loaderLog("il2cpp-Funktion fehlt: il2cpp_%s", #name); \
        missing++; \
    }
    IL2CPP_FUNCTIONS(RESOLVE)
    return missing == 0;
}

void *il2cppExport(const char *name)
{
    return gameAssembly ? (void *)GetProcAddress(gameAssembly, name) : NULL;
}

Il2CppClass *findClass(const char *assembly, const char *namespaze, const char *name)
{
    void *domain = il2cpp_domain_get();
    void *loaded = domain ? il2cpp_domain_assembly_open(domain, assembly) : NULL;
    void *image = loaded ? il2cpp_assembly_get_image(loaded) : NULL;
    return image ? il2cpp_class_from_name(image, namespaze, name) : NULL;
}

MethodInfo *classMethod(Il2CppClass *klass, const char *name, int argc)
{
    return klass ? il2cpp_class_get_method_from_name(klass, name, argc) : NULL;
}

MethodInfo *findMethod(const char *assembly, const char *namespaze, const char *klass, const char *name, int argc)
{
    Il2CppClass *found = findClass(assembly, namespaze, klass);
    MethodInfo *method = found ? il2cpp_class_get_method_from_name(found, name, argc) : NULL;
    if (!method)
        loaderLog("Spiel-Funktion nicht gefunden: %s.%s.%s(%d)", namespaze, klass, name, argc);
    return method;
}

void *methodCode(const MethodInfo *method)
{
    if (!method)
        return NULL;
    unsigned char *code = *(unsigned char *const *)method;
    return code >= gameAssemblyStart && code < gameAssemblyEnd ? code : NULL;
}

Il2CppClass *paramClass(const MethodInfo *method, int index)
{
    const void *type = method ? il2cpp_method_get_param(method, index) : NULL;
    return type ? il2cpp_class_from_type(type) : NULL;
}

Il2CppClass *elementClass(Il2CppClass *arrayClass)
{
    return arrayClass ? il2cpp_class_get_element_class(arrayClass) : NULL;
}

int arrayElementSize(Il2CppClass *arrayClass)
{
    return il2cpp_class_array_element_size(arrayClass);
}

Il2CppClass *classOf(Il2CppObject *object)
{
    return object ? il2cpp_object_get_class(object) : NULL;
}

int fieldOffset(Il2CppClass *klass, const char *name)
{
    FieldInfo *field = klass ? il2cpp_class_get_field_from_name(klass, name) : NULL;
    return field ? (int)il2cpp_field_get_offset(field) : -1;
}

Il2CppString *newString(const char *utf8)
{
    return il2cpp_string_new(utf8);
}

Il2CppObject *newArray(Il2CppClass *element, int length)
{
    return il2cpp_array_new(element, (uintptr_t)length);
}

void setArrayItem(Il2CppObject *array, int index, Il2CppObject *item)
{
    void **slot = (void **)((unsigned char *)array + 0x20) + index;
    il2cpp_gc_wbarrier_set_field(array, slot, item);
}

Il2CppObject *invoke(const MethodInfo *method, void *instance, void **params)
{
    void *exception = NULL;
    invokeError = 0;
    if (!method) {
        invokeError = 1;
        return NULL;
    }
    Il2CppObject *result = il2cpp_runtime_invoke(method, instance, params, &exception);
    if (exception) {
        char message[1024] = "";
        il2cpp_format_exception(exception, message, sizeof message);
        loaderLog("Fehler im Spiel-Code: %s", message);
        invokeError = 1;
        return NULL;
    }
    return result;
}

GcHandle keepAlive(Il2CppObject *object)
{
    return object ? il2cpp_gchandle_new(object, 0) : NULL;
}

Il2CppObject *aliveTarget(GcHandle handle)
{
    return handle ? il2cpp_gchandle_get_target(handle) : NULL;
}

void release(GcHandle handle)
{
    if (handle)
        il2cpp_gchandle_free(handle);
}

int unityAlive(Il2CppObject *object)
{
    return object && *(void **)((unsigned char *)object + 0x10);
}

int stringToUtf8(Il2CppString *string, char *buffer, int size)
{
    buffer[0] = 0;
    if (!string)
        return 0;
    int length = *(int32_t *)((unsigned char *)string + 0x10);
    const wchar_t *chars = (const wchar_t *)((unsigned char *)string + 0x14);
    int written = WideCharToMultiByte(CP_UTF8, 0, chars, length, buffer, size - 1, NULL, NULL);
    buffer[written > 0 ? written : 0] = 0;
    return written;
}

static void thunkAction(NativeCall *call) { call->action(call->context); }
static void thunkActionObject(Il2CppObject *argument, NativeCall *call) { call->actionObject(call->context, argument); }
static uint8_t thunkFuncBool(NativeCall *call) { return call->getValue(call->context) != 0; }
static void thunkActionBool(uint8_t value, NativeCall *call) { call->setValue(call->context, value != 0); }
static int32_t thunkFuncInt(NativeCall *call) { return call->getValue(call->context); }
static void thunkActionInt(int32_t value, NativeCall *call) { call->setValue(call->context, value); }

/** Game methods with the same shape as each delegate kind; their method info is copied and pointed at our thunk */
static const struct {
    const char *klass, *name;
    int argc;
    void *thunk;
} shapes[CALL_KIND_COUNT] = {
    [CALL_ACTION] = { "MoonWhiteboard", "SaveAll", 0, (void *)thunkAction },
    [CALL_ACTION_OBJECT] = { "MoonAchievements", "Attach", 1, (void *)thunkActionObject },
    [CALL_FUNC_BOOL] = { "MoonSteamInput", "get_Active", 0, (void *)thunkFuncBool },
    [CALL_ACTION_BOOL] = { "MoonSteamLobby", "set_Busy", 1, (void *)thunkActionBool },
    [CALL_FUNC_INT] = { "MoonSteamLobby", "get_LocalMemberIndex", 0, (void *)thunkFuncInt },
    [CALL_ACTION_INT] = { "MoonSaveSlots", "set_Active", 1, (void *)thunkActionInt },
};

static const MethodInfo *shapeMethods[CALL_KIND_COUNT];

NativeCall *newCall(CallKind kind, void *context)
{
    if (!shapeMethods[kind]) {
        shapeMethods[kind] = findMethod("Assembly-CSharp", "MoonCleaners", shapes[kind].klass, shapes[kind].name, shapes[kind].argc);
        if (!methodCode(shapeMethods[kind])) {
            loaderLog("Vorlage fuer Rueckruf %d fehlt", kind);
            shapeMethods[kind] = NULL;
            return NULL;
        }
    }
    NativeCall *call = calloc(1, sizeof *call);
    memcpy(call->method, shapeMethods[kind], sizeof call->method);
    void *original = *(void **)call->method;
    memcpy(call->method, &shapes[kind].thunk, sizeof(void *));
    if (memcmp(call->method + sizeof(void *), &original, sizeof(void *)) == 0)
        memcpy(call->method + sizeof(void *), &shapes[kind].thunk, sizeof(void *));
    call->kind = kind;
    call->context = context;
    return call;
}

Il2CppObject *callDelegate(NativeCall *call, Il2CppClass *delegateClass)
{
    if (!call || !delegateClass)
        return NULL;
    Il2CppObject *existing = aliveTarget(call->handle);
    if (existing)
        return existing;
    Il2CppObject *delegate = il2cpp_object_new(delegateClass);
    const MethodInfo *constructor = il2cpp_class_get_method_from_name(delegateClass, ".ctor", 2);
    intptr_t target = (intptr_t)call->method;
    void *params[2] = { NULL, &target };
    invoke(constructor, delegate, params);
    if (invokeError) {
        loaderLog("Rueckruf %d konnte nicht erstellt werden", call->kind);
        return NULL;
    }
    call->handle = keepAlive(delegate);
    return delegate;
}
