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

typedef enum { TEXT_GENERAL, TEXT_PAGE_NUMBER, TEXT_PASSWORD } TextInput;
typedef void (*StateKeyCallback)(const char *key, void *data);

void platform_init(int argc, char **argv);
void platform_shutdown(void);
void platform_choose_file(void);
bool platform_reopen_file(const char *locator);
void platform_file_locator(const FileEvent *event, char *locator, size_t size);
bool platform_poll_file(FileEvent *event);
const char *platform_pick_hint(void);
bool platform_request_text(const char *label, const char *initial, TextInput input);
bool platform_poll_text(TextEvent *event);
bool platform_read(const char *key, char *value, size_t size);
bool platform_write(const char *key, const char *value);
bool platform_delete(const char *key);
void platform_list(const char *prefix, StateKeyCallback callback, void *data);
void platform_remove_temp(const FileEvent *event);

#endif
