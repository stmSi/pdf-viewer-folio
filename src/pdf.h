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

typedef struct {
    PdfBox box;
    int codepoint, line;
} PdfGlyph;

typedef struct {
    PdfBox box;
    char *uri;
    int page; // -1 for external links.
    float x, y;
} PdfLink;

typedef struct {
    PdfGlyph *glyphs;
    size_t count;
    PdfLink *links;
    size_t link_count;
    char *text;
} PdfText;

typedef struct {
    int page, match;
    PdfBox box;
} PdfMatch;

typedef bool (*PdfSearchCallback)(const PdfBox *boxes, size_t count, void *data);

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
bool pdf_extract_text(Pdf *pdf, int page, PdfText **text, char *error, int error_size);
void pdf_text_free(PdfText *text);
char *pdf_copy_text(const PdfText *text, size_t first, size_t last);
int pdf_search_page(Pdf *pdf, int page, const char *query, PdfSearchCallback callback,
                    void *data, char *error, int error_size);
bool pdf_can_copy(const Pdf *pdf);
bool pdf_can_print(const Pdf *pdf);
void pdf_interrupt(Pdf *pdf);
void pdf_reset_interrupt(Pdf *pdf);
typedef bool (*PdfCancelCallback)(void *data);
bool pdf_print_pages(const char *path,const char *password,const int *pages,size_t count,
                     const char *destination,PdfCancelCallback cancel,void *data,char *error,int error_size);

#endif
