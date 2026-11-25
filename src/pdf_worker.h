#ifndef FOLIO_PDF_WORKER_H
#define FOLIO_PDF_WORKER_H
#include "pdf.h"

typedef struct PdfWorker PdfWorker;
typedef enum { WORKER_OPENING, WORKER_PASSWORD, WORKER_READY, WORKER_FAILED } WorkerStatus;
typedef enum { WORK_PREVIEW, WORK_DETAIL, WORK_THUMBNAIL, WORK_TEXT } WorkKind;
typedef struct {
    WorkKind kind;
    int page;
    float scale;
    PixelBox pixels;
    bool crop;
} PdfRequest;
typedef struct PdfResult {
    PdfRequest request;
    PdfImage image;
    PdfText *text;
    PdfBox crop;
    bool cropped;
    char error[256];
    struct PdfResult *next;
} PdfResult;
typedef struct {
    PdfMatch *hits;
    size_t count;
    int matches, scanned, total, errors;
    bool complete, limited;
    unsigned version;
} PdfSearch;

PdfWorker *worker_open(const char *path);
void worker_close(PdfWorker *worker);
WorkerStatus worker_status(PdfWorker *worker, int *done, int *total, char *error, size_t size);
void worker_password(PdfWorker *worker, const char *password);
Pdf *worker_document(PdfWorker *worker);
bool worker_bounds(PdfWorker *worker, int page, PdfBox *bounds);
bool worker_contents(PdfWorker *worker, PdfContents *contents);
bool worker_request(PdfWorker *worker, PdfRequest request);
PdfResult *worker_poll(PdfWorker *worker);
void worker_result_free(PdfResult *result);
void worker_search(PdfWorker *worker, const char *query);
bool worker_search_snapshot(PdfWorker *worker, unsigned version, PdfSearch *search);
void worker_search_free(PdfSearch *search);
int worker_active_count(void);
// Cooperative fallback for Web builds without shared-memory threads.
void worker_tick(PdfWorker *worker);
#endif
