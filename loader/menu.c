#include <stdio.h>
#include <string.h>

#include "loader.h"

#define TAB_DEV 5

static struct {
    MethodInfo *constructor, *build, *toggleRow, *choiceRow, *buttonRow, *textRow, *refreshPage, *gap;
    MethodInfo *setText, *childCount, *getChild, *gameObject, *destroy;
    Il2CppClass *buildDevType, *actionType, *stringType, *funcBoolType, *actionBoolType, *funcIntType, *actionIntType;
    void (*originalConstructor)(void *self, void *kit, void *host, void *buildDev, const void *method);
    void (*originalBuild)(void *self, void *frame, const void *method);
    NativeCall *buildDev, *back;
    GcHandle screen;
    Mod *page;
} menu;

static void *objectField(void *object, const char *name)
{
    int offset = fieldOffset(classOf(object), name);
    return offset < 0 ? NULL : *(void **)((unsigned char *)object + offset);
}

static void setIntField(void *object, const char *name, int value)
{
    int offset = fieldOffset(classOf(object), name);
    if (offset >= 0)
        *(int32_t *)((unsigned char *)object + offset) = value;
}

static void reportOnce(int *reported, const char *message)
{
    if (!*reported)
        loaderLog("%s", message);
    *reported = 1;
}

/** The settings screen keeps its pages in a Dictionary<Tab, Page>; ask the dictionary itself for the Dev tab */
static void *devPage(void *screen)
{
    static int noPages, noLookup, noPage;
    void *pages = objectField(screen, "pages");
    if (!pages) {
        reportOnce(&noPages, "Seiten-Liste der Einstellungen fehlt");
        return NULL;
    }
    MethodInfo *tryGetValue = classMethod(classOf(pages), "TryGetValue", 2);
    if (!tryGetValue) {
        reportOnce(&noLookup, "Seiten-Abfrage fehlt");
        return NULL;
    }
    int key = TAB_DEV;
    void *page = NULL;
    void *params[2] = { &key, &page };
    Il2CppObject *found = invoke(tryGetValue, pages, params);
    if (!found || !*(uint8_t *)((unsigned char *)found + 0x10) || !page) {
        reportOnce(&noPage, "Mods-Seite noch nicht angelegt");
        return NULL;
    }
    return page;
}

static void setText(void *text, const char *value)
{
    if (!unityAlive(text))
        return;
    void *params[1] = { newString(value) };
    invoke(menu.setText, text, params);
}

static void renameTab(void *screen)
{
    static int reported;
    void *page = devPage(screen);
    void *word = page ? objectField(page, "word") : NULL;
    void *text = word ? objectField(word, "text") : NULL;
    if (text)
        setText(text, "MODS");
    else
        reportOnce(&reported, "Reiter-Name nicht gefunden");
}

/** Game buttons are upper case; covers ASCII and the German umlauts */
static void upperCase(const char *text, char *out, size_t size)
{
    size_t length = 0;
    for (size_t i = 0; text[i] && length + 3 < size; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c >= 'a' && c <= 'z') {
            out[length++] = (char)(c - 32);
        } else if (c == 0xC3 && ((unsigned char)text[i + 1] == 0xA4 || (unsigned char)text[i + 1] == 0xB6
                                    || (unsigned char)text[i + 1] == 0xBC)) {
            out[length++] = (char)c;
            out[length++] = (char)((unsigned char)text[++i] - 0x20);
        } else {
            out[length++] = (char)c;
        }
    }
    out[length] = 0;
}

static void toggleRow(void *screen, const char *name, NativeCall *get, NativeCall *set, NativeCall *editable)
{
    int tab = TAB_DEV;
    uint8_t notify = 0;
    void *params[6] = {
        &tab,
        newString(name),
        callDelegate(get, menu.funcBoolType),
        callDelegate(set, menu.actionBoolType),
        editable ? callDelegate(editable, menu.funcBoolType) : NULL,
        &notify,
    };
    invoke(menu.toggleRow, screen, params);
}

static void choiceRow(void *screen, const char *name, Setting *setting, NativeCall *editable)
{
    int tab = TAB_DEV;
    uint8_t notify = 0;
    Il2CppObject *options = newArray(menu.stringType, setting->optionCount);
    for (int i = 0; i < setting->optionCount; i++)
        setArrayItem(options, i, newString(setting->options[i]));
    void *params[7] = {
        &tab,
        newString(name),
        options,
        callDelegate(setting->get, menu.funcIntType),
        callDelegate(setting->set, menu.actionIntType),
        editable ? callDelegate(editable, menu.funcBoolType) : NULL,
        &notify,
    };
    invoke(menu.choiceRow, screen, params);
}

static void buttonRow(void *screen, const char *name, const char *const *labels, NativeCall *const *presses, int count)
{
    int tab = TAB_DEV;
    Il2CppObject *texts = newArray(menu.stringType, count);
    Il2CppObject *acts = newArray(menu.actionType, count);
    for (int i = 0; i < count; i++) {
        setArrayItem(texts, i, newString(labels[i]));
        setArrayItem(acts, i, callDelegate(presses[i], menu.actionType));
    }
    void *params[4] = { &tab, newString(name), texts, acts };
    invoke(menu.buttonRow, screen, params);
}

static void textRow(void *screen, const char *text)
{
    int tab = TAB_DEV;
    void *params[1] = { &tab };
    setText(invoke(menu.textRow, screen, params), text);
}

static void gap(void *screen)
{
    void *page = devPage(screen);
    if (!menu.gap || !page)
        return;
    void *params[1] = { page };
    invoke(menu.gap, NULL, params);
}

static int modEnabled(void *context) { return ((Mod *)context)->enabled; }
static void switchMod(void *context, int value) { setModEnabled(context, value); }
static int modUsable(void *context) { return ((Mod *)context)->loaded && !((Mod *)context)->incompatible[0]; }
static int settingValue(void *context) { return ((Setting *)context)->value; }
static int worldEditable(void *context) { (void)context; return isHost(); }

static void changeSetting(void *context, int value)
{
    Setting *setting = context;
    if (setting->kind == SETTING_CHOICE && (value < 0 || value >= setting->optionCount))
        return;
    if (setting->kind == SETTING_TOGGLE)
        value = value != 0;
    if (setting->value == value)
        return;
    setting->value = value;
    saveSetting(setting);
    modLog(setting->mod, "%s = %d", setting->key, value);
    if (setting->changed)
        setting->changed(setting->mod);
}

static void rebuild(void);

static void openMod(void *context)
{
    menu.page = context;
    rebuild();
}

static void closeMod(void *context)
{
    (void)context;
    menu.page = NULL;
    rebuild();
}

static NativeCall *valueCall(CallKind kind, void *context, int (*get)(void *), void (*set)(void *, int))
{
    NativeCall *call = newCall(kind, context);
    if (call) {
        call->getValue = get;
        call->setValue = set;
    }
    return call;
}

static NativeCall *actionCall(void *context, void (*action)(void *))
{
    NativeCall *call = newCall(CALL_ACTION, context);
    if (call)
        call->action = action;
    return call;
}

#define BUTTONS_PER_ROW 3

static int hasSettings(Mod *mod)
{
    for (int i = 0; i < mod->settingCount; i++)
        if (mod->settings[i].kind != SETTING_NOTE)
            return modUsable(mod);
    return 0;
}

/** One On/Off row per mod, then the game's own button rows to open each mod's page */
static void buildList(void *screen)
{
    int de = germanText();
    if (!modCount) {
        textRow(screen, de ? "Keine Mods gefunden. Mods gehören in den Ordner Mods im Spielordner."
                           : "No mods found. Mods go into the Mods folder inside the game folder.");
        return;
    }
    for (int i = 0; i < modCount; i++) {
        Mod *mod = &mods[i];
        if (!mod->listGet) {
            mod->listGet = valueCall(CALL_FUNC_BOOL, mod, modEnabled, NULL);
            mod->listSet = valueCall(CALL_ACTION_BOOL, mod, NULL, switchMod);
            mod->listEditable = valueCall(CALL_FUNC_BOOL, mod, modUsable, NULL);
            mod->open = actionCall(mod, openMod);
        }
        toggleRow(screen, de ? mod->nameDe : mod->nameEn, mod->listGet, mod->listSet, mod->listEditable);
    }

    char names[BUTTONS_PER_ROW][96];
    const char *labels[BUTTONS_PER_ROW];
    NativeCall *presses[BUTTONS_PER_ROW];
    int count = 0, rows = 0;
    for (int i = 0; i <= modCount; i++) {
        if (i < modCount && hasSettings(&mods[i])) {
            upperCase(de ? mods[i].nameDe : mods[i].nameEn, names[count], sizeof names[count]);
            labels[count] = names[count];
            presses[count] = mods[i].open;
            count++;
        }
        if (count == BUTTONS_PER_ROW || (i == modCount && count)) {
            if (!rows)
                gap(screen);
            buttonRow(screen, rows ? "" : de ? "Einstellungen" : "Settings", labels, presses, count);
            rows++;
            count = 0;
        }
    }

    for (int i = 0; i < modCount; i++) {
        if (modUsable(&mods[i]))
            continue;
        char line[256];
        snprintf(line, sizeof line, de ? "%s passt nicht zu dieser Spielversion." : "%s is not compatible with this game version.",
            de ? mods[i].nameDe : mods[i].nameEn);
        textRow(screen, line);
    }
}

static void buildModPage(void *screen, Mod *mod)
{
    int de = germanText(), greyed = 0;
    if (!menu.back)
        menu.back = actionCall(NULL, closeMod);
    const char *back = de ? "ZURÜCK" : "BACK";
    buttonRow(screen, de ? mod->nameDe : mod->nameEn, &back, &menu.back, 1);
    const char *info = de ? mod->infoDe : mod->infoEn;
    if (info[0])
        textRow(screen, info);
    gap(screen);
    for (int i = 0; i < mod->settingCount; i++) {
        Setting *setting = &mod->settings[i];
        const char *label = de ? setting->labelDe : setting->labelEn;
        if (setting->kind == SETTING_NOTE) {
            textRow(screen, label);
            continue;
        }
        if (!setting->get) {
            CallKind get = setting->kind == SETTING_TOGGLE ? CALL_FUNC_BOOL : CALL_FUNC_INT;
            CallKind set = setting->kind == SETTING_TOGGLE ? CALL_ACTION_BOOL : CALL_ACTION_INT;
            setting->get = valueCall(get, setting, settingValue, NULL);
            setting->set = valueCall(set, setting, NULL, changeSetting);
            setting->editable = valueCall(CALL_FUNC_BOOL, setting, worldEditable, NULL);
        }
        NativeCall *editable = setting->flags & MOD_WORLD ? setting->editable : NULL;
        if (setting->kind == SETTING_TOGGLE)
            toggleRow(screen, label, setting->get, setting->set, editable);
        else
            choiceRow(screen, label, setting, editable);
        greyed |= (setting->flags & MOD_WORLD) && !isHost();
    }
    if (greyed)
        textRow(screen, de ? "Graue Einstellungen bestimmt der Host." : "Greyed out settings are set by the host.");
}

static void buildRows(void *screen)
{
    if (menu.page)
        buildModPage(screen, menu.page);
    else
        buildList(screen);
}

static void clearPage(void *screen, void *page)
{
    void *items = objectField(page, "items");
    if (items) {
        invoke(classMethod(classOf(items), "Clear", 0), items, NULL);
    }
    setIntField(page, "lines", 0);
    setIntField(screen, "cursor", 0);
    void *rect = objectField(page, "rect");
    if (!unityAlive(rect))
        return;
    Il2CppObject *boxed = invoke(menu.childCount, rect, NULL);
    int children = boxed ? *(int32_t *)((unsigned char *)boxed + 0x10) : 0;
    for (int i = children - 1; i >= 0; i--) {
        void *params[1] = { &i };
        void *child = invoke(menu.getChild, rect, params);
        void *object = unityAlive(child) ? invoke(menu.gameObject, child, NULL) : NULL;
        if (unityAlive(object)) {
            void *target[1] = { object };
            invoke(menu.destroy, NULL, target);
        }
    }
}

static void rebuild(void)
{
    void *screen = aliveTarget(menu.screen);
    void *page = screen ? devPage(screen) : NULL;
    if (!page) {
        loaderLog("Mods-Seite nicht gefunden");
        return;
    }
    clearPage(screen, page);
    buildRows(screen);
    invoke(menu.refreshPage, screen, NULL);
}

static void onBuildDev(void *context, Il2CppObject *screen)
{
    (void)context;
    release(menu.screen);
    menu.screen = keepAlive(screen);
    menu.page = NULL;
    renameTab(screen);
    buildRows(screen);
    loaderLog("Mods-Seite aufgebaut (%d Mods)", modCount);
}

static void buildHook(void *self, void *frame, const void *method)
{
    menu.originalBuild(self, frame, method);
    if (aliveTarget(menu.screen) == self)
        renameTab(self);
}

static void constructorHook(void *self, void *kit, void *host, void *buildDev, const void *method)
{
    if (!menu.buildDev) {
        menu.buildDev = newCall(CALL_ACTION_OBJECT, NULL);
        if (menu.buildDev)
            menu.buildDev->actionObject = onBuildDev;
    }
    Il2CppObject *ours = callDelegate(menu.buildDev, menu.buildDevType);
    menu.originalConstructor(self, kit, host, ours ? ours : buildDev, method);
}

int menuInstall(void)
{
    const char *game = "Assembly-CSharp", *core = "UnityEngine.CoreModule";
    menu.constructor = findMethod(game, "MoonCleaners", "MoonSettingsScreen", ".ctor", 3);
    menu.build = findMethod(game, "MoonCleaners", "MoonSettingsScreen", "Build", 1);
    menu.toggleRow = findMethod(game, "MoonCleaners", "MoonSettingsScreen", "ToggleRow", 6);
    menu.choiceRow = findMethod(game, "MoonCleaners", "MoonSettingsScreen", "ChoiceRow", 7);
    menu.buttonRow = findMethod(game, "MoonCleaners", "MoonSettingsScreen", "ButtonRow", 4);
    menu.textRow = findMethod(game, "MoonCleaners", "MoonSettingsScreen", "TextRow", 1);
    menu.refreshPage = findMethod(game, "MoonCleaners", "MoonSettingsScreen", "RefreshPage", 0);
    menu.gap = findMethod(game, "MoonCleaners", "MoonSettingsScreen", "Gap", 1);
    menu.setText = findMethod("Unity.TextMeshPro", "TMPro", "TMP_Text", "set_text", 1);
    menu.childCount = findMethod(core, "UnityEngine", "Transform", "get_childCount", 0);
    menu.getChild = findMethod(core, "UnityEngine", "Transform", "GetChild", 1);
    menu.gameObject = findMethod(core, "UnityEngine", "Component", "get_gameObject", 0);
    menu.destroy = findMethod(core, "UnityEngine", "Object", "Destroy", 1);

    menu.buildDevType = paramClass(menu.constructor, 2);
    menu.funcBoolType = paramClass(menu.toggleRow, 2);
    menu.actionBoolType = paramClass(menu.toggleRow, 3);
    menu.funcIntType = paramClass(menu.choiceRow, 3);
    menu.actionIntType = paramClass(menu.choiceRow, 4);
    menu.stringType = elementClass(paramClass(menu.buttonRow, 2));
    menu.actionType = elementClass(paramClass(menu.buttonRow, 3));

    if (!menu.toggleRow || !menu.choiceRow || !menu.buttonRow || !menu.textRow || !menu.refreshPage || !menu.setText
        || !menu.childCount || !menu.getChild || !menu.gameObject || !menu.destroy || !menu.buildDevType
        || !menu.funcBoolType || !menu.actionBoolType || !menu.funcIntType || !menu.actionIntType || !menu.stringType
        || !menu.actionType)
        return 0;
    void *build = methodCode(menu.build);
    if (!build || !hookFunction(build, (void *)buildHook, (void **)&menu.originalBuild))
        loaderLog("Reiter-Name bleibt DEV TOOLS");
    void *code = methodCode(menu.constructor);
    return code && hookFunction(code, (void *)constructorHook, (void **)&menu.originalConstructor);
}
