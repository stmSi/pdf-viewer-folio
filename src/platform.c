#include "platform.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#elif defined(__ANDROID__)
#include <android_native_app_glue.h>
#include <pthread.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#else
#include <gio/gio.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

static FileEvent pending_file;
static TextEvent pending_text;
static bool file_ready;
static bool text_ready;

#if defined(__ANDROID__)
static pthread_mutex_t event_mutex = PTHREAD_MUTEX_INITIALIZER;
#define LOCK_EVENTS() pthread_mutex_lock(&event_mutex)
#define UNLOCK_EVENTS() pthread_mutex_unlock(&event_mutex)
#else
#define LOCK_EVENTS() ((void)0)
#define UNLOCK_EVENTS() ((void)0)
#endif

static void queue_file(const char *path, const char *name, const char *error, const char *locator, bool temporary)
{
    LOCK_EVENTS();
    if (file_ready) platform_remove_temp(&pending_file);
    pending_file = (FileEvent){.temporary = temporary};
    snprintf(pending_file.locator, sizeof pending_file.locator, "%s", locator);
    snprintf(pending_file.path, sizeof pending_file.path, "%s", path);
    snprintf(pending_file.name, sizeof pending_file.name, "%s", name);
    snprintf(pending_file.error, sizeof pending_file.error, "%s", error);
    file_ready = true;
    UNLOCK_EVENTS();
}

#if defined(__ANDROID__) || defined(__EMSCRIPTEN__)
static void queue_text(const char *text, bool accepted)
{
    LOCK_EVENTS();
    pending_text = (TextEvent){.accepted = accepted};
    snprintf(pending_text.value, sizeof pending_text.value, "%s", text);
    text_ready = true;
    UNLOCK_EVENTS();
}
#endif

bool platform_poll_text(TextEvent *event)
{
    LOCK_EVENTS();
    bool ready = text_ready;
    if (ready) *event = pending_text;
    pending_text = (TextEvent){0};
    text_ready = false;
    UNLOCK_EVENTS();
    return ready;
}

#if defined(__EMSCRIPTEN__)

EM_JS(void, web_pick, (void), {
    document.getElementById("pdf-file").click();
})

EM_JS(void, web_prompt, (const char *label, const char *initial, int input), {
    const dialog = document.getElementById("text-dialog");
    const field = document.getElementById("text-field");
    document.getElementById("text-label").textContent = UTF8ToString(label);
    field.type = input === 2 ? "password" : "text";
    field.inputMode = input === 1 ? "numeric" : "text";
    field.value = UTF8ToString(initial);
    dialog.returnValue = "";
    dialog.showModal();
    field.focus();
    field.select();
})

EM_JS(int, web_get, (const char *key, char *value, int size), {
    try {
        const saved = localStorage.getItem("folio:" + UTF8ToString(key));
        if (saved === null) return 0;
        stringToUTF8(saved, value, size);
        return 1;
    } catch (error) {
        return 0;
    }
})

EM_JS(int, web_set, (const char *key, const char *value), {
    try {
        localStorage.setItem("folio:" + UTF8ToString(key), UTF8ToString(value));
        return 1;
    } catch (error) {
        return 0;
    }
})

EM_JS(int, web_delete, (const char *key), {
    try {
        localStorage.removeItem("folio:" + UTF8ToString(key));
        return 1;
    } catch (error) {
        return 0;
    }
})

EM_JS(char *, web_keys, (const char *prefix), {
    try {
        const start = "folio:" + UTF8ToString(prefix);
        const keys = [];
        for (let i = 0; i < localStorage.length; i++) {
            const key = localStorage.key(i);
            if (key?.startsWith(start) && !key.includes("\n")) keys.push(key.slice(6));
        }
        const list = keys.join("\n");
        const size = lengthBytesUTF8(list) + 1;
        const bytes = _malloc(size);
        if (bytes) stringToUTF8(list, bytes, size);
        return bytes;
    } catch (error) {
        return 0;
    }
})

EM_JS(int, web_reopen, (const char *locator), {
    return Module['folioReopen']?.(UTF8ToString(locator)) ? 1 : 0;
})

EMSCRIPTEN_KEEPALIVE void folio_web_file(const char *path, const char *name, const char *error, const char *locator)
{
    queue_file(path, name, error, locator, path[0] != '\0');
}

EMSCRIPTEN_KEEPALIVE void folio_web_text(const char *value, int accepted)
{
    queue_text(value, accepted != 0);
}

void platform_init(int argc, char **argv) { (void)argc; (void)argv; }
void platform_shutdown(void) {}
void platform_choose_file(void) { web_pick(); }
bool platform_reopen_file(const char *locator) { return web_reopen(locator) != 0; }
const char *platform_pick_hint(void) { return "Choose a PDF from your device"; }

bool platform_request_text(const char *label, const char *initial, TextInput input)
{
    web_prompt(label, initial, input);
    return true;
}

bool platform_read(const char *key, char *value, size_t size) { return web_get(key, value, (int)size) != 0; }
bool platform_write(const char *key, const char *value) { return web_set(key, value) != 0; }
bool platform_delete(const char *key) { return web_delete(key) != 0; }

void platform_list(const char *prefix, StateKeyCallback callback, void *data)
{
    char *keys = web_keys(prefix);
    if (keys == NULL) return;
    for (char *start = keys; *start != '\0';) {
        char *end = strchr(start, '\n');
        if (end != NULL) *end = '\0';
        callback(start, data);
        if (end == NULL) break;
        start = end + 1;
    }
    free(keys);
}

#elif defined(__ANDROID__)

extern struct android_app *GetAndroidApp(void);

static const char *state_dir;

static void copy_bytes(JNIEnv *env, jbyteArray source, char *target, size_t size)
{
    jsize length = (*env)->GetArrayLength(env, source);
    if ((size_t)length >= size) length = (jsize)size - 1;
    (*env)->GetByteArrayRegion(env, source, 0, length, (jbyte *)target);
    target[length] = '\0';
}

JNIEXPORT void JNICALL Java_org_folio_viewer_ViewerActivity_deliverFile(
    JNIEnv *env, jclass cls, jbyteArray path, jbyteArray name, jbyteArray error, jbyteArray locator)
{
    (void)cls;
    char native_path[FOLIO_PATH_MAX], native_name[FOLIO_NAME_MAX], native_error[256];
    char native_locator[FOLIO_PATH_MAX];
    copy_bytes(env, path, native_path, sizeof native_path);
    copy_bytes(env, name, native_name, sizeof native_name);
    copy_bytes(env, error, native_error, sizeof native_error);
    copy_bytes(env, locator, native_locator, sizeof native_locator);
    queue_file(native_path, native_name, native_error, native_locator, native_path[0] != '\0');
}

JNIEXPORT void JNICALL Java_org_folio_viewer_ViewerActivity_deliverText(
    JNIEnv *env, jclass cls, jbyteArray value, jboolean accepted)
{
    (void)cls;
    char native_text[FOLIO_NAME_MAX];
    copy_bytes(env, value, native_text, sizeof native_text);
    queue_text(native_text, accepted == JNI_TRUE);
}

static jbyteArray java_bytes(JNIEnv *env, const char *text)
{
    jsize length = (jsize)strlen(text);
    jbyteArray bytes = (*env)->NewByteArray(env, length);
    if (bytes != NULL && length > 0)
        (*env)->SetByteArrayRegion(env, bytes, 0, length, (const jbyte *)text);
    return bytes;
}

static bool activity_call(const char *method, const char *argument, const char *initial, TextInput input)
{
    ANativeActivity *activity = GetAndroidApp()->activity;
    JNIEnv *env;
    if ((*activity->vm)->AttachCurrentThread(activity->vm, &env, NULL) != JNI_OK)
        return false;

    jclass cls = (*env)->GetObjectClass(env, activity->clazz);
    const char *signature = argument == NULL ? "()V" : initial == NULL ? "([B)V" : "([B[BI)V";
    jmethodID id = cls == NULL ? NULL : (*env)->GetMethodID(env, cls, method, signature);
    bool invoked = false;
    if (id != NULL) {
        if (argument == NULL) {
            (*env)->CallVoidMethod(env, activity->clazz, id);
            invoked = true;
        } else {
            jbyteArray first = java_bytes(env, argument);
            if (first != NULL && initial == NULL) {
                (*env)->CallVoidMethod(env, activity->clazz, id, first);
                invoked = true;
            } else if (first != NULL) {
                jbyteArray value = java_bytes(env, initial);
                if (value != NULL) {
                    (*env)->CallVoidMethod(env, activity->clazz, id, first, value, (jint)input);
                    invoked = true;
                }
            }
        }
    }
    bool ok = invoked && !(*env)->ExceptionCheck(env);
    if (!ok) (*env)->ExceptionClear(env);
    (*activity->vm)->DetachCurrentThread(activity->vm);
    return ok;
}

void platform_init(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    state_dir = GetAndroidApp()->activity->internalDataPath;
}

void platform_shutdown(void) {}

void platform_choose_file(void)
{
    if (!activity_call("choosePdf", NULL, NULL, TEXT_GENERAL))
        queue_file("", "", "Could not open the Android document picker.", "", false);
}

const char *platform_pick_hint(void) { return "Choose a PDF from Downloads or Files"; }

bool platform_request_text(const char *label, const char *initial, TextInput input)
{
    return activity_call("requestText", label, initial, input);
}

bool platform_reopen_file(const char *locator)
{
    return locator[0] != '\0' && activity_call("reopenPdf", locator, NULL, TEXT_GENERAL);
}

#elif defined(_WIN32)

static wchar_t state_dir[MAX_PATH];

static bool utf8(const wchar_t *source, char *target, int size)
{
    return WideCharToMultiByte(CP_UTF8, 0, source, -1, target, size, NULL, NULL) != 0;
}

void platform_init(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, SHGFP_TYPE_CURRENT, state_dir)) &&
        wcslen(state_dir) + 7 < MAX_PATH) {
        wcscat(state_dir, L"\\Folio");
        CreateDirectoryW(state_dir, NULL);
    } else {
        state_dir[0] = L'\0';
    }

    int count = 0;
    wchar_t **arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (arguments != NULL) {
        if (count > 1) {
            char path[FOLIO_PATH_MAX];
            if (utf8(arguments[1], path, sizeof path)) queue_file(path, "", "", "", false);
            else queue_file("", "", "The PDF file path is too long.", "", false);
        }
        LocalFree(arguments);
    }
}

void platform_shutdown(void) {}

void platform_choose_file(void)
{
    wchar_t selected[FOLIO_PATH_MAX] = {0};
    OPENFILENAMEW dialog = {0};
    dialog.lStructSize = sizeof dialog;
    dialog.hwndOwner = GetForegroundWindow();
    dialog.lpstrFilter = L"PDF documents\0*.pdf\0All files\0*.*\0";
    dialog.lpstrFile = selected;
    dialog.nMaxFile = FOLIO_PATH_MAX;
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    if (GetOpenFileNameW(&dialog)) {
        char path[FOLIO_PATH_MAX];
        if (utf8(selected, path, sizeof path)) queue_file(path, "", "", "", false);
        else queue_file("", "", "The PDF file path is too long.", "", false);
    } else if (CommDlgExtendedError() != 0) {
        queue_file("", "", "The Windows file picker could not open.", "", false);
    } else {
        queue_file("", "", "", "", false);
    }
}

const char *platform_pick_hint(void) { return "Choose a file or drop a PDF here"; }

static bool state_path(const char *key, wchar_t *path)
{
    if (state_dir[0] == L'\0') return false;
    wchar_t suffix[64];
    if (MultiByteToWideChar(CP_UTF8, 0, key, -1, suffix, 64) == 0) return false;
    return swprintf(path, MAX_PATH, L"%ls\\%ls", state_dir, suffix) > 0;
}

bool platform_read(const char *key, char *value, size_t size)
{
    wchar_t path[MAX_PATH];
    if (!state_path(key, path)) return false;
    FILE *file = _wfopen(path, L"rb");
    if (file == NULL) return false;
    value[fread(value, 1, size - 1, file)] = '\0';
    fclose(file);
    return true;
}

bool platform_write(const char *key, const char *value)
{
    wchar_t path[MAX_PATH], temporary[MAX_PATH];
    if (!state_path(key, path) || GetTempFileNameW(state_dir, L"fol", 0, temporary) == 0) return false;
    FILE *file = _wfopen(temporary, L"wb");
    bool ok = false;
    if (file != NULL) {
        bool written = fputs(value, file) >= 0;
        if (fclose(file) == 0 && written)
            ok = MoveFileExW(temporary, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    }
    DeleteFileW(temporary);
    return ok;
}

bool platform_delete(const char *key)
{
    wchar_t path[MAX_PATH];
    return state_path(key, path) && (DeleteFileW(path) || GetLastError() == ERROR_FILE_NOT_FOUND);
}

void platform_list(const char *prefix, StateKeyCallback callback, void *data)
{
    char pattern[64];
    int length = snprintf(pattern, sizeof pattern, "%s*", prefix);
    wchar_t search[MAX_PATH];
    if (length < 0 || (size_t)length >= sizeof pattern || !state_path(pattern, search)) return;
    WIN32_FIND_DATAW entry;
    HANDLE handle = FindFirstFileW(search, &entry);
    if (handle == INVALID_HANDLE_VALUE) return;
    do {
        char key[64];
        if (!(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && utf8(entry.cFileName, key, sizeof key))
            callback(key, data);
    } while (FindNextFileW(handle, &entry));
    FindClose(handle);
}

#else

static char *state_dir;
static GDBusConnection *portal;
static guint picker_signal;
static unsigned int picker_id;

static void clear_picker(void)
{
    if (picker_signal != 0) g_dbus_connection_signal_unsubscribe(portal, picker_signal);
    picker_signal = 0;
}

static void picker_result(GDBusConnection *connection, const char *sender, const char *object,
                          const char *interface, const char *signal,
                          GVariant *parameters, void *user_data)
{
    (void)connection;
    (void)sender;
    (void)object;
    (void)interface;
    (void)signal;
    (void)user_data;
    guint32 response;
    GVariant *results;
    g_variant_get(parameters, "(u@a{sv})", &response, &results);
    clear_picker();
    if (response == 0) {
        GVariant *uris = g_variant_lookup_value(results, "uris", G_VARIANT_TYPE_STRING_ARRAY);
        if (uris != NULL && g_variant_n_children(uris) > 0) {
            const char *uri;
            g_variant_get_child(uris, 0, "&s", &uri);
            GFile *file = g_file_new_for_uri(uri);
            char *path = g_file_get_path(file);
            if (path != NULL && strlen(path) < FOLIO_PATH_MAX) queue_file(path, "", "", "", false);
            else queue_file("", "", "The selected file is unavailable as a local PDF.", "", false);
            g_free(path);
            g_object_unref(file);
        }
        if (uris != NULL) g_variant_unref(uris);
    } else if (response != 1) {
        queue_file("", "", "The desktop file picker could not open. Drop a PDF here or press Ctrl+L.", "", false);
    } else {
        queue_file("", "", "", "", false);
    }
    g_variant_unref(results);
}

void platform_init(int argc, char **argv)
{
    state_dir = g_build_filename(g_get_user_state_dir(), "folio", NULL);
    g_mkdir_with_parents(state_dir, 0700);
    if (argc > 1) {
        if (strlen(argv[1]) < FOLIO_PATH_MAX) queue_file(argv[1], "", "", "", false);
        else queue_file("", "", "The PDF file path is too long.", "", false);
    }
}

void platform_shutdown(void)
{
    clear_picker();
    g_clear_object(&portal);
    g_free(state_dir);
}

void platform_choose_file(void)
{
    if (picker_signal != 0) return;
    if (portal == NULL) portal = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    if (portal == NULL) {
        queue_file("", "", "The desktop file picker is unavailable. Drop a PDF here or press Ctrl+L.", "", false);
        return;
    }

    char *sender = g_strdup(g_dbus_connection_get_unique_name(portal) + 1);
    g_strdelimit(sender, ".", '_');
    char *token = g_strdup_printf("folio_%u", ++picker_id);
    char *path = g_strdup_printf("/org/freedesktop/portal/desktop/request/%s/%s", sender, token);
    picker_signal = g_dbus_connection_signal_subscribe(
        portal, "org.freedesktop.portal.Desktop", "org.freedesktop.portal.Request",
        "Response", path, NULL, G_DBUS_SIGNAL_FLAGS_NONE, picker_result, NULL, NULL);

    GVariantBuilder filters, patterns, options;
    g_variant_builder_init(&filters, G_VARIANT_TYPE("a(sa(us))"));
    g_variant_builder_init(&patterns, G_VARIANT_TYPE("a(us)"));
    g_variant_builder_add(&patterns, "(us)", 1u, "application/pdf");
    g_variant_builder_add(&filters, "(s@a(us))", "PDF documents", g_variant_builder_end(&patterns));
    g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&options, "{sv}", "handle_token", g_variant_new_string(token));
    g_variant_builder_add(&options, "{sv}", "filters", g_variant_builder_end(&filters));

    GVariant *result = g_dbus_connection_call_sync(
        portal, "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
        "org.freedesktop.portal.FileChooser", "OpenFile",
        g_variant_new("(ss@a{sv})", "", "Open PDF", g_variant_builder_end(&options)),
        G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NONE, 3000, NULL, NULL);
    if (result == NULL) {
        clear_picker();
        queue_file("", "", "The desktop file picker is unavailable. Drop a PDF here or press Ctrl+L.", "", false);
    } else {
        const char *returned_path;
        g_variant_get(result, "(&o)", &returned_path);
        if (strcmp(returned_path, path) != 0) {
            clear_picker();
            picker_signal = g_dbus_connection_signal_subscribe(
                portal, "org.freedesktop.portal.Desktop", "org.freedesktop.portal.Request",
                "Response", returned_path, NULL, G_DBUS_SIGNAL_FLAGS_NONE, picker_result, NULL, NULL);
        }
        g_variant_unref(result);
    }

    g_free(path);
    g_free(token);
    g_free(sender);
}

const char *platform_pick_hint(void) { return "Choose a file or drop a PDF here"; }

#endif

#if !defined(__EMSCRIPTEN__) && !defined(__ANDROID__)
bool platform_request_text(const char *label, const char *initial, TextInput input)
{
    (void)label;
    (void)initial;
    (void)input;
    return false;
}
#endif

#if defined(__ANDROID__) || (!defined(__EMSCRIPTEN__) && !defined(_WIN32))
static bool state_path(const char *key, char *path, size_t size)
{
    if (state_dir == NULL) return false;
    int count = snprintf(path, size, "%s/%s", state_dir, key);
    return count > 0 && (size_t)count < size;
}

bool platform_read(const char *key, char *value, size_t size)
{
    char path[FOLIO_PATH_MAX];
    if (!state_path(key, path, sizeof path)) return false;
    FILE *file = fopen(path, "rb");
    if (file == NULL) return false;
    value[fread(value, 1, size - 1, file)] = '\0';
    fclose(file);
    return true;
}

bool platform_write(const char *key, const char *value)
{
    char path[FOLIO_PATH_MAX], temporary[FOLIO_PATH_MAX];
    if (!state_path(key, path, sizeof path) || !state_path(".write-XXXXXX", temporary, sizeof temporary))
        return false;
    int descriptor = mkstemp(temporary);
    if (descriptor == -1) return false;
    FILE *file = fdopen(descriptor, "wb");
    bool ok = false;
    if (file == NULL) {
        close(descriptor);
    } else {
        bool written = fputs(value, file) >= 0;
        if (fclose(file) == 0 && written) ok = rename(temporary, path) == 0;
    }
    unlink(temporary);
    return ok;
}

bool platform_delete(const char *key)
{
    char path[FOLIO_PATH_MAX];
    return state_path(key, path, sizeof path) && (unlink(path) == 0 || errno == ENOENT);
}

void platform_list(const char *prefix, StateKeyCallback callback, void *data)
{
    DIR *directory = state_dir == NULL ? NULL : opendir(state_dir);
    if (directory == NULL) return;
    size_t size = strlen(prefix);
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        if (strncmp(entry->d_name, prefix, size) == 0) callback(entry->d_name, data);
    }
    closedir(directory);
}
#endif

#if !defined(__ANDROID__) && !defined(__EMSCRIPTEN__)
bool platform_reopen_file(const char *locator)
{
    if (locator[0] == '\0') return false;
#if defined(_WIN32)
    wchar_t path[FOLIO_PATH_MAX];
    if (MultiByteToWideChar(CP_UTF8, 0, locator, -1, path, FOLIO_PATH_MAX) == 0) return false;
    DWORD attributes = GetFileAttributesW(path);
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY)) return false;
#else
    if (!g_file_test(locator, G_FILE_TEST_IS_REGULAR)) return false;
#endif
    queue_file(locator, "", "", "", false);
    return true;
}
#endif

void platform_file_locator(const FileEvent *event, char *locator, size_t size)
{
    locator[0] = '\0';
    if (event->locator[0] != '\0') {
        snprintf(locator, size, "%s", event->locator);
        return;
    }
    if (event->temporary) return;
#if defined(_WIN32)
    wchar_t source[FOLIO_PATH_MAX], absolute[FOLIO_PATH_MAX];
    if (MultiByteToWideChar(CP_UTF8, 0, event->path, -1, source, FOLIO_PATH_MAX) > 0) {
        DWORD length = GetFullPathNameW(source, FOLIO_PATH_MAX, absolute, NULL);
        if (length > 0 && length < FOLIO_PATH_MAX && !utf8(absolute, locator, (int)size))
            locator[0] = '\0';
    }
#elif !defined(__ANDROID__) && !defined(__EMSCRIPTEN__)
    char *absolute = g_canonicalize_filename(event->path, NULL);
    if (strlen(absolute) < size) snprintf(locator, size, "%s", absolute);
    g_free(absolute);
#endif
}

bool platform_poll_file(FileEvent *event)
{
#if !defined(__ANDROID__) && !defined(__EMSCRIPTEN__) && !defined(_WIN32)
    while (g_main_context_iteration(NULL, FALSE)) { }
#endif
    LOCK_EVENTS();
    bool ready = file_ready;
    if (ready) *event = pending_file;
    file_ready = false;
    UNLOCK_EVENTS();
    return ready;
}

void platform_remove_temp(const FileEvent *event)
{
    if (event->temporary && event->path[0] != '\0') remove(event->path);
}
