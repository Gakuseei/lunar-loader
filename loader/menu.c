#include <stdio.h>
#include <string.h>

#include "loader.h"

#define TAB_DEV 5

static struct {
    MethodInfo *constructor, *build, *toggleRow, *choiceRow, *buttonRow, *textRow, *refreshPage, *gap;
    MethodInfo *setText, *childCount, *getChild, *gameObject, *destroy, *onClick, *addListener;
    Il2CppClass *buildDevType, *actionType, *unityActionType, *stringType, *funcBoolType, *actionBoolType, *funcIntType, *actionIntType;
    void (*originalConstructor)(void *self, void *kit, void *host, void *buildDev, const void *method);
    void (*originalBuild)(void *self, void *frame, const void *method);
    void (*originalUpdate)(void *self, const void *method);
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
    menu.page->confirm = 0;
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

enum { VIEW_INSTALLED, VIEW_AVAILABLE };

static int view;
static OnlineMod *onlinePage;
static NativeCall *showInstalled, *showAvailable, *onlineGet, *onlineSet, *retry, *backOnline;

static void setView(void *context)
{
    view = context ? VIEW_AVAILABLE : VIEW_INSTALLED;
    if (view == VIEW_AVAILABLE && (onlineState() == ONLINE_OFF || onlineState() == ONLINE_FAILED))
        onlineRefresh();
    rebuild();
}

static int onlineValue(void *context) { (void)context; return onlineEnabled(); }
static void onlineSwitch(void *context, int value) { (void)context; setOnlineEnabled(value); rebuild(); }
static void retryOnline(void *context) { (void)context; onlineRefresh(); rebuild(); }

static void openOnline(void *context)
{
    onlinePage = context;
    if (onlinePage)
        onlinePage->confirm = 0;
    rebuild();
}

static void pressInstall(void *context)
{
    OnlineMod *mod = context;
    if (mod->confirm)
        installMod(mod);
    mod->confirm = !mod->confirm;
    rebuild();
}

static void pressUpdate(void *context)
{
    Mod *mod = context;
    OnlineMod *online = onlineMod(mod->name);
    if (mod->confirm == 1 && online)
        installMod(online);
    mod->confirm = mod->confirm == 1 ? 0 : 1;
    rebuild();
}

static void pressRemove(void *context)
{
    Mod *mod = context;
    if (mod->confirm == 2)
        removeMod(mod->name);
    mod->confirm = mod->confirm == 2 ? 0 : 2;
    rebuild();
}

/** Enter runs Item.press and a mouse click the row's button; both open the mod page, the arrows still switch it */
static void pressOpens(void *screen, NativeCall *press, NativeCall *click)
{
    void *page = devPage(screen);
    void *items = page ? objectField(page, "items") : NULL;
    int count = items ? *(int32_t *)((unsigned char *)items + 0x18) : 0;
    void *array = count ? *(void **)((unsigned char *)items + 0x10) : NULL;
    void *item = array ? ((void **)((unsigned char *)array + 0x20))[count - 1] : NULL;
    if (!item)
        return;
    setObjectField(item, "press", callDelegate(press, menu.actionType));
    void *row = objectField(item, "row");
    void *button = row ? objectField(row, "button") : NULL;
    void *event = button && menu.onClick ? invoke(menu.onClick, button, NULL) : NULL;
    void *params[1] = { callDelegate(click, menu.unityActionType) };
    if (event && params[0])
        invoke(menu.addListener, event, params);
}

static void viewRow(void *screen, int de)
{
    if (!showInstalled) {
        showInstalled = actionCall(NULL, setView);
        showAvailable = actionCall((void *)1, setView);
    }
    const char *labels[2] = { de ? "INSTALLIERT" : "INSTALLED", de ? "VERFÜGBAR" : "AVAILABLE" };
    NativeCall *presses[2] = { showInstalled, showAvailable };
    const char *title = view == VIEW_INSTALLED ? de ? "Installiert" : "Installed" : de ? "Verfügbar" : "Available";
    buttonRow(screen, title, labels, presses, 2);
}

static void backRow(void *screen, const char *title, NativeCall *back, int de)
{
    const char *label = de ? "ZURÜCK" : "BACK";
    buttonRow(screen, title, &label, &back, 1);
}

static void buildInstalled(void *screen, int de)
{
    if (!modCount) {
        textRow(screen, de ? "Keine Mods gefunden. Mods gehören in den Ordner Mods im Spielordner."
                           : "No mods found. Mods go into the Mods folder inside the game folder.");
        return;
    }
    int pending = 0;
    for (int i = 0; i < modCount; i++) {
        Mod *mod = &mods[i];
        if (!mod->listGet) {
            mod->listGet = valueCall(CALL_FUNC_BOOL, mod, modEnabled, NULL);
            mod->listSet = valueCall(CALL_ACTION_BOOL, mod, NULL, switchMod);
            mod->listEditable = valueCall(CALL_FUNC_BOOL, mod, modUsable, NULL);
            mod->open = actionCall(mod, openMod);
            mod->click = actionCall(mod, openMod);
        }
        char name[128];
        snprintf(name, sizeof name, "%s  ›", de ? mod->nameDe : mod->nameEn);
        toggleRow(screen, name, mod->listGet, mod->listSet, mod->listEditable);
        pressOpens(screen, mod->open, mod->click);
        InstallState state = installState(mod->name);
        pending |= state == INSTALL_PENDING || state == INSTALL_REMOVING;
    }
    for (int i = 0; i < modCount; i++) {
        if (modUsable(&mods[i]))
            continue;
        char line[256];
        snprintf(line, sizeof line, de ? "%s passt nicht zu dieser Spielversion." : "%s is not compatible with this game version.",
            de ? mods[i].nameDe : mods[i].nameEn);
        textRow(screen, line);
    }
    if (pending)
        textRow(screen, de ? "Starte das Spiel neu, damit die Änderungen wirken." : "Restart the game to apply the changes.");
}

static void statusRow(void *screen, Mod *mod, int de)
{
    OnlineMod *online = onlineMod(mod->name);
    char line[256] = "";
    switch (installState(mod->name)) {
    case INSTALL_PENDING:
        snprintf(line, sizeof line, de ? "Update geladen, aktiv nach dem Neustart." : "Update downloaded, active after a restart.");
        break;
    case INSTALL_REMOVING:
        snprintf(line, sizeof line, de ? "Wird beim nächsten Start entfernt." : "Will be removed on the next start.");
        break;
    case INSTALL_OUTDATED:
        snprintf(line, sizeof line, de ? "Update auf Version %s verfügbar." : "Update to version %s available.", online->version);
        break;
    default:
        if (online)
            snprintf(line, sizeof line, de ? "Version %s, aktuell." : "Version %s, up to date.", online->version);
    }
    if (online && online->status == DOWNLOAD_RUNNING)
        snprintf(line, sizeof line, de ? "Lädt..." : "Downloading...");
    if (online && online->status == DOWNLOAD_FAILED)
        snprintf(line, sizeof line, de ? "Download fehlgeschlagen oder Datei verändert. Nichts wurde installiert."
                                       : "Download failed or the file was changed. Nothing was installed.");
    if (mod->incompatible[0])
        snprintf(line, sizeof line, de ? "Passt nicht zu dieser Spielversion: %s" : "Not compatible with this game version: %s",
            mod->incompatible);
    if (line[0])
        textRow(screen, line);
}

static void buildModPage(void *screen, Mod *mod, int de)
{
    int greyed = 0;
    if (!menu.back)
        menu.back = actionCall(NULL, closeMod);
    backRow(screen, de ? mod->nameDe : mod->nameEn, menu.back, de);
    const char *info = de ? mod->infoDe : mod->infoEn;
    if (info[0])
        textRow(screen, info);
    statusRow(screen, mod, de);
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

    InstallState state = installState(mod->name);
    if (state == INSTALL_PENDING || state == INSTALL_REMOVING)
        return;
    if (!mod->update) {
        mod->update = actionCall(mod, pressUpdate);
        mod->remove = actionCall(mod, pressRemove);
    }
    const char *labels[2];
    NativeCall *presses[2];
    int count = 0;
    if (state == INSTALL_OUTDATED) {
        labels[count] = mod->confirm == 1 ? de ? "WIRKLICH UPDATEN?" : "REALLY UPDATE?" : "UPDATE";
        presses[count++] = mod->update;
    }
    labels[count] = mod->confirm == 2 ? de ? "WIRKLICH ENTFERNEN?" : "REALLY REMOVE?" : de ? "DEINSTALLIEREN" : "UNINSTALL";
    presses[count++] = mod->remove;
    if (mod->settingCount)
        gap(screen);
    buttonRow(screen, "", labels, presses, count);
}

static const char *onlineLabel(OnlineMod *mod, int de)
{
    if (mod->status == DOWNLOAD_RUNNING)
        return de ? "LÄDT" : "LOADING";
    switch (installState(mod->folder)) {
    case INSTALL_CURRENT:
        return de ? "INSTALLIERT" : "INSTALLED";
    case INSTALL_OUTDATED:
        return "UPDATE";
    case INSTALL_PENDING:
    case INSTALL_REMOVING:
        return de ? "NEUSTART" : "RESTART";
    default:
        return de ? "ANSEHEN" : "VIEW";
    }
}

static void onlineRows(void *screen, int community, int de)
{
    for (int i = 0; i < onlineCount; i++) {
        OnlineMod *mod = &onlineMods[i];
        if (mod->community != community)
            continue;
        if (!mod->open) {
            mod->open = actionCall(mod, openOnline);
            mod->install = actionCall(mod, pressInstall);
        }
        const char *label = onlineLabel(mod, de);
        buttonRow(screen, mod->name, &label, &mod->open, 1);
    }
}

static void buildAvailable(void *screen, int de)
{
    if (!onlineGet) {
        onlineGet = valueCall(CALL_FUNC_BOOL, NULL, onlineValue, NULL);
        onlineSet = valueCall(CALL_ACTION_BOOL, NULL, NULL, onlineSwitch);
        retry = actionCall(NULL, retryOnline);
    }
    toggleRow(screen, de ? "Online-Liste" : "Online list", onlineGet, onlineSet, NULL);
    switch (onlineState()) {
    case ONLINE_OFF:
        textRow(screen, de ? "Lädt die Liste der Mods von GitHub. Solange der Schalter aus ist, geht der Loader nicht ins Internet."
                           : "Loads the list of mods from GitHub. While this is off, the loader never goes online.");
        return;
    case ONLINE_LOADING:
        textRow(screen, de ? "Lade Liste..." : "Loading list...");
        return;
    case ONLINE_FAILED: {
        const char *label = de ? "NOCHMAL" : "RETRY";
        buttonRow(screen, de ? "Liste nicht erreichbar." : "List not reachable.", &label, &retry, 1);
        return;
    }
    case ONLINE_READY:
        break;
    }
    gap(screen);
    onlineRows(screen, 0, de);
    int community = 0;
    for (int i = 0; i < onlineCount; i++)
        community |= onlineMods[i].community;
    if (!community)
        return;
    gap(screen);
    textRow(screen, de ? "Community: Mods anderer Modder. Wir prüfen sie vor der Aufnahme, geschrieben haben sie andere."
                       : "Community: mods by other modders. We check them before listing them, but we did not write them.");
    onlineRows(screen, 1, de);
}

static void buildOnlinePage(void *screen, OnlineMod *mod, int de)
{
    if (!backOnline)
        backOnline = actionCall(NULL, openOnline);
    backRow(screen, mod->name, backOnline, de);
    textRow(screen, de && mod->de[0] ? mod->de : mod->en);
    char line[160];
    snprintf(line, sizeof line, de ? "Version %s von %s%s" : "Version %s by %s%s", mod->version, mod->author,
        mod->community ? de ? ", Community" : ", community" : "");
    textRow(screen, line);
    gap(screen);
    InstallState state = installState(mod->folder);
    if (mod->status == DOWNLOAD_RUNNING) {
        textRow(screen, de ? "Lädt..." : "Downloading...");
    } else if (state == INSTALL_PENDING || mod->status == DOWNLOAD_DONE) {
        textRow(screen, de ? "Geladen. Starte das Spiel neu, dann ist die Mod aktiv." : "Downloaded. Restart the game to use it.");
    } else if (state == INSTALL_CURRENT) {
        textRow(screen, de ? "Installiert und aktuell." : "Installed and up to date.");
    } else if (state != INSTALL_REMOVING) {
        if (mod->status == DOWNLOAD_FAILED)
            textRow(screen, de ? "Download fehlgeschlagen oder Datei verändert. Nichts wurde installiert."
                               : "Download failed or the file was changed. Nothing was installed.");
        const char *label = mod->confirm ? de ? "WIRKLICH INSTALLIEREN?" : "REALLY INSTALL?"
                          : state == INSTALL_OUTDATED ? "UPDATE" : de ? "INSTALLIEREN" : "INSTALL";
        buttonRow(screen, "", &label, &mod->install, 1);
    }
}

static void buildRows(void *screen)
{
    int de = germanText();
    if (menu.page) {
        buildModPage(screen, menu.page, de);
    } else if (onlinePage && view == VIEW_AVAILABLE) {
        buildOnlinePage(screen, onlinePage, de);
    } else {
        viewRow(screen, de);
        if (view == VIEW_INSTALLED)
            buildInstalled(screen, de);
        else
            buildAvailable(screen, de);
    }
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

static void updateHook(void *self, const void *method)
{
    menu.originalUpdate(self, method);
    if (onlineChanged() && aliveTarget(menu.screen) == self)
        rebuild();
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
    menu.onClick = findMethod("UnityEngine.UI", "UnityEngine.UI", "Button", "get_onClick", 0);
    menu.unityActionType = findClass(core, "UnityEngine.Events", "UnityAction");
    menu.addListener = classMethod(findClass(core, "UnityEngine.Events", "UnityEvent"), "AddListener", 1);

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
    void *update = methodCode(findMethod(game, "MoonCleaners", "MoonSettingsScreen", "Update", 0));
    if (!update || !hookFunction(update, (void *)updateHook, (void **)&menu.originalUpdate))
        loaderLog("Online-Liste aktualisiert sich erst beim Wiederoeffnen");
    void *code = methodCode(menu.constructor);
    return code && hookFunction(code, (void *)constructorHook, (void **)&menu.originalConstructor);
}
