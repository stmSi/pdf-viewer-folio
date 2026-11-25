#include "pdf_worker.h"
#include "thread.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { QUEUE_LIMIT = 48, SEARCH_QUAD_LIMIT = 20000, SEARCH_MATCH_LIMIT = 10000 };
struct PdfWorker {
    FolioMutex mutex;
    FolioCondition condition;
    FolioThread thread;
    Pdf *pdf;
    PdfBox *bounds;
    PdfContents contents;
    char *path;
    WorkerStatus status;
    int measured, total;
    char error[256], password[256];
    bool password_ready, stopping, threaded, prepared;
    PdfRequest queue[QUEUE_LIMIT];
    size_t queued;
    PdfResult *first, *last;
    char query[256];
    unsigned search_generation;
    PdfSearch search;
};
static atomic_int active_workers;

int worker_active_count(void) { return atomic_load(&active_workers); }

void worker_result_free(PdfResult *result)
{
    if (result == NULL) return;
    pdf_image_free(&result->image);
    pdf_text_free(result->text);
    free(result);
}

void worker_search_free(PdfSearch *search)
{
    free(search->hits);
    *search = (PdfSearch){0};
}

static bool stopped(PdfWorker *w)
{
    mutex_lock(&w->mutex);
    bool stop = w->stopping;
    mutex_unlock(&w->mutex);
    return stop;
}

static void destroy(PdfWorker *w)
{
    pdf_close(w->pdf);
    while (w->first != NULL) {
        PdfResult *next = w->first->next;
        worker_result_free(w->first);
        w->first = next;
    }
    free(w->bounds);
    pdf_free_contents(&w->contents);
    worker_search_free(&w->search);
    free(w->path);
    condition_destroy(&w->condition);
    mutex_destroy(&w->mutex);
    free(w);
    atomic_fetch_sub(&active_workers, 1);
}

static void publish_error(PdfWorker *w, const char *error)
{
    mutex_lock(&w->mutex);
    snprintf(w->error, sizeof w->error, "%s", error);
    w->status = WORKER_FAILED;
    mutex_unlock(&w->mutex);
}

static void open_document(PdfWorker *w)
{
    char error[256] = {0};
    Pdf *pdf = pdf_open(w->path, error, sizeof error);
    mutex_lock(&w->mutex);
    w->pdf = pdf;
    if (pdf == NULL) {
        snprintf(w->error, sizeof w->error, "%s", error);
        w->status = WORKER_FAILED;
    } else if (pdf_locked(pdf)) w->status = WORKER_PASSWORD;
    mutex_unlock(&w->mutex);
}

static bool prepare_document(PdfWorker *w)
{
    int count = pdf_page_count(w->pdf);
    PdfBox *bounds = calloc((size_t)count, sizeof *bounds);
    if (bounds == NULL) { publish_error(w, "Out of memory measuring PDF pages."); return false; }
    mutex_lock(&w->mutex);
    w->total = count;
    mutex_unlock(&w->mutex);
    for (int i = 0; i < count; i++) {
        if (stopped(w)) { free(bounds); return false; }
        if (!pdf_measure_page(w->pdf, i, &bounds[i])) bounds[i] = (PdfBox){0, 0, 612, 792};
        mutex_lock(&w->mutex);
        w->measured = i + 1;
        mutex_unlock(&w->mutex);
    }
    PdfContents contents = {0};
    pdf_load_contents(w->pdf, &contents, NULL, 0);
    mutex_lock(&w->mutex);
    w->bounds = bounds;
    w->contents = contents;
    w->prepared = true;
    w->status = WORKER_READY;
    mutex_unlock(&w->mutex);
    return true;
}

static void process_request(PdfWorker *w, PdfRequest request)
{
    PdfResult *result = calloc(1, sizeof *result);
    if (result == NULL) return;
    result->request = request;
    if (request.kind == WORK_TEXT) {
        pdf_extract_text(w->pdf, request.page, &result->text, result->error, sizeof result->error);
    } else if (pdf_set_page(w->pdf, request.page, result->error, sizeof result->error)) {
        pdf_render(w->pdf, request.scale, request.pixels, &result->image, result->error, sizeof result->error);
        if (request.crop && !stopped(w)) {
            result->crop = pdf_content_bounds(w->pdf);
            result->cropped = true;
        }
    }
    mutex_lock(&w->mutex);
    if (w->stopping) { mutex_unlock(&w->mutex); worker_result_free(result); return; }
    if (w->last != NULL) w->last->next = result;
    else w->first = result;
    w->last = result;
    mutex_unlock(&w->mutex);
}

typedef struct { PdfWorker *worker; unsigned generation; int page; } SearchContext;
static bool append_match(const PdfBox *boxes, size_t count, void *opaque)
{
    SearchContext *context = opaque;
    PdfWorker *w = context->worker;
    mutex_lock(&w->mutex);
    if (w->stopping || w->search_generation != context->generation) {
        mutex_unlock(&w->mutex);
        return false;
    }
    PdfSearch *search = &w->search;
    if (count > SEARCH_QUAD_LIMIT - search->count || search->matches >= SEARCH_MATCH_LIMIT) {
        search->limited = search->complete = true;
        search->version++;
        mutex_unlock(&w->mutex);
        return false;
    }
    PdfMatch *hits = realloc(search->hits, (search->count + count) * sizeof *hits);
    if (hits == NULL && count > 0) {
        search->errors++;
        search->complete = true;
        search->version++;
        mutex_unlock(&w->mutex);
        return false;
    }
    search->hits = hits;
    for (size_t i = 0; i < count; i++)
        search->hits[search->count++] = (PdfMatch){context->page, search->matches, boxes[i]};
    search->matches++;
    search->version++;
    mutex_unlock(&w->mutex);
    return true;
}

static void process_search_page(PdfWorker *w, int page, unsigned generation, const char *query)
{
    SearchContext context = {w, generation, page};
    char error[256] = {0};
    int result = pdf_search_page(w->pdf, page, query, append_match, &context, error, sizeof error);
    mutex_lock(&w->mutex);
    if (generation == w->search_generation) {
        if (result < 0) w->search.errors++;
        w->search.scanned = page + 1;
        w->search.total = w->total;
        if (page + 1 >= w->total) w->search.complete = true;
        w->search.version++;
    }
    mutex_unlock(&w->mutex);
}

static bool step(PdfWorker *w, bool wait)
{
    mutex_lock(&w->mutex);
    while (!w->stopping && !w->password_ready && w->queued == 0 &&
           (w->status != WORKER_READY || w->query[0] == '\0' || w->search.complete)) {
        if (!wait) { mutex_unlock(&w->mutex); return false; }
        condition_wait(&w->condition, &w->mutex);
    }
    if (w->stopping) { mutex_unlock(&w->mutex); return false; }
    if (w->password_ready) {
        char password[256];
        snprintf(password, sizeof password, "%s", w->password);
        memset(w->password, 0, sizeof w->password);
        w->password_ready = false;
        mutex_unlock(&w->mutex);
        char error[256] = {0};
        bool unlocked = pdf_unlock(w->pdf, password, error, sizeof error);
        memset(password, 0, sizeof password);
        if (unlocked) prepare_document(w);
        else {
            mutex_lock(&w->mutex);
            w->status = WORKER_PASSWORD;
            snprintf(w->error, sizeof w->error, "%s", error);
            mutex_unlock(&w->mutex);
        }
        return true;
    }
    if (w->queued > 0) {
        size_t selected = 0;
        // Interactive page text, then previews, detail tiles, and thumbnails.
        int priority[] = {1, 2, 3, 0};
        for (size_t i = 1; i < w->queued; i++)
            if (priority[w->queue[i].kind] < priority[w->queue[selected].kind]) selected = i;
        PdfRequest request = w->queue[selected];
        memmove(&w->queue[selected], &w->queue[selected + 1], (w->queued - selected - 1) * sizeof request);
        w->queued--;
        pdf_reset_interrupt(w->pdf);
        mutex_unlock(&w->mutex);
        process_request(w, request);
        return true;
    }
    int page = w->search.scanned;
    unsigned generation = w->search_generation;
    char query[256];
    snprintf(query, sizeof query, "%s", w->query);
    pdf_reset_interrupt(w->pdf);
    mutex_unlock(&w->mutex);
    process_search_page(w, page, generation, query);
    return true;
}

static THREAD_RETURN worker_main(void *opaque)
{
    PdfWorker *w = opaque;
    open_document(w);
    if (w->pdf != NULL && !pdf_locked(w->pdf) && !stopped(w)) prepare_document(w);
    while (!stopped(w)) step(w, true);
    destroy(w);
    return THREAD_DONE;
}

PdfWorker *worker_open(const char *path)
{
    PdfWorker *w = calloc(1, sizeof *w);
    if (w == NULL) return NULL;
    w->path = malloc(strlen(path) + 1);
    if (w->path == NULL) { free(w); return NULL; }
    strcpy(w->path, path);
    mutex_init(&w->mutex);
    condition_init(&w->condition);
    atomic_fetch_add(&active_workers, 1);
#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
    w->threaded = false;
#else
    w->threaded = true;
    if (thread_start(&w->thread, worker_main, w) != 0) { destroy(w); return NULL; }
    thread_detach(w->thread);
#endif
    return w;
}

void worker_tick(PdfWorker *w)
{
    if (w == NULL || w->threaded) return;
    if (w->pdf == NULL && w->status == WORKER_OPENING) open_document(w);
    if (w->pdf != NULL && !pdf_locked(w->pdf) && !w->prepared) prepare_document(w);
    step(w, false);
}

void worker_close(PdfWorker *w)
{
    if (w == NULL) return;
    mutex_lock(&w->mutex);
    bool threaded = w->threaded;
    w->stopping = true;
    if (w->pdf != NULL) pdf_interrupt(w->pdf);
    condition_signal(&w->condition);
    mutex_unlock(&w->mutex);
    if (!threaded) destroy(w);
}

WorkerStatus worker_status(PdfWorker *w, int *done, int *total, char *error, size_t size)
{
    mutex_lock(&w->mutex);
    WorkerStatus status = w->status;
    if (done != NULL) *done = w->measured;
    if (total != NULL) *total = w->total;
    if (error != NULL && size > 0) snprintf(error, size, "%s", w->error);
    mutex_unlock(&w->mutex);
    return status;
}

void worker_password(PdfWorker *w, const char *password)
{
    mutex_lock(&w->mutex);
    snprintf(w->password, sizeof w->password, "%s", password);
    w->password_ready = true;
    w->status = WORKER_OPENING;
    condition_signal(&w->condition);
    mutex_unlock(&w->mutex);
}

Pdf *worker_document(PdfWorker *w) { return w->pdf; } // Only after WORKER_READY.
bool worker_bounds(PdfWorker *w, int page, PdfBox *bounds)
{
    if (!w->prepared || page < 0 || page >= w->total) return false;
    *bounds = w->bounds[page];
    return true;
}

bool worker_contents(PdfWorker *w, PdfContents *contents)
{
    *contents = (PdfContents){0};
    if (w->contents.count == 0) return true;
    contents->entries = malloc(w->contents.count * sizeof *contents->entries);
    if (contents->entries == NULL) return false;
    contents->count = w->contents.count;
    memcpy(contents->entries, w->contents.entries, contents->count * sizeof *contents->entries);
    return true;
}

bool worker_request(PdfWorker *w, PdfRequest request)
{
    if (request.page < 0 || request.page >= w->total) return false;
    mutex_lock(&w->mutex);
    if (w->stopping || w->queued == QUEUE_LIMIT) { mutex_unlock(&w->mutex); return false; }
    for (size_t i = 0; i < w->queued; i++) {
        if (w->queue[i].page == request.page && w->queue[i].kind == request.kind) {
            w->queue[i] = request;
            mutex_unlock(&w->mutex);
            return true;
        }
    }
    w->queue[w->queued++] = request;
    condition_signal(&w->condition);
    mutex_unlock(&w->mutex);
    return true;
}

PdfResult *worker_poll(PdfWorker *w)
{
    mutex_lock(&w->mutex);
    PdfResult *result = w->first;
    if (result != NULL) {
        w->first = result->next;
        if (w->first == NULL) w->last = NULL;
        result->next = NULL;
    }
    mutex_unlock(&w->mutex);
    return result;
}

void worker_search(PdfWorker *w, const char *query)
{
    mutex_lock(&w->mutex);
    snprintf(w->query, sizeof w->query, "%s", query);
    w->search_generation++;
    unsigned version = w->search.version + 1;
    worker_search_free(&w->search);
    w->search.version = version;
    w->search.total = w->total;
    w->search.complete = *query == '\0';
    condition_signal(&w->condition);
    mutex_unlock(&w->mutex);
}

bool worker_search_snapshot(PdfWorker *w, unsigned version, PdfSearch *search)
{
    mutex_lock(&w->mutex);
    if (version == w->search.version) { mutex_unlock(&w->mutex); return false; }
    PdfMatch *hits = NULL;
    if (w->search.count > 0) {
        hits = malloc(w->search.count * sizeof *hits);
        if (hits == NULL) { mutex_unlock(&w->mutex); return false; }
        memcpy(hits, w->search.hits, w->search.count * sizeof *hits);
    }
    *search = w->search;
    search->hits = hits;
    mutex_unlock(&w->mutex);
    return true;
}
