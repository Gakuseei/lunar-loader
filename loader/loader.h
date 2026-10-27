#pragma once

#include <windows.h>
#include <stdint.h>

#include "modapi.h"

#define MAX_MODS 32
#define MAX_SETTINGS 16
#define MAX_OPTIONS 12

typedef void Il2CppObject;
typedef void Il2CppClass;
typedef void Il2CppString;
typedef void MethodInfo;
typedef void FieldInfo;
/** Unity 6 GC handles are pointer sized; storing them in 32 bits corrupts them */
typedef void *GcHandle;

typedef enum {
    CALL_ACTION,
    CALL_ACTION_OBJECT,
    CALL_FUNC_BOOL,
    CALL_ACTION_BOOL,
    CALL_FUNC_INT,
    CALL_ACTION_INT,
    CALL_KIND_COUNT,
} CallKind;

/** A managed delegate whose target is native code; the fake method info must stay first, the thunk gets its address */
typedef struct NativeCall {
    unsigned char method[0x80];
    CallKind kind;
    void *context;
    void (*action)(void *context);
    void (*actionObject)(void *context, Il2CppObject *argument);
    int (*getValue)(void *context);
    void (*setValue)(void *context, int value);
    GcHandle handle;
} NativeCall;

typedef enum { SETTING_TOGGLE, SETTING_CHOICE, SETTING_NOTE } SettingKind;

typedef struct Setting {
    Mod *mod;
    SettingKind kind;
    char key[48];
    char labelDe[96], labelEn[96];
    int flags;
    int value;
    char options[MAX_OPTIONS][24];
    int optionCount;
    void (*changed)(Mod *mod);
    NativeCall *get, *set, *editable;
} Setting;

struct Mod {
    char name[64];
    wchar_t ini[MAX_PATH];
    HANDLE log;
    char nameDe[96], nameEn[96], infoDe[256], infoEn[256];
    char incompatible[256];
    int loaded;
    int enabled;
    void (*onEnabled)(Mod *mod, int enabled);
    Setting settings[MAX_SETTINGS];
    int settingCount;
    NativeCall *listGet, *listSet, *listEditable, *open;
};

extern Mod mods[MAX_MODS];
extern int modCount;
extern HMODULE gameAssembly;
extern unsigned char *gameAssemblyStart, *gameAssemblyEnd;

void loaderLog(const char *format, ...);
void modLog(Mod *mod, const char *format, ...);
void setModEnabled(Mod *mod, int enabled);
void saveSetting(Setting *setting);
int isHost(void);
int germanText(void);

int il2cppLoad(HMODULE module);
void *il2cppExport(const char *name);
Il2CppClass *findClass(const char *assembly, const char *namespaze, const char *name);
MethodInfo *classMethod(Il2CppClass *klass, const char *name, int argc);
MethodInfo *findMethod(const char *assembly, const char *namespaze, const char *klass, const char *name, int argc);
void *methodCode(const MethodInfo *method);
Il2CppClass *paramClass(const MethodInfo *method, int index);
Il2CppClass *elementClass(Il2CppClass *arrayClass);
Il2CppClass *classOf(Il2CppObject *object);
int fieldOffset(Il2CppClass *klass, const char *name);
int arrayElementSize(Il2CppClass *arrayClass);
extern int invokeError;
Il2CppString *newString(const char *utf8);
Il2CppObject *newArray(Il2CppClass *element, int length);
void setArrayItem(Il2CppObject *array, int index, Il2CppObject *item);
Il2CppObject *invoke(const MethodInfo *method, void *instance, void **params);
GcHandle keepAlive(Il2CppObject *object);
Il2CppObject *aliveTarget(GcHandle handle);
void release(GcHandle handle);
int unityAlive(Il2CppObject *object);
int stringToUtf8(Il2CppString *string, char *buffer, int size);
NativeCall *newCall(CallKind kind, void *context);
Il2CppObject *callDelegate(NativeCall *call, Il2CppClass *delegateClass);

unsigned char *findPattern(const char *pattern, int *matches);
int writeCode(void *target, const void *bytes, size_t length);
int hookFunction(void *target, void *detour, void **original);
int setHookEnabled(void *target, int enabled);
int hooksStart(void);

int menuInstall(void);

int registerUpgrade(Mod *mod, const char *id, const char *titleDe, const char *titleEn, const char *summaryDe,
    const char *summaryEn, int glyph, int maxLevel, const int *prices);
int upgradeLevel(void *world, const char *id);
void upgradesInstall(const wchar_t *modsDir);
