#ifndef FOLIO_READING_STATE_H
#define FOLIO_READING_STATE_H

#include "platform.h"
#include <stdint.h>

typedef struct {
    int page;
    double x, y;
} ReadingPosition;

typedef struct {
    ReadingPosition location;
    float zoom;
    bool fit_width;
    int version;
    int layout, rotation;
} SavedPosition;

typedef struct {
    SavedPosition position;
    char title[FOLIO_NAME_MAX];
} Bookmark;

typedef struct {
    uint64_t document;
    uint64_t accessed;
    char title[FOLIO_NAME_MAX];
    char locator[FOLIO_PATH_MAX];
    int page;
} RecentSession;

bool reading_load_position(uint64_t document, SavedPosition *position);
bool reading_save_position(uint64_t document, SavedPosition position);

bool reading_save_bookmark(uint64_t document, SavedPosition position, const char *title);
bool reading_delete_bookmark(uint64_t document, int page);
size_t reading_load_bookmarks(uint64_t document, Bookmark **bookmarks);

bool reading_record_session(uint64_t document, const char *title, const char *locator);
bool reading_forget_session(uint64_t document);
size_t reading_load_sessions(RecentSession *sessions, size_t limit);

#endif
