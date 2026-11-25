#include "reading_state.h"

#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void position_key(char *key, size_t size, uint64_t document)
{
    snprintf(key, size, "pdf-%016" PRIx64, document);
}

static void bookmark_key(char *key, size_t size, uint64_t document, int page)
{
    snprintf(key, size, "bookmark-%016" PRIx64 "-%d", document, page);
}

static void session_key(char *key, size_t size, uint64_t document)
{
    snprintf(key, size, "recent-%016" PRIx64, document);
}

static bool parse_position(const char *text, SavedPosition *position)
{
    int version, page, fit;
    float zoom;
    double x, y;
    if (sscanf(text, "%d %d %f %d %lf %lf", &version, &page, &zoom, &fit, &x, &y) != 6 ||
        (version != 1 && version != 2) || page < 0 || page == INT_MAX ||
        !isfinite(zoom) || !isfinite(x) || !isfinite(y))
        return false;

    *position = (SavedPosition){
        .location = {page, fmax(0, fmin(1, x)), fmax(-10000, fmin(1, y))},
        .zoom = fmaxf(0.15f, fminf(8.0f, zoom)),
        .fit_width = fit != 0,
        .version = version
    };
    return true;
}

static int print_position(char *text, size_t size, SavedPosition position)
{
    return snprintf(text, size, "2 %d %.6g %d %.9g %.9g\n",
                    position.location.page, position.zoom, position.fit_width,
                    position.location.x, position.location.y);
}

bool reading_load_position(uint64_t document, SavedPosition *position)
{
    char key[32], value[112];
    position_key(key, sizeof key, document);
    return platform_read(key, value, sizeof value) && parse_position(value, position);
}

bool reading_save_position(uint64_t document, SavedPosition position)
{
    char key[32], value[112];
    position_key(key, sizeof key, document);
    int length = print_position(value, sizeof value, position);
    return length > 0 && (size_t)length < sizeof value && platform_write(key, value);
}

static void format_title(char *output, const char *title)
{
    while (*title == ' ' || *title == '\t') title++;
    size_t length = 0;
    while (length < FOLIO_NAME_MAX - 1 && title[length] != '\0') {
        unsigned char byte = (unsigned char)title[length];
        output[length] = byte < 32 || byte == 127 ? ' ' : (char)byte;
        length++;
    }
    if (title[length] != '\0') {
        while (length > 0 && ((unsigned char)title[length] & 0xc0) == 0x80) length--;
    }
    while (length > 0 && output[length - 1] == ' ') length--;
    output[length] = '\0';
}

bool reading_save_bookmark(uint64_t document, SavedPosition position, const char *title)
{
    char key[48], value[112 + FOLIO_NAME_MAX];
    bookmark_key(key, sizeof key, document, position.location.page);
    int end = print_position(value, sizeof value, position);
    if (end < 0 || (size_t)end + FOLIO_NAME_MAX > sizeof value) return false;
    format_title(value + end, title);
    if (value[end] == '\0')
        snprintf(value + end, sizeof value - (size_t)end, "Page %d", position.location.page + 1);
    return platform_write(key, value);
}

bool reading_delete_bookmark(uint64_t document, int page)
{
    char key[48];
    bookmark_key(key, sizeof key, document, page);
    return platform_delete(key);
}

typedef struct {
    uint64_t document;
    Bookmark *items;
    size_t used, capacity;
} BookmarkList;

static void load_bookmark(const char *key, void *data)
{
    BookmarkList *list = data;
    char value[112 + FOLIO_NAME_MAX], expected[48];
    Bookmark bookmark;
    if (!platform_read(key, value, sizeof value) || !parse_position(value, &bookmark.position))
        return;

    const char *title = strchr(value, '\n');
    bookmark_key(expected, sizeof expected, list->document, bookmark.position.location.page);
    if (title == NULL || strcmp(key, expected) != 0) return;
    format_title(bookmark.title, title + 1);
    if (bookmark.title[0] == '\0')
        snprintf(bookmark.title, sizeof bookmark.title, "Page %d", bookmark.position.location.page + 1);

    if (list->used == list->capacity) {
        size_t capacity = list->capacity > 0 ? 2 * list->capacity : 16;
        if (capacity < list->capacity || capacity > SIZE_MAX / sizeof *list->items) return;
        Bookmark *items = realloc(list->items, capacity * sizeof *items);
        if (items == NULL) return;
        list->items = items;
        list->capacity = capacity;
    }
    list->items[list->used++] = bookmark;
}

static int compare_bookmarks(const void *a, const void *b)
{
    const Bookmark *first = a;
    const Bookmark *second = b;
    return (first->position.location.page > second->position.location.page) -
        (first->position.location.page < second->position.location.page);
}

size_t reading_load_bookmarks(uint64_t document, Bookmark **bookmarks)
{
    char prefix[40];
    snprintf(prefix, sizeof prefix, "bookmark-%016" PRIx64 "-", document);
    BookmarkList list = {.document = document};
    platform_list(prefix, load_bookmark, &list);
    if (list.used > 1) qsort(list.items, list.used, sizeof *list.items, compare_bookmarks);
    *bookmarks = list.items;
    return list.used;
}

static bool load_session(const char *key, RecentSession *session)
{
    char expected[32], value[64 + FOLIO_NAME_MAX + FOLIO_PATH_MAX];
    if (strlen(key) != 23 || sscanf(key, "recent-%16" SCNx64, &session->document) != 1)
        return false;
    session_key(expected, sizeof expected, session->document);
    if (strcmp(key, expected) != 0 || !platform_read(key, value, sizeof value)) return false;
    if (sscanf(value, "1 %" SCNu64, &session->accessed) != 1) return false;
    char *title = strchr(value, '\n');
    if (title == NULL) return false;
    title++;
    char *locator = strchr(title, '\n');
    if (locator == NULL || strlen(locator + 1) >= sizeof session->locator) return false;
    *locator++ = '\0';
    format_title(session->title, title);
    if (session->title[0] == '\0') return false;
    strcpy(session->locator, locator);
    return true;
}

bool reading_record_session(uint64_t document, const char *title, const char *locator)
{
    char key[32], name[FOLIO_NAME_MAX], value[64 + FOLIO_NAME_MAX + FOLIO_PATH_MAX];
    session_key(key, sizeof key, document);
    format_title(name, title);
    if (name[0] == '\0') snprintf(name, sizeof name, "Document.pdf");
    RecentSession existing;
    if (locator[0] == '\0' && load_session(key, &existing)) locator = existing.locator;

    struct timespec now = {0};
    timespec_get(&now, TIME_UTC);
    uint64_t accessed = (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
    int length = snprintf(value, sizeof value, "1 %" PRIu64 "\n%s\n%s", accessed, name, locator);
    return length > 0 && (size_t)length < sizeof value && platform_write(key, value);
}

bool reading_forget_session(uint64_t document)
{
    char key[32];
    session_key(key, sizeof key, document);
    return platform_delete(key);
}

typedef struct {
    RecentSession *items;
    size_t used, limit;
} SessionList;

static void load_recent_session(const char *key, void *data)
{
    SessionList *list = data;
    RecentSession session;
    if (!load_session(key, &session)) return;
    size_t index = 0;
    while (index < list->used && (list->items[index].accessed > session.accessed ||
        (list->items[index].accessed == session.accessed && list->items[index].document > session.document)))
        index++;
    if (index >= list->limit) return;

    SavedPosition position;
    session.page = reading_load_position(session.document, &position) ? position.location.page : 0;
    if (list->used < list->limit) list->used++;
    memmove(&list->items[index + 1], &list->items[index],
            (list->used - index - 1) * sizeof session);
    list->items[index] = session;
}

size_t reading_load_sessions(RecentSession *sessions, size_t limit)
{
    SessionList list = {.items = sessions, .limit = limit};
    if (limit != 0) platform_list("recent-", load_recent_session, &list);
    return list.used;
}
