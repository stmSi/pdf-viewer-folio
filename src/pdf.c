#include "pdf.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

struct Pdf {
    fz_context *context;
    fz_document *document;
    fz_display_list *page;
    PdfBox bounds;
    int page_index;
    uint64_t key;
    int pages;
    bool locked;
};

static uint64_t hash_chunk(uint64_t hash, const unsigned char *bytes, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        hash ^= bytes[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static uint64_t fingerprint(fz_context *context, fz_stream *stream)
{
    unsigned char sample[4096];
    fz_seek(context, stream, 0, SEEK_END);
    uint64_t size = (uint64_t)fz_tell(context, stream);
    uint64_t hash = UINT64_C(14695981039346656037) ^ size;
    fz_seek(context, stream, 0, SEEK_SET);
    hash = hash_chunk(hash, sample, fz_read(context, stream, sample, sizeof sample));
    if (size > sizeof sample) {
        fz_seek(context, stream, -(int64_t)sizeof sample, SEEK_END);
        hash = hash_chunk(hash, sample, fz_read(context, stream, sample, sizeof sample));
    }
    fz_seek(context, stream, 0, SEEK_SET);
    return hash;
}

static void report(char *error, int error_size, const char *message)
{
    if (error != NULL && error_size > 0) snprintf(error, (size_t)error_size, "%s", message);
}

Pdf *pdf_open(const char *path, char *error, int error_size)
{
    Pdf *pdf = calloc(1, sizeof *pdf);
    if (pdf == NULL) {
        report(error, error_size, "Out of memory.");
        return NULL;
    }
    pdf->context = fz_new_context(NULL, NULL, 48 * 1024 * 1024);
    if (pdf->context == NULL) {
        free(pdf);
        report(error, error_size, "Could not initialize the PDF renderer.");
        return NULL;
    }

    fz_context *context = pdf->context;
    fz_stream *stream = NULL;
    fz_var(stream);
    fz_set_aa_level(context, 8);
    fz_try(context) {
        stream = fz_open_file(context, path);
        pdf->key = fingerprint(context, stream);
        pdf->document = &pdf_open_document_with_stream(context, stream)->super;
        pdf->locked = fz_needs_password(context, pdf->document);
        if (!pdf->locked) {
            pdf->pages = fz_count_pages(context, pdf->document);
            if (pdf->pages <= 0) fz_throw(context, FZ_ERROR_FORMAT, "This PDF has no pages.");
        }
    }
    fz_always(context) {
        fz_drop_stream(context, stream);
    }
    fz_catch(context) {
        report(error, error_size, fz_caught_message(context));
        pdf_close(pdf);
        return NULL;
    }
    return pdf;
}

void pdf_close(Pdf *pdf)
{
    if (pdf == NULL) return;
    fz_drop_display_list(pdf->context, pdf->page);
    fz_drop_document(pdf->context, pdf->document);
    fz_drop_context(pdf->context);
    free(pdf);
}

bool pdf_locked(const Pdf *pdf) { return pdf->locked; }
int pdf_page_count(const Pdf *pdf) { return pdf->pages; }
uint64_t pdf_key(const Pdf *pdf) { return pdf->key; }

bool pdf_unlock(Pdf *pdf, const char *password, char *error, int error_size)
{
    fz_context *context = pdf->context;
    fz_try(context) {
        if (!fz_authenticate_password(context, pdf->document, password))
            fz_throw(context, FZ_ERROR_ARGUMENT, "Incorrect PDF password.");
        pdf->pages = fz_count_pages(context, pdf->document);
        if (pdf->pages <= 0) fz_throw(context, FZ_ERROR_FORMAT, "This PDF has no pages.");
        pdf->locked = false;
    }
    fz_catch(context) {
        report(error, error_size, fz_caught_message(context));
        return false;
    }
    return true;
}

bool pdf_render(Pdf *pdf, float scale, PixelBox pixels, PdfImage *image,
                char *error, int error_size)
{
    *image = (PdfImage){0};
    int width = pixels.x1 - pixels.x0;
    int height = pixels.y1 - pixels.y0;
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096 ||
        (size_t)width * (size_t)height > 10000000) {
        report(error, error_size, "The requested page area is too large.");
        return false;
    }

    fz_context *context = pdf->context;
    fz_irect box = {pixels.x0, pixels.y0, pixels.x1, pixels.y1};
    fz_pixmap *pixmap = NULL;
    fz_device *device = NULL;
    fz_var(pixmap);
    fz_var(device);

    fz_try(context) {
        pixmap = fz_new_pixmap_with_bbox(context, fz_device_rgb(context), box, NULL, 0);
        fz_clear_pixmap_with_value(context, pixmap, 255);
        device = fz_new_draw_device(context, fz_identity, pixmap);
        fz_run_display_list(context, pdf->page, device, fz_scale(scale, scale),
                            fz_rect_from_irect(box), NULL);
        fz_close_device(context, device);
        size_t row_bytes = (size_t)width * 3;
        image->pixels = malloc(row_bytes * (size_t)height);
        if (image->pixels == NULL)
            fz_throw(context, FZ_ERROR_SYSTEM, "Out of memory rendering PDF.");
        const unsigned char *source = fz_pixmap_samples(context, pixmap);
        int stride = fz_pixmap_stride(context, pixmap);
        for (int y = 0; y < height; y++)
            memcpy(image->pixels + (size_t)y * row_bytes, source + (size_t)y * (size_t)stride, row_bytes);
        image->width = width;
        image->height = height;
    }
    fz_always(context) {
        fz_drop_device(context, device);
        fz_drop_pixmap(context, pixmap);
    }
    fz_catch(context) {
        report(error, error_size, fz_caught_message(context));
        pdf_image_free(image);
        return false;
    }
    return true;
}

void pdf_image_free(PdfImage *image)
{
    free(image->pixels);
    *image = (PdfImage){0};
}

PdfBox pdf_content_bounds(Pdf *pdf)
{
    PdfBox bounds = pdf->bounds;
    float scale = 440.0f / fmaxf(bounds.x1 - bounds.x0, bounds.y1 - bounds.y0);
    PixelBox pixels = {
        (int)floorf(bounds.x0 * scale), (int)floorf(bounds.y0 * scale),
        (int)ceilf(bounds.x1 * scale), (int)ceilf(bounds.y1 * scale)
    };
    PdfImage image;
    if (!pdf_render(pdf, scale, pixels, &image, NULL, 0)) return bounds;

    int left = image.width, right = -1, top = image.height, bottom = -1;
    for (int y = 0; y < image.height; y++) {
        for (int x = 0; x < image.width; x++) {
            const unsigned char *color = image.pixels + ((size_t)y * image.width + x) * 3;
            if (color[0] >= 238 && color[1] >= 238 && color[2] >= 238) continue;
            if (x < left) left = x;
            if (x > right) right = x;
            if (y < top) top = y;
            if (y > bottom) bottom = y;
        }
    }
    pdf_image_free(&image);
    if (left > right || top > bottom) return bounds;

    float padding = 11.0f;
    PdfBox crop = {
        fmaxf(bounds.x0, (pixels.x0 + left) / scale - padding),
        fmaxf(bounds.y0, (pixels.y0 + top) / scale - padding),
        fminf(bounds.x1, (pixels.x0 + right + 1) / scale + padding),
        fminf(bounds.y1, (pixels.y0 + bottom + 1) / scale + padding)
    };

    // Restrict cropping on sparse pages so a page number cannot cause extreme zoom.
    float max_x = (bounds.x1 - bounds.x0) * 0.20f;
    float max_y = (bounds.y1 - bounds.y0) * 0.20f;
    if (crop.x0 > bounds.x0 + max_x) crop.x0 = bounds.x0 + max_x;
    if (crop.x1 < bounds.x1 - max_x) crop.x1 = bounds.x1 - max_x;
    if (crop.y0 > bounds.y0 + max_y) crop.y0 = bounds.y0 + max_y;
    if (crop.y1 < bounds.y1 - max_y) crop.y1 = bounds.y1 - max_y;
    return crop;
}

static bool valid_bounds(fz_rect bounds)
{
    float width = bounds.x1 - bounds.x0;
    float height = bounds.y1 - bounds.y0;
    return isfinite(width) && isfinite(height) && width >= 1 && height >= 1 &&
        fabsf(bounds.x0) <= 1000000 && fabsf(bounds.y0) <= 1000000 &&
        width <= 1000000 && height <= 1000000;
}

bool pdf_measure_page(Pdf *pdf, int index, PdfBox *bounds)
{
    fz_context *context = pdf->context;
    fz_rect box;
    fz_matrix transform;
    fz_try(context) {
        pdf_obj *page = pdf_lookup_page_obj(context, pdf_specifics(context, pdf->document), index);
        pdf_page_obj_transform(context, page, &box, &transform);
        box = fz_transform_rect(box, transform);
    }
    fz_catch(context) {
        return false;
    }
    if (!valid_bounds(box)) return false;
    *bounds = (PdfBox){box.x0, box.y0, box.x1, box.y1};
    return true;
}

bool pdf_set_page(Pdf *pdf, int index, char *error, int error_size)
{
    if (pdf->page != NULL && pdf->page_index == index) return true;
    fz_context *context = pdf->context;
    fz_display_list *new_page = NULL;
    fz_rect bounds;
    fz_var(new_page);
    fz_try(context) {
        new_page = fz_new_display_list_from_page_number(context, pdf->document, index);
        bounds = fz_bound_display_list(context, new_page);
        if (!valid_bounds(bounds))
            fz_throw(context, FZ_ERROR_FORMAT, "Unsupported PDF page dimensions.");
    }
    fz_catch(context) {
        fz_drop_display_list(context, new_page);
        report(error, error_size, fz_caught_message(context));
        return false;
    }
    fz_drop_display_list(context, pdf->page);
    pdf->page = new_page;
    pdf->bounds = (PdfBox){bounds.x0, bounds.y0, bounds.x1, bounds.y1};
    pdf->page_index = index;
    return true;
}


static void read_contents(Pdf *pdf, fz_outline_iterator *iterator, PdfContents *contents)
{
    fz_context *ctx = pdf->context;
    size_t parent = SIZE_MAX;
    size_t capacity = 0;
    const fz_outline_item *item;
    while ((item = fz_outline_iterator_item(ctx, iterator)) != NULL) {
        if (contents->count == capacity) {
            size_t next = capacity == 0 ? 32 : capacity * 2;
            if (next < capacity || next > SIZE_MAX / sizeof *contents->entries)
                fz_throw(ctx, FZ_ERROR_SYSTEM, "The PDF outline is too large.");
            PdfContentsEntry *entries = realloc(contents->entries, next * sizeof *entries);
            if (entries == NULL) fz_throw(ctx, FZ_ERROR_SYSTEM, "Out of memory reading PDF contents.");
            contents->entries = entries;
            capacity = next;
        }

        size_t index = contents->count++;
        PdfContentsEntry *entry = &contents->entries[index];
        *entry = (PdfContentsEntry){
            .page = -1, .x = NAN, .y = NAN, .parent = parent, .after = contents->count,
            .depth = parent == SIZE_MAX ? 0 : contents->entries[parent].depth + 1,
            .expanded = item->is_open
        };
        const char *title = item->title != NULL && *item->title != '\0' ? item->title : "Untitled";
        size_t length = 0;
        while (length < sizeof entry->title - 1 && title[length] != '\0') {
            unsigned char ch = (unsigned char)title[length];
            entry->title[length++] = ch < 32 || ch == 127 ? ' ' : (char)ch;
        }
        while (length > 0 && ((unsigned char)title[length] & 0xc0) == 0x80) length--;
        entry->title[length] = '\0';

        if (item->uri != NULL && !fz_is_external_link(ctx, item->uri)) {
            fz_location target = fz_resolve_link(ctx, pdf->document, item->uri, &entry->x, &entry->y);
            if (target.chapter >= 0 && target.page >= 0) {
                int page = fz_page_number_from_location(ctx, pdf->document, target);
                if (page >= 0 && page < pdf->pages) entry->page = page;
            }
        }

        int child = fz_outline_iterator_down(ctx, iterator);
        if (child == FZ_OUTLINE_ITERATOR_AT_ITEM) {
            parent = index;
            continue;
        }
        if (child == FZ_OUTLINE_ITERATOR_AT_EMPTY) fz_outline_iterator_up(ctx, iterator);

        while (fz_outline_iterator_next(ctx, iterator) != FZ_OUTLINE_ITERATOR_AT_ITEM) {
            if (parent == SIZE_MAX) return;
            if (fz_outline_iterator_up(ctx, iterator) != FZ_OUTLINE_ITERATOR_AT_ITEM)
                fz_throw(ctx, FZ_ERROR_FORMAT, "The PDF outline has a missing parent.");
            contents->entries[parent].after = contents->count;
            parent = contents->entries[parent].parent;
        }
    }
}

bool pdf_load_contents(Pdf *pdf, PdfContents *contents, char *error, int error_size)
{
    *contents = (PdfContents){0};
    fz_context *ctx = pdf->context;
    fz_outline_iterator *iterator = NULL;
    fz_var(iterator);
    fz_try(ctx) {
        iterator = fz_new_outline_iterator(ctx, pdf->document);
        if (iterator != NULL) read_contents(pdf, iterator, contents);
    }
    fz_always(ctx) {
        fz_drop_outline_iterator(ctx, iterator);
    }
    fz_catch(ctx) {
        pdf_free_contents(contents);
        report(error, error_size, fz_caught_message(ctx));
        return false;
    }
    return true;
}

void pdf_free_contents(PdfContents *contents)
{
    free(contents->entries);
    *contents = (PdfContents){0};
}
