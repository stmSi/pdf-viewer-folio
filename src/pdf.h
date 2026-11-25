#ifndef FOLIO_PDF_H
#define FOLIO_PDF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct Pdf Pdf;

typedef struct {
    float x0, y0, x1, y1;
} PdfBox;

typedef struct {
    int x0, y0, x1, y1;
} PixelBox;

typedef struct {
    unsigned char *pixels;
    int width, height;
} PdfImage;

typedef struct {
    char title[256];
    int page;
    float x, y;
    size_t parent, after;
    unsigned int depth;
    bool expanded;
} PdfContentsEntry;

typedef struct {
    PdfContentsEntry *entries;
    size_t count;
} PdfContents;

Pdf *pdf_open(const char *path, char *error, int error_size);
void pdf_close(Pdf *pdf);
bool pdf_locked(const Pdf *pdf);
bool pdf_unlock(Pdf *pdf, const char *password, char *error, int error_size);
int pdf_page_count(const Pdf *pdf);
uint64_t pdf_key(const Pdf *pdf);
bool pdf_set_page(Pdf *pdf, int index, char *error, int error_size);
bool pdf_measure_page(Pdf *pdf, int index, PdfBox *bounds);
PdfBox pdf_content_bounds(Pdf *pdf);
bool pdf_render(Pdf *pdf, float scale, PixelBox pixels, PdfImage *image,
                char *error, int error_size);
void pdf_image_free(PdfImage *image);
bool pdf_load_contents(Pdf *pdf, PdfContents *contents, char *error, int error_size);
void pdf_free_contents(PdfContents *contents);

#endif
