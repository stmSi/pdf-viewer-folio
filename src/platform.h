#ifndef FOLIO_PLATFORM_H
#define FOLIO_PLATFORM_H

#include <stdbool.h>
#include <stddef.h>

#define FOLIO_PATH_MAX 4096
#define FOLIO_NAME_MAX 256

typedef struct {
    char path[FOLIO_PATH_MAX];
    char name[FOLIO_NAME_MAX];
    char locator[FOLIO_PATH_MAX];
    char error[256];
    bool temporary;
} FileEvent;

typedef struct {
    char value[FOLIO_NAME_MAX];
    bool accepted;
} TextEvent;

typedef struct {
    float x, y; // Pointer position as a fraction of the Android surface size.
    float horizontal, vertical;
} ScrollEvent;

typedef enum { TEXT_GENERAL, TEXT_PAGE_NUMBER, TEXT_PASSWORD } TextInput;
typedef void (*StateKeyCallback)(const char *key, void *data);
typedef enum {
    PLATFORM_NONE, PLATFORM_OPEN, PLATFORM_FIND, PLATFORM_COPY, PLATFORM_SELECT_ALL,
    PLATFORM_FIND_NEXT, PLATFORM_FIND_PREVIOUS, PLATFORM_GO_PAGE, PLATFORM_ZOOM_IN,
    PLATFORM_ZOOM_OUT, PLATFORM_FIT, PLATFORM_PRINT, PLATFORM_SHARE, PLATFORM_EXPORT,
    PLATFORM_BACK, PLATFORM_FORWARD, PLATFORM_ACCESSIBILITY, PLATFORM_NEXT_PAGE, PLATFORM_PREVIOUS_PAGE,
    PLATFORM_SCROLL_DOWN, PLATFORM_SCROLL_UP
} PlatformCommand;
typedef struct { PlatformCommand kind; unsigned element; } CommandEvent;

void platform_preinit(void);
void platform_deliver_command(PlatformCommand command, unsigned element);
void platform_deliver_path(const char *path);
void platform_deliver_text(const char *text, bool accepted);
void platform_notice(const char *message);
bool platform_poll_notice(char *message,size_t size);
bool platform_poll_command(CommandEvent *event);
bool platform_copy_text(const char *text);
bool platform_open_link(const char *uri);
bool platform_document_action(PlatformCommand command, const char *path, const char *title,
                              const char *password, int pages);
void platform_accessibility(const char *json);
bool platform_frame_ready(void);

void platform_init(int argc, char **argv);
void platform_shutdown(void);
void platform_choose_file(void);
bool platform_reopen_file(const char *locator);
void platform_file_locator(const FileEvent *event, char *locator, size_t size);
bool platform_poll_file(FileEvent *event);
const char *platform_pick_hint(void);
bool platform_request_text(const char *label, const char *initial, TextInput input);
bool platform_poll_text(TextEvent *event);
bool platform_poll_scroll(ScrollEvent *event);
bool platform_read(const char *key, char *value, size_t size);
bool platform_write(const char *key, const char *value);
bool platform_delete(const char *key);
void platform_list(const char *prefix, StateKeyCallback callback, void *data);
void platform_remove_temp(const FileEvent *event);

#endif
