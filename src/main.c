#include "pdf.h"
#include "pdf_worker.h"
#include "json.h"
#include "text_util.h"
#include "platform.h"
#include "reading_state.h"

#include <raylib.h>
#include <raymath.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
EM_JS(double, web_density, (void), { return window.devicePixelRatio || 1; })
EM_JS(int, web_width, (void), { return window.innerWidth; })
EM_JS(int, web_height, (void), { return window.innerHeight; })
#endif

extern const unsigned char folio_font[];
extern const int folio_font_size;

typedef enum { ORIGINAL, WARM, DARK, SEPIA, SOFT_GRAY, SAGE, DUSK, NIGHT_AMBER, THEME_COUNT } Theme;
typedef enum { INPUT_NONE, INPUT_PAGE, INPUT_PASSWORD, INPUT_PATH, INPUT_BOOKMARK, INPUT_FIND } InputKind;
typedef enum { MENU_NONE, MENU_VIEW, MENU_THEME_TOP, MENU_THEME_BOTTOM, MENU_READING } Menu;
typedef enum { READING_CONTENTS, READING_BOOKMARKS, READING_RECENT, READING_THUMBNAILS, READING_TAB_COUNT } ReadingTab;
typedef enum { CONTENTS_UNREAD, CONTENTS_READY, CONTENTS_FAILED } ContentsStatus;
enum { RECENT_LIMIT = 24, HISTORY_LIMIT = 128, UI_LIMIT = 256 };
typedef enum { VIEW_CONTINUOUS, VIEW_SINGLE, VIEW_SPREAD } ViewMode;

typedef struct {
    Color background, panel, ink, muted, line, button, hover, accent, accent_ink, paper, page_ink;
} Palette;

static const Palette palettes[] = {
    {{232, 235, 240, 255}, {252, 253, 255, 255}, {29, 37, 53, 255}, {82, 94, 111, 255},
     {215, 222, 232, 255}, {238, 242, 248, 255}, {224, 232, 246, 255}, {38, 73, 139, 255},
     {255, 255, 255, 255}, {255, 255, 255, 255}, {0, 0, 0, 255}},
    {{226, 220, 206, 255}, {250, 245, 233, 255}, {49, 41, 31, 255}, {104, 90, 74, 255},
     {222, 210, 190, 255}, {241, 232, 215, 255}, {233, 219, 195, 255}, {112, 68, 36, 255},
     {255, 255, 255, 255}, {249, 236, 209, 255}, {43, 33, 23, 255}},
    {{27, 29, 34, 255}, {38, 41, 49, 255}, {231, 235, 243, 255}, {171, 182, 200, 255},
     {60, 67, 80, 255}, {49, 55, 67, 255}, {63, 73, 90, 255}, {164, 198, 254, 255},
     {16, 37, 66, 255}, {32, 35, 42, 255}, {232, 235, 240, 255}},
    // Sepia: deeper parchment with brown ink.
    {{213, 198, 172, 255}, {236, 222, 197, 255}, {72, 51, 34, 255}, {106, 84, 60, 255},
     {197, 177, 146, 255}, {226, 208, 178, 255}, {215, 193, 159, 255}, {117, 75, 41, 255},
     {255, 246, 227, 255}, {232, 215, 181, 255}, {72, 51, 34, 255}},
    // Soft Gray: a neutral page without a bright white background.
    {{202, 206, 206, 255}, {229, 232, 231, 255}, {50, 57, 57, 255}, {84, 93, 92, 255},
     {182, 190, 188, 255}, {216, 222, 220, 255}, {202, 213, 209, 255}, {61, 91, 81, 255},
     {244, 248, 246, 255}, {221, 225, 223, 255}, {58, 62, 63, 255}},
    // Sage: muted green paper and a matching, quiet interface.
    {{195, 208, 190, 255}, {224, 234, 218, 255}, {43, 60, 45, 255}, {78, 98, 77, 255},
     {174, 194, 166, 255}, {208, 223, 201, 255}, {192, 213, 184, 255}, {65, 101, 63, 255},
     {245, 249, 239, 255}, {216, 230, 207, 255}, {47, 62, 52, 255}},
    // Dusk: a softer dark page with a warm gray foreground.
    {{38, 35, 43, 255}, {53, 48, 59, 255}, {221, 212, 219, 255}, {175, 161, 175, 255},
     {79, 69, 85, 255}, {65, 57, 72, 255}, {84, 71, 91, 255}, {202, 176, 210, 255},
     {48, 33, 55, 255}, {48, 43, 54, 255}, {215, 205, 207, 255}},
    // Night Amber: dim, warm lettering on a brown charcoal page.
    {{28, 25, 20, 255}, {42, 36, 27, 255}, {226, 196, 147, 255}, {173, 148, 111, 255},
     {74, 61, 42, 255}, {57, 47, 32, 255}, {77, 61, 39, 255}, {216, 174, 103, 255},
     {38, 28, 14, 255}, {38, 33, 25, 255}, {216, 186, 139, 255}}
};

static const char *theme_names[] = {
    "Original", "Warm Paper", "Dark", "Sepia", "Soft Gray", "Sage", "Dusk", "Night Amber"
};
_Static_assert(sizeof palettes / sizeof *palettes == THEME_COUNT, "Every theme needs a palette");
_Static_assert(sizeof theme_names / sizeof *theme_names == THEME_COUNT, "Every theme needs a name");
static const float wheel_scroll_distance = 104.0f;

typedef struct {
    Texture2D texture;
    PdfBox area;
    float scale;
} PageTexture;

typedef struct {
    PdfBox bounds, crop;
    PageTexture preview, detail, thumbnail;
    PdfText *text;
    bool preview_pending, detail_pending, text_pending, thumb_pending, text_failed;
    double offset;
    float scale, width, height;
    bool crop_ready, preview_attempted;
} ReadingPage;

typedef struct {
    float ui, width, height, bar;
    Rectangle body, open, find, reading, theme, settings, title, prev, page, next, out, fit, in, bottom_theme;
    Rectangle search_field, search_prev, search_next, search_close, loading_cancel;
    Rectangle brightness_less, brightness_more;
    Rectangle modes[3], rotate_left, rotate_right, history_back, history_forward, print, share, export_copy;
    Rectangle popup, theme_popup, themes[THEME_COUNT], view_theme, crop, brightness, focus, status;
    Rectangle dialog, field, cancel, confirm;
} Layout;

typedef struct {
    Vector2 point, start, previous, delta, pinch_center;
    float pinch_distance;
    bool down, pressed, released, moved, pinch;
    int touches;
    double began;
} Pointer;

typedef struct {
    Rectangle track, thumb;
    double maximum;
} Scrollbar;

typedef struct {
    float grab;
    bool active;
} ScrollbarDrag;

typedef struct {
    Bookmark *bookmarks;
    RecentSession sessions[RECENT_LIMIT];
    PdfContents contents;
    size_t *visible_contents, visible_count;
    ContentsStatus contents_status;
    ScrollbarDrag scrollbar;
    size_t bookmark_count, session_count, selected;
    double scroll; // Fractional rows, independent of the keyboard selection.
    ReadingTab tab;
    Vector2 anchor;
    bool context;
} ReadingMenu;

typedef struct {
    int page;
    size_t first, last;
    bool active, dragging;
    int handle;
} TextSelection;

typedef struct {
    Rectangle box;
    char label[256];
    unsigned id;
    bool enabled, selected, text;
} UiElement;

typedef struct {
    Pdf *pdf, *locked;
    PdfWorker *worker, *opening;
    FileEvent source, opening_file;
    char password[256], opening_password[256];
    bool password_requested, loading_cancelled;
    int opening_done, opening_total;
    TextSelection selection;
    bool search_open, search_auto_jump;
    char query[256];
    PdfSearch search;
    int search_index;
    SavedPosition history[HISTORY_LIMIT];
    int history_count, history_cursor;
    bool history_restoring;
    ViewMode view_mode;
    int rotation, thumb_start, thumb_end;
    float view_scroll;
    double accessibility_due;
    UiElement elements[UI_LIMIT];
    size_t element_count;
    unsigned focus_id;
    bool building_menu, keyboard_activation;
    char title[FOLIO_NAME_MAX], pending_title[FOLIO_NAME_MAX];
    char pending_locator[FOLIO_PATH_MAX];
    char message[320], input[FOLIO_PATH_MAX];
    ReadingPage *pages;
    Layout layout;
    Pointer pointer;
    Vector2 wheel;
    ScrollbarDrag scrollbar;
    Font font;
    Shader shader;
    int shader_theme, shader_brightness, shader_paper, shader_ink;
    int page, page_count, cache_start, cache_end, theme_cursor;
    float zoom, brightness, pan, widest;
    double scroll, document_height, render_due, save_due, message_until;
    Theme theme;
    Menu menu;
    ReadingMenu reading;
    SavedPosition pending_bookmark;
    uint64_t bookmark_document, resume_document;
    InputKind input_kind;
    bool crop, fit_width, focused, system_input, input_replace, resuming, bookmark_edit, quit;
} Viewer;

static Viewer viewer;

static void begin_find(void);
static void find_next(int direction);
static void search_close(void);
static void copy_selection(void);
static void select_all_text(void);
static void history_push(void);
static void history_move(int direction);
static void set_view_mode(ViewMode mode);
static void rotate_document(int direction);
static bool selection_pointer(void);
static void draw_interaction(void);
static void draw_search(void);
static void draw_loading(void);
static void draw_focus(void);
static void update_loading(void);
static void update_document_work(void);
static void update_search(void);
static void update_commands(void);
static void update_keyboard_focus(void);
static void publish_accessibility(void);
static void register_element(Rectangle box, const char *label, bool enabled, bool selected, bool text);
static bool follow_link_at(Vector2 point);
static void document_action(int action);
static Vector2 pdf_to_screen(int page,Vector2 point);
static Vector2 screen_to_pdf(int page,Vector2 point);
static Rectangle pdf_box_screen(int page,PdfBox box);
static Rectangle selection_copy_box(void);
static void draw_thumbnail_row(Rectangle row,size_t index);

static float minf(float a, float b) { return fminf(a, b); }
static float maxf(float a, float b) { return fmaxf(a, b); }

static Rectangle rect(float x, float y, float width, float height)
{
    return (Rectangle){x, y, width, height};
}

static Vector2 center(Rectangle box)
{
    return (Vector2){box.x + box.width * 0.5f, box.y + box.height * 0.5f};
}

static bool contains(Rectangle box, Vector2 point)
{
    return CheckCollisionPointRec(point, box);
}

static bool back_pressed(void)
{
#if defined(__ANDROID__)
    if (IsKeyPressed(KEY_BACK)) return true;
#endif
    return IsKeyPressed(KEY_ESCAPE);
}

static void set_message(const char *message)
{
    snprintf(viewer.message, sizeof viewer.message, "%s", message);
    viewer.message_until = GetTime() + 6.0;
}

static float ui_scale(void)
{
#if defined(__EMSCRIPTEN__)
    return (float)web_density();
#elif defined(__ANDROID__)
    return Clamp(GetWindowScaleDPI().x, 1.0f, 4.0f);
#else
    return 1.0f;
#endif
}

static Layout layout(void)
{
    Layout result = {
        .ui = ui_scale(), .width = (float)GetScreenWidth(), .height = (float)GetScreenHeight()
    };
    float u = result.ui;
    float bar = 54 * u;
    float button = 42 * u;
    float gap = 4 * u;
    float x = 8 * u;
    float y = 6 * u;
    result.bar = bar;
    result.body = viewer.focused ? rect(0, 0, result.width, result.height)
                                 : rect(0, bar, result.width, maxf(1, result.height - 2 * bar));
    if (viewer.search_open) {
        result.search_field = rect(8*u, bar+4*u, result.width-144*u, 44*u);
        result.search_prev = rect(result.width-130*u, bar+4*u, 38*u, 44*u);
        result.search_next = rect(result.width-88*u, bar+4*u, 38*u, 44*u);
        result.search_close = rect(result.width-46*u, bar+4*u, 38*u, 44*u);
        result.body.y += 52*u;
        result.body.height = maxf(1, result.body.height-52*u);
    }
    result.loading_cancel = rect(result.width-106*u, result.body.y+8*u, 94*u, 38*u);
    bool compact = result.width < 560 * u;
    float toolbar_scale = minf(1, maxf(0.5f, (result.width - 24 * u) / (274 * u)));
    float open_width = (compact ? 54 : 88) * u * toolbar_scale;
    float view_width = (compact ? 46 : 54) * u * toolbar_scale;
    float theme_width = (compact ? 62 : 82) * u * toolbar_scale;
    float reading_width = (compact ? 66 : 88) * u * toolbar_scale;
    result.open = rect(x, y, open_width, button);
    result.find = rect(x+open_width+gap, y, (compact ? 44 : 62)*u, button);
    result.settings = rect(result.width - view_width - x, y, view_width, button);
    result.theme = rect(result.settings.x - theme_width - gap, y, theme_width, button);
    result.reading = rect(result.theme.x - reading_width - gap, y, reading_width, button);
    result.title = rect(result.find.x + result.find.width + 12*u, y,
                        result.reading.x-result.find.x-result.find.width-24*u, button);

    float group = minf(428 * u, result.width - 2 * x);
    x = (result.width - group) * 0.5f;
    y = result.height - bar + 6 * u;
    float fit_width = minf(88 * u, group * 0.23f);
    float page_width = minf(106 * u, group * 0.22f);
    float key_width = (group - page_width - fit_width - 6 * gap) / 5;
    result.prev = rect(x, y, key_width, button);
    result.page = rect(x + key_width + gap, y, page_width, button);
    result.next = rect(result.page.x + page_width + gap, y, key_width, button);
    result.out = rect(result.next.x + key_width + gap, y, key_width, button);
    result.fit = rect(result.out.x + key_width + gap, y, fit_width, button);
    result.in = rect(result.fit.x + result.fit.width + gap, y, key_width, button);
    result.bottom_theme = rect(result.in.x + key_width + gap, y, key_width, button);

    float panel_width = minf(result.width - 16 * u, 360 * u);
    result.popup = rect(result.width-panel_width-8*u, result.body.y+8*u, panel_width,
                        minf(450*u, maxf(80*u, result.body.height-16*u)));
    x = result.popup.x + 12 * u;
    float inner = result.popup.width - 24 * u;
    y = result.popup.y + 8*u - viewer.view_scroll;
    result.view_theme = rect(x, y, inner, button);
    result.brightness=rect(x+42*u,y+60*u,inner-84*u,26*u);
    result.brightness_less=rect(x,y+52*u,34*u,42*u);
    result.brightness_more=rect(x+inner-34*u,y+52*u,34*u,42*u);
    result.crop = rect(x, y + 91 * u, inner, button);
    result.focus = rect(x, y+137*u, inner, button);
    float third = (inner-8*u)/3;
    for (int i=0; i<3; i++) result.modes[i]=rect(x+i*(third+4*u), y+183*u, third, button);
    float half = (inner-4*u)/2;
    result.rotate_left=rect(x,y+229*u,half,button);
    result.rotate_right=rect(x+half+4*u,y+229*u,half,button);
    result.history_back=rect(x,y+275*u,half,button);
    result.history_forward=rect(x+half+4*u,y+275*u,half,button);
    result.print=rect(x,y+321*u,half,button);
    result.share=rect(x+half+4*u,y+321*u,half,button);
    result.export_copy=rect(x,y+367*u,inner,button);

    Rectangle trigger = viewer.menu == MENU_THEME_BOTTOM ? result.bottom_theme : result.theme;
    int columns = THEME_COUNT * button + 12 * u > result.height - 2 * bar - 12 * u ? 2 : 1;
    int rows = (THEME_COUNT + columns - 1) / columns;
    float menu_width = minf(190 * columns * u, result.width - 16 * u);
    float menu_height = rows * button + 12 * u;
    float column_width = (menu_width - 12 * u) / columns;
    float menu_x = Clamp(trigger.x + trigger.width - menu_width, 8 * u, result.width - menu_width - 8 * u);
    float menu_y = viewer.menu == MENU_THEME_BOTTOM ? result.height - bar - menu_height - 6 * u : bar + 6 * u;
    result.theme_popup = rect(menu_x, menu_y, menu_width, menu_height);
    for (int i = 0; i < THEME_COUNT; i++)
        result.themes[i] = rect(menu_x + 6 * u + (i / rows) * column_width,
                                menu_y + 6 * u + (i % rows) * button, column_width, button);

    float dialog_width = minf(460 * u, result.width - 24 * u);
    result.dialog = rect((result.width - dialog_width) * 0.5f, (result.height - 171 * u) * 0.5f,
                         dialog_width, 171 * u);
    x = result.dialog.x + 16 * u;
    y = result.dialog.y;
    result.field = rect(x, y + 52 * u, dialog_width - 32 * u, button);
    result.confirm = rect(x + dialog_width - 114 * u, y + 110 * u, 82 * u, button);
    result.cancel = rect(result.confirm.x - 80 * u - gap, y + 110 * u, 80 * u, button);

    float status_width = minf(result.width - 24 * u, 680 * u);
    result.status = rect((result.width - status_width) * 0.5f,
                         result.body.y + result.body.height - 53 * u, status_width, 42 * u);
    return result;
}

static float page_margin(void) { return 12 * viewer.layout.ui; }
static float page_gap(void) { return 16 * viewer.layout.ui; }

static PdfBox reading_bounds(const ReadingPage *page)
{
    return viewer.crop && page->crop_ready ? page->crop : page->bounds;
}

static void measure_document(void)
{
    Rectangle body = viewer.layout.body;
    float margin = page_margin();
    double offset = 0;
    viewer.widest = 0;
    int columns = viewer.view_mode == VIEW_SPREAD ? 2 : 1;
    for (int i=0; i<viewer.page_count; i+=columns) {
        float widths[2]={0}, heights[2]={0};
        int count= i+columns<=viewer.page_count ? columns : 1;
        for (int j=0;j<count;j++) {
            PdfBox bounds=reading_bounds(&viewer.pages[i+j]);
            widths[j]=bounds.x1-bounds.x0;
            heights[j]=bounds.y1-bounds.y0;
            if (viewer.rotation%2) { float swap=widths[j];widths[j]=heights[j];heights[j]=swap; }
        }
        float width=widths[0]+widths[1];
        float height=maxf(heights[0],heights[1]);
        float scale=maxf(1,body.width-2*margin-(count-1)*page_gap())/width;
        if (!viewer.fit_width) scale=minf(scale,maxf(1,body.height-2*margin)/height);
        for(int j=0;j<count;j++) {
            ReadingPage *page=&viewer.pages[i+j];
            page->offset=offset;
            page->scale=scale;
            page->width=widths[j]*scale;
            page->height=heights[j]*scale;
        }
        viewer.widest=maxf(viewer.widest,width*scale);
        offset+=height*scale;
    }
    viewer.document_height=offset;
}

static double page_top(int index)
{
    if (viewer.view_mode==VIEW_SINGLE) return page_margin();
    int row=viewer.view_mode==VIEW_SPREAD?index/2:index;
    return page_margin()+viewer.pages[index].offset*viewer.zoom+(double)row*page_gap();
}

static double page_bottom(int index)
{
    return page_top(index)+viewer.pages[index].height*viewer.zoom;
}

static float pan_limit(float width)
{
    return maxf(0, (width - viewer.layout.body.width) * 0.5f + page_margin());
}

static Rectangle page_rect(int index)
{
    Rectangle body=viewer.layout.body;
    const ReadingPage *page=&viewer.pages[index];
    float width=page->width*viewer.zoom;
    float group_width=width;
    if(viewer.view_mode==VIEW_SPREAD) {
        int left=index-index%2;
        group_width=viewer.pages[left].width*viewer.zoom;
        if(left+1<viewer.page_count) group_width+=viewer.pages[left+1].width*viewer.zoom+page_gap();
    }
    float x=body.x+(body.width-group_width)*.5f+Clamp(viewer.pan,-pan_limit(group_width),pan_limit(group_width));
    if(viewer.view_mode==VIEW_SPREAD && index%2) x+=viewer.pages[index-1].width*viewer.zoom+page_gap();
    return rect(x,body.y+(float)(page_top(index)-viewer.scroll),width,page->height*viewer.zoom);
}

static int page_at(double offset)
{
    if(viewer.view_mode==VIEW_SINGLE) return viewer.page;
    int columns=viewer.view_mode==VIEW_SPREAD?2:1;
    int first=0,end=(viewer.page_count+columns-1)/columns;
    while(first<end) {
        int middle=first+(end-first)/2;
        int index=middle*columns;
        double bottom=page_bottom(index);
        if(columns==2 && index+1<viewer.page_count) bottom=fmax(bottom,page_bottom(index+1));
        if(bottom<=offset) first=middle+1; else end=middle;
    }
    int index=first*columns;
    return index<viewer.page_count?index:(viewer.page_count-1)/columns*columns;
}

static double document_height(void)
{
    if(viewer.view_mode==VIEW_SINGLE) return 2*page_margin()+viewer.pages[viewer.page].height*viewer.zoom;
    int rows=viewer.view_mode==VIEW_SPREAD?(viewer.page_count+1)/2:viewer.page_count;
    return 2*page_margin()+viewer.document_height*viewer.zoom+(double)(rows-1)*page_gap();
}

static void clamp_view(void)
{
    viewer.pan=Clamp(viewer.pan,-pan_limit(viewer.widest*viewer.zoom),pan_limit(viewer.widest*viewer.zoom));
    double maximum=fmax(0,document_height()-viewer.layout.body.height);
    viewer.scroll=fmax(0,fmin(maximum,viewer.scroll));
    if(viewer.view_mode!=VIEW_SINGLE) viewer.page=page_at(viewer.scroll+page_margin());
}

static void refresh(double delay)
{
    viewer.render_due = GetTime() + delay;
    viewer.save_due = GetTime() + 0.55;
}

static Vector2 reading_point(void)
{
    Rectangle body = viewer.layout.body;
    float x=body.x+body.width*.5f;
    if(viewer.view_mode==VIEW_SPREAD && viewer.pdf!=NULL) {
        Rectangle page=page_rect(viewer.page);
        x=page.x+page.width*.5f;
    }
    return (Vector2){x,body.y+page_margin()};
}

static ReadingPosition position_at(Vector2 point)
{
    int index=page_at(viewer.scroll+point.y-viewer.layout.body.y);
    if(viewer.view_mode==VIEW_SPREAD && index+1<viewer.page_count && contains(page_rect(index+1),point)) index++;
    Rectangle page=page_rect(index);
    return (ReadingPosition){index, (point.x - page.x) / page.width,
        (viewer.scroll + point.y - viewer.layout.body.y - page_top(index)) / page.height};
}

static void restore_position(ReadingPosition location, Vector2 point)
{
    Rectangle body = viewer.layout.body;
    ReadingPage *page = &viewer.pages[location.page];
    float width = page->width * viewer.zoom;
    viewer.pan = point.x - body.x - body.width * 0.5f - (float)(location.x - 0.5) * width;
    viewer.scroll = page_top(location.page) + location.y * page->height * viewer.zoom - (point.y - body.y);
    clamp_view();
}

static SavedPosition current_position(void)
{
    return (SavedPosition){position_at(reading_point()),viewer.zoom,viewer.fit_width,3,viewer.view_mode,viewer.rotation};
}

static void save_reading(void)
{
    if (viewer.pdf != NULL) reading_save_position(pdf_key(viewer.pdf), current_position());
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE void folio_save_session(void)
{
    if (viewer.save_due >= 0) save_reading();
}
#endif

static void save_settings(void)
{
    char value[64];
    snprintf(value, sizeof value, "1 %d %d %.6g\n", viewer.theme, viewer.crop, viewer.brightness);
    platform_write("settings", value);
}

static void load_settings(void)
{
    char value[64];
    int theme, crop;
    float brightness;
    if (platform_read("settings", value, sizeof value) &&
        sscanf(value, "1 %d %d %f", &theme, &crop, &brightness) == 3 &&
        theme >= 0 && theme < THEME_COUNT && isfinite(brightness)) {
        viewer.theme = (Theme)theme;
        viewer.crop = crop != 0;
        viewer.brightness = Clamp(brightness, 0.4f, 1.0f);
    }
}

static void release_texture(PageTexture *page)
{
    if (page->texture.id != 0) UnloadTexture(page->texture);
    *page = (PageTexture){0};
}

static bool upload_texture(PageTexture *target, PdfImage image, float scale, PixelBox pixels)
{
    Image raw={image.pixels,image.width,image.height,1,PIXELFORMAT_UNCOMPRESSED_R8G8B8};
    Texture2D texture=LoadTextureFromImage(raw);
    if(!IsTextureValid(texture)) { set_message("Could not upload the PDF page to the display."); return false; }
    SetTextureFilter(texture,TEXTURE_FILTER_BILINEAR);
    release_texture(target);
    target->texture=texture;
    target->scale=scale;
    target->area=(PdfBox){pixels.x0/scale,pixels.y0/scale,pixels.x1/scale,pixels.y1/scale};
    return true;
}

static PixelBox pixel_box(PdfBox bounds, float scale)
{
    return (PixelBox){(int)floorf(bounds.x0 * scale), (int)floorf(bounds.y0 * scale),
                      (int)ceilf(bounds.x1 * scale), (int)ceilf(bounds.y1 * scale)};
}

static void clear_cached_page(ReadingPage *page)
{
    release_texture(&page->preview);
    release_texture(&page->detail);
    page->preview_attempted = false;
}

static void close_document(void)
{
    for(int i=0;i<viewer.page_count;i++) {
        clear_cached_page(&viewer.pages[i]);
        release_texture(&viewer.pages[i].thumbnail);
        pdf_text_free(viewer.pages[i].text);
    }
    viewer.cache_start=viewer.cache_end=0;
    free(viewer.pages);
    viewer.pages=NULL;
    viewer.page_count=0;
    pdf_free_contents(&viewer.reading.contents);
    free(viewer.reading.visible_contents);
    viewer.reading.visible_contents=NULL;
    viewer.reading.visible_count=0;
    viewer.reading.contents_status=CONTENTS_UNREAD;
    viewer.scrollbar.active=false;
    worker_search_free(&viewer.search);
    viewer.search_open=false;
    viewer.search_index=-1;
    viewer.selection=(TextSelection){0};
    viewer.history_count=viewer.history_cursor=0;
    worker_close(viewer.worker);
    viewer.worker=NULL;
    viewer.pdf=NULL;
    platform_remove_temp(&viewer.source);
    viewer.source=(FileEvent){0};
    memset(viewer.password,0,sizeof viewer.password);
}

static void render_preview(int index)
{
    ReadingPage *page=&viewer.pages[index];
    if(page->preview_pending) return;
    PdfBox full=page->bounds;
    float side=maxf(full.x1-full.x0,full.y1-full.y0);
    float scale=minf(860.0f/side,page->scale*maxf(.5f,viewer.zoom));
    PdfRequest request={WORK_PREVIEW,index,scale,pixel_box(full,scale),viewer.crop&&!page->crop_ready};
    if(worker_request(viewer.worker,request)) page->preview_pending=true;
}

static void extend_render_range(float *start, float *end, float page_start, float page_end,
                                float padding, float maximum)
{
    float span = *end - *start;
    float extra = minf(padding, maxf(0, maximum - span));
    float old_start = *start;
    *start = maxf(page_start, *start - extra * 0.5f);
    *end = minf(page_end, *end + extra - (old_start - *start));
    *start = maxf(page_start, *end - span - extra);
}

static bool render_detail(int index, float density)
{
    Rectangle body = viewer.layout.body;
    Rectangle page = page_rect(index);
    Rectangle visible = GetCollisionRec(body, page);
    if (visible.width <= 0 || visible.height <= 0) return false;
    ReadingPage *item = &viewer.pages[index];
    if (item->preview.texture.id == 0) return false;
    PdfBox full = reading_bounds(item);
    float display_scale = item->scale * viewer.zoom;
    Vector2 a=screen_to_pdf(index,(Vector2){visible.x,visible.y});
    Vector2 b=screen_to_pdf(index,(Vector2){visible.x+visible.width,visible.y+visible.height});
    PdfBox bounds={maxf(full.x0,minf(a.x,b.x)),maxf(full.y0,minf(a.y,b.y)),
                   minf(full.x1,maxf(a.x,b.x)),minf(full.y1,maxf(a.y,b.y))};
    float width = bounds.x1 - bounds.x0;
    float height = bounds.y1 - bounds.y0;
    if (width <= 0 || height <= 0) return false;
    const float max_side = 4093.0f;
    const float max_pixels = 7500000.0f;
    float scale = minf(display_scale * density, minf(max_side / width, max_side / height));
    float furthest = maxf(maxf(fabsf(full.x0), fabsf(full.x1)), maxf(fabsf(full.y0), fabsf(full.y1)));
    scale = minf(scale, 1000000000.0f / maxf(1, furthest));
    scale = minf(scale, sqrtf(max_pixels / (width * height)));

    if (item->preview.scale >= scale * 0.992f) return false;
    PageTexture *detail = &item->detail;
    PdfBox cached = detail->area;
    if (detail->texture.id != 0 && fabsf(detail->scale / scale - 1) < 0.008f &&
        bounds.x0 >= cached.x0 && bounds.y0 >= cached.y0 &&
        bounds.x1 <= cached.x1 && bounds.y1 <= cached.y1) return false;

    // Keep extra pixels above and below the viewport without lowering its resolution.
    float pixel_area = max_pixels / (scale * scale);
    float max_span = max_side / scale;
    float vertical_padding = maxf(6 * wheel_scroll_distance * viewer.layout.ui, body.height * 0.6f) / display_scale;
    extend_render_range(&bounds.y0, &bounds.y1, full.y0, full.y1,
                        vertical_padding, minf(max_span, pixel_area / width));
    extend_render_range(&bounds.x0, &bounds.x1, full.x0, full.x1,
                        96 * viewer.layout.ui / display_scale,
                        minf(max_span, pixel_area / (bounds.y1 - bounds.y0)));
    if(item->detail_pending) return false;
    PdfRequest request={WORK_DETAIL,index,scale,pixel_box(bounds,scale),false};
    item->detail_pending=worker_request(viewer.worker,request);
    return item->detail_pending;
}

static int visible_end(int first)
{
    if(viewer.view_mode==VIEW_SINGLE) return first+1;
    double bottom = viewer.scroll + viewer.layout.body.height;
    int end = first;
    while (end < viewer.page_count && page_top(end) < bottom) end++;
    return end;
}

static void update_visible_pages(void)
{
    int first=page_at(viewer.scroll),end=visible_end(first);
    int start_cache=first>0?first-1:first;
    int end_cache=end<viewer.page_count?end+1:end;
    for(int i=viewer.cache_start;i<viewer.cache_end;i++) {
        if(i<start_cache||i>=end_cache) {
            clear_cached_page(&viewer.pages[i]);
            if(!viewer.selection.active||viewer.selection.page!=i) {
                pdf_text_free(viewer.pages[i].text);
                viewer.pages[i].text=NULL;
            }
        }
    }
    viewer.cache_start=start_cache;viewer.cache_end=end_cache;
    for(int i=first;i<end;i++) {
        ReadingPage *page=&viewer.pages[i];
        if(!page->text&&!page->text_pending&&!page->text_failed)
            page->text_pending=worker_request(viewer.worker,(PdfRequest){.kind=WORK_TEXT,.page=i});
        if(!page->preview_attempted&&!page->preview_pending) render_preview(i);
        else if(viewer.crop&&!page->crop_ready&&!page->preview_pending) render_preview(i);
    }
    if(viewer.render_due<0||GetTime()<viewer.render_due||viewer.pointer.pinch) return;
    float density=maxf(GetRenderWidth()/viewer.layout.width,GetRenderHeight()/viewer.layout.height);
    for(int i=first;i<end;i++) render_detail(i,Clamp(density,1,4));
    viewer.render_due=-1;
}

static void show_page(int index)
{
    if(!viewer.pdf||index<0||index>=viewer.page_count) return;
    history_push();
    viewer.page=index;
    viewer.pan=0;
    viewer.scroll=page_top(index)-page_margin();
    clamp_view();
    viewer.page=index;
    viewer.selection=(TextSelection){0};
    refresh(0);
}

static int adjacent_page(int direction)
{
    return viewer.view_mode==VIEW_SPREAD ? (viewer.page/2+direction)*2 : viewer.page+direction;
}

static void change_page(int direction)
{
    show_page(adjacent_page(direction));
}

static void use_document(PdfWorker *worker, const FileEvent *file)
{
    Pdf *document=worker_document(worker);
    int count=pdf_page_count(document);
    ReadingPage *pages=calloc((size_t)count,sizeof *pages);
    if(!pages) {
        worker_close(worker);platform_remove_temp(file);
        set_message("Out of memory measuring PDF pages.");return;
    }
    for(int i=0;i<count;i++) worker_bounds(worker,i,&pages[i].bounds);
    SavedPosition position;
    if(!reading_load_position(pdf_key(document),&position)||position.location.page>=count)
        position=(SavedPosition){{0,.5,0},1,true,3,VIEW_CONTINUOUS,0};
    save_reading();
    close_document();
    viewer.worker=worker;viewer.pdf=document;viewer.source=*file;
    viewer.pages=pages;viewer.page_count=count;viewer.page=position.location.page;
    viewer.zoom=position.zoom;viewer.fit_width=position.fit_width;
    viewer.view_mode=(ViewMode)position.layout;viewer.rotation=position.rotation;
    viewer.message_until=0;viewer.menu=MENU_NONE;
    snprintf(viewer.title,sizeof viewer.title,"%s",file->name[0]?file->name:GetFileName(file->path));
    measure_document();
    restore_position(position.location,position.version==1?center(viewer.layout.body):reading_point());
    char locator[FOLIO_PATH_MAX];platform_file_locator(file,locator,sizeof locator);
    reading_record_session(pdf_key(document),viewer.title,locator);
    refresh(0);
}

static const char *input_label(InputKind kind)
{
    switch (kind) {
        case INPUT_PASSWORD: return "PDF password";
        case INPUT_PAGE: return "Go to page";
        case INPUT_PATH: return "Open PDF by path";
        case INPUT_BOOKMARK: return viewer.bookmark_edit ? "Rename bookmark" : "Name bookmark";
        case INPUT_FIND: return "Find in document";
        default: return "";
    }
}

static void request_input(InputKind kind, const char *initial)
{
    viewer.input_kind = kind;
    viewer.menu = MENU_NONE;
    viewer.input_replace = initial[0] != '\0';
    snprintf(viewer.input, sizeof viewer.input, "%s", initial);
    TextInput input = kind == INPUT_PASSWORD ? TEXT_PASSWORD : kind == INPUT_PAGE ? TEXT_PAGE_NUMBER : TEXT_GENERAL;
    viewer.system_input = kind != INPUT_PATH && platform_request_text(input_label(kind), initial, input);
}

static void open_pdf(const FileEvent *event)
{
    if(event->error[0]) { set_message(event->error);return; }
    if(!event->path[0]) return;
    if(viewer.input_kind!=INPUT_NONE) {
        platform_remove_temp(event);set_message("Finish the open dialog before switching PDFs.");return;
    }
    if(viewer.opening) {
        worker_close(viewer.opening);
        platform_remove_temp(&viewer.opening_file);
    }
    viewer.input_kind=INPUT_NONE;viewer.system_input=false;
    viewer.opening=worker_open(event->path);
    if(!viewer.opening) { platform_remove_temp(event);set_message("Could not start loading this PDF.");return; }
    viewer.opening_file=*event;
    viewer.password_requested=false;
    viewer.opening_done=viewer.opening_total=0;
    memset(viewer.opening_password,0,sizeof viewer.opening_password);
    viewer.menu=MENU_NONE;
}

static void show_reading_menu(ReadingTab tab, bool context);

static void finish_input(const char *value, bool accepted)
{
    InputKind kind = viewer.input_kind;
    viewer.input_kind = INPUT_NONE;
    viewer.system_input = false;
    if(kind==INPUT_PASSWORD) {
        if(viewer.opening) {
            if(accepted) {
                snprintf(viewer.opening_password,sizeof viewer.opening_password,"%s",value);
                viewer.password_requested=false;
                worker_password(viewer.opening,value);
            } else {
                worker_close(viewer.opening);viewer.opening=NULL;
                platform_remove_temp(&viewer.opening_file);
            }
        }
    } else if(accepted&&kind==INPUT_FIND) {
        snprintf(viewer.query,sizeof viewer.query,"%s",value);
        viewer.search_open=true;
        viewer.search_index=-1;viewer.search_auto_jump=true;
        worker_search_free(&viewer.search);
        worker_search(viewer.worker,viewer.query);
    } else if (accepted && kind == INPUT_PAGE) {
        char *end;
        long page = strtol(value, &end, 10);
        if (end == value || *end != '\0' || page < 1 || page > pdf_page_count(viewer.pdf))
            set_message("Enter a page number from this document.");
        else show_page((int)page - 1);
    } else if (accepted && kind == INPUT_PATH && value[0] != '\0') {
        FileEvent event = {0};
        snprintf(event.path, sizeof event.path, "%s", value);
        open_pdf(&event);
    } else if (kind == INPUT_BOOKMARK) {
        if (accepted && !reading_save_bookmark(viewer.bookmark_document, viewer.pending_bookmark, value))
            set_message("Could not save the bookmark. Check local storage.");
        show_reading_menu(READING_BOOKMARKS, false);
    }
    if (viewer.input_kind == INPUT_NONE) memset(viewer.input, 0, sizeof viewer.input);
}

static void go_to_page(void)
{
    char current[24];
    snprintf(current, sizeof current, "%d", viewer.page + 1);
    request_input(INPUT_PAGE, current);
}

static void zoom_at(float factor, Vector2 from, Vector2 to)
{
    ReadingPosition anchor = position_at(from);
    viewer.zoom = Clamp(viewer.zoom * factor, 0.15f, 8.0f);
    restore_position(anchor, to);
    refresh(0.10);
}

static void pan_by(float x, float y)
{
    if (x == 0 && y == 0) return;
    viewer.pan += x;
    viewer.scroll -= y;
    clamp_view();
    refresh(0);
}

static void fit(bool width)
{
    viewer.fit_width = width;
    viewer.search_index=-1;
    viewer.zoom = 1;
    measure_document();
    show_page(viewer.page);
}

static void update_layout(void)
{
#ifdef __EMSCRIPTEN__
    int width = (int)round(web_width() * web_density());
    int height = (int)round(web_height() * web_density());
    if (width > 0 && height > 0 && (GetScreenWidth() != width || GetScreenHeight() != height))
        SetWindowSize(width, height);
#endif
    Layout next = layout();
    Rectangle old = viewer.layout.body;
    if (viewer.pdf != NULL && (old.width != next.body.width || old.height != next.body.height ||
        old.y != next.body.y || next.ui != viewer.layout.ui)) {
        ReadingPosition anchor = position_at(reading_point());
        viewer.layout = next;
        measure_document();
        restore_position(anchor, reading_point());
        refresh(0.05);
    } else {
        viewer.layout = next;
    }
}

static Scrollbar scrollbar_geometry(Rectangle track, double offset, double total, double visible)
{
    double maximum = total - visible;
    if (maximum <= 0 || track.height <= 1) return (Scrollbar){0};
    float height = minf(track.height - 1,
                        maxf(28 * viewer.layout.ui, (float)(track.height * visible / total)));
    float travel = track.height - height;
    float y = track.y + (float)(travel * fmax(0, fmin(1, offset / maximum)));
    return (Scrollbar){track, rect(track.x, y, track.width, height), maximum};
}

static Scrollbar document_scrollbar(void)
{
    if (viewer.pdf == NULL) return (Scrollbar){0};
    Rectangle body = viewer.layout.body;
    float u = viewer.layout.ui;
    Rectangle track = rect(body.x + body.width - 22 * u, body.y + 5 * u,
                           22 * u, body.height - 10 * u);
    return scrollbar_geometry(track, viewer.scroll, document_height(), body.height);
}

static bool drag_scrollbar(Scrollbar bar, ScrollbarDrag *drag, double *offset)
{
    Pointer *p = &viewer.pointer;
    if (bar.maximum <= 0 || !p->down) {
        drag->active = false;
        return false;
    }
    if (p->pressed && contains(bar.track, p->point)) {
        drag->active = true;
        drag->grab = contains(bar.thumb, p->point) ? p->point.y - bar.thumb.y : bar.thumb.height * 0.5f;
    }
    if (!drag->active) return false;

    float travel = bar.track.height - bar.thumb.height;
    *offset = bar.maximum * Clamp((p->point.y - bar.track.y - drag->grab) / travel, 0, 1);
    p->moved = true;
    return true;
}

static void pointer_update(void)
{
    Pointer *p = &viewer.pointer;
    int touches = GetTouchPointCount();
    p->touches=touches;
    bool down=touches>0||IsMouseButtonDown(MOUSE_BUTTON_LEFT)||IsMouseButtonDown(MOUSE_BUTTON_MIDDLE);
    Vector2 position = touches > 0 ? GetTouchPosition(0) : GetMousePosition();
    p->pressed = down && !p->down;
    p->released = !down && p->down;
    if (!p->released || touches > 0) p->point = position;
    viewer.wheel = GetMouseWheelMoveV();
    ScrollEvent scroll;
    if (platform_poll_scroll(&scroll)) {
        viewer.wheel = Vector2Add(viewer.wheel, (Vector2){scroll.horizontal, scroll.vertical});
        p->point = (Vector2){scroll.x * viewer.layout.width, scroll.y * viewer.layout.height};
    }
    p->down = down;
    if (p->pressed) {
        p->start = p->point;
        p->previous = p->point;
        p->moved = false;
        p->began=GetTime();
        viewer.focus_id=0;
    }
    p->delta = Vector2Subtract(p->point, p->previous);

    bool reading = viewer.pdf != NULL && viewer.input_kind == INPUT_NONE && viewer.menu == MENU_NONE;
    if (!reading || touches >= 2) viewer.scrollbar.active = false;
    double position_y;
    if (reading && touches < 2 && drag_scrollbar(document_scrollbar(), &viewer.scrollbar, &position_y)) {
        if (position_y != viewer.scroll) {
            viewer.scroll = position_y;
            clamp_view();
            refresh(0);
        }
        p->pinch = false;
        p->previous = p->point;
        return;
    }
    if(reading&&touches<2&&selection_pointer()) {
        p->previous=p->point;return;
    }
    if (reading && touches >= 2) {
        Vector2 second = GetTouchPosition(1);
        Vector2 midpoint = Vector2Scale(Vector2Add(p->point, second), 0.5f);
        float distance = Vector2Distance(p->point, second);
        if (p->pinch && p->pinch_distance >= 1 && distance >= 1)
            zoom_at(distance / p->pinch_distance, p->pinch_center, midpoint);
        p->pinch = true;
        p->pinch_distance = distance;
        p->pinch_center = midpoint;
        p->moved = true;
    } else if (!p->pinch && reading && down && contains(viewer.layout.body, p->start)) {
        if (Vector2Distance(p->start, p->point) > 5 * viewer.layout.ui) p->moved = true;
        if (p->moved) pan_by(p->point.x - p->previous.x, p->point.y - p->previous.y);
    }
    if (touches < 2) p->pinch = false;
    p->previous = p->point;
}

static bool clicked(Rectangle box)
{
    Pointer p = viewer.pointer;
    return p.released && !p.moved && contains(box, p.start) && contains(box, p.point);
}

static void update_modal(void)
{
    if (viewer.system_input) return;
    for (int codepoint = GetCharPressed(); codepoint > 0; codepoint = GetCharPressed()) {
        if (viewer.input_kind == INPUT_PAGE && (codepoint < '0' || codepoint > '9')) continue;
        if (viewer.input_replace) viewer.input[0] = '\0';
        viewer.input_replace = false;
        int count;
        const char *bytes = CodepointToUTF8(codepoint, &count);
        size_t length = strlen(viewer.input);
        if (length + (size_t)count < sizeof viewer.input)
            memcpy(viewer.input + length, bytes, (size_t)count + 1);
    }
    size_t length = strlen(viewer.input);
    if (length > 0 && (IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE))) {
        if (viewer.input_replace) length = 0;
        else do { length--; } while (length > 0 && ((unsigned char)viewer.input[length] & 0xc0) == 0x80);
        viewer.input[length] = '\0';
        viewer.input_replace = false;
    }
    bool control=IsKeyDown(KEY_LEFT_CONTROL)||IsKeyDown(KEY_RIGHT_CONTROL);
#if defined(__APPLE__)
    control=control||IsKeyDown(KEY_LEFT_SUPER)||IsKeyDown(KEY_RIGHT_SUPER);
#endif
    if (control && IsKeyPressed(KEY_V)) {
        const char *text = GetClipboardText();
        if (text != NULL) {
            snprintf(viewer.input, sizeof viewer.input, "%s", text);
            viewer.input_replace = false;
        }
    }
    if (control && IsKeyPressed(KEY_A)) viewer.input_replace = true;
    if (back_pressed() || clicked(viewer.layout.cancel)) finish_input("", false);
    else if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER) || clicked(viewer.layout.confirm))
        finish_input(viewer.input, true);
}

static void open_theme_menu(Menu menu)
{
    viewer.menu = viewer.menu == menu ? MENU_NONE : menu;
    viewer.theme_cursor = viewer.theme;
    viewer.layout = layout();
}

static bool theme_menu_open(void)
{
    return viewer.menu == MENU_THEME_TOP || viewer.menu == MENU_THEME_BOTTOM;
}

static void select_theme(int index)
{
    viewer.theme = (Theme)index;
    viewer.menu = MENU_NONE;
    save_settings();
}

static void update_theme_menu(void)
{
    Layout l = viewer.layout;
    for (int i = 0; i < THEME_COUNT; i++) {
        if (clicked(l.themes[i])) {
            select_theme(i);
            return;
        }
    }
    if (IsKeyPressed(KEY_UP)) viewer.theme_cursor = (viewer.theme_cursor + THEME_COUNT - 1) % THEME_COUNT;
    if (IsKeyPressed(KEY_DOWN)) viewer.theme_cursor = (viewer.theme_cursor + 1) % THEME_COUNT;
    if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) select_theme(viewer.theme_cursor);
    else if (back_pressed() ||
        (viewer.pointer.released && !contains(l.theme_popup, viewer.pointer.start))) viewer.menu = MENU_NONE;
}

static bool view_clicked(Rectangle target)
{
    Pointer *p=&viewer.pointer;
    return contains(viewer.layout.popup,p->start)&&contains(viewer.layout.popup,p->point)&&clicked(target);
}

static void update_view_menu(void)
{
    Layout l=viewer.layout;Pointer *p=&viewer.pointer;
    float maximum=maxf(0,425*l.ui-l.popup.height);
    if(IsKeyPressed(KEY_PAGE_DOWN)||IsKeyPressed(KEY_PAGE_UP)||IsKeyPressed(KEY_HOME)||IsKeyPressed(KEY_END)) {
        float offset=IsKeyPressed(KEY_PAGE_DOWN)?l.popup.height*.8f:IsKeyPressed(KEY_PAGE_UP)?-l.popup.height*.8f:0;
        viewer.view_scroll=Clamp(viewer.view_scroll+offset,0,maximum);
        if(IsKeyPressed(KEY_HOME)) viewer.view_scroll=0;
        if(IsKeyPressed(KEY_END)) viewer.view_scroll=maximum;
        viewer.layout=layout();return;
    }
    if(p->down&&contains(l.popup,p->start)&&!contains(l.brightness,p->start)) {
        if(Vector2Distance(p->start,p->point)>5*l.ui) p->moved=true;
        if(p->moved) {viewer.view_scroll=Clamp(viewer.view_scroll-p->delta.y,0,maximum);viewer.layout=layout();return;}
    }
    if(contains(l.popup,p->point)&&viewer.wheel.y!=0) {
        viewer.view_scroll=Clamp(viewer.view_scroll-viewer.wheel.y*60*l.ui,0,maximum);
        viewer.layout=layout();return;
    }
    viewer.view_scroll=Clamp(viewer.view_scroll,0,maximum);
    if(view_clicked(l.view_theme)) {open_theme_menu(MENU_THEME_TOP);return;}
    if((p->down||p->released)&&contains(l.brightness,p->start)&&contains(l.popup,p->point)) {
        viewer.brightness=.4f+.6f*Clamp((p->point.x-l.brightness.x)/l.brightness.width,0,1);
        if(p->released) save_settings();
    }
    if(view_clicked(l.brightness_less)||view_clicked(l.brightness_more)) {
        viewer.brightness=Clamp(viewer.brightness+(view_clicked(l.brightness_more)?.1f:-.1f),.4f,1);
        save_settings();return;
    }
    if(view_clicked(l.crop)) {
        viewer.crop=!viewer.crop;
        if(viewer.pdf) {ReadingPosition anchor=position_at(reading_point());measure_document();restore_position(anchor,reading_point());refresh(0);}
        save_settings();return;
    }
    if(view_clicked(l.focus)&&viewer.pdf) {viewer.focused=true;viewer.menu=MENU_NONE;return;}
    for(int i=0;i<3;i++) if(view_clicked(l.modes[i])) {set_view_mode((ViewMode)i);return;}
    if(view_clicked(l.rotate_left)) {rotate_document(-1);return;}
    if(view_clicked(l.rotate_right)) {rotate_document(1);return;}
    if(view_clicked(l.history_back)) {history_move(-1);return;}
    if(view_clicked(l.history_forward)) {history_move(1);return;}
    if(view_clicked(l.print)) {document_action(PLATFORM_PRINT);return;}
    if(view_clicked(l.share)) {document_action(PLATFORM_SHARE);return;}
    if(view_clicked(l.export_copy)) {document_action(PLATFORM_EXPORT);return;}
    if(back_pressed()||(p->released&&!contains(l.popup,p->start))) viewer.menu=MENU_NONE;
}

typedef struct {
    Rectangle panel, list, tabs[READING_TAB_COUNT], add, previous, next;
    float row_y, row_height;
    size_t slots;
} ReadingLayout;

static size_t reading_item_count(void)
{
    switch (viewer.reading.tab) {
        case READING_CONTENTS: return viewer.reading.visible_count;
        case READING_BOOKMARKS: return viewer.reading.bookmark_count;
        case READING_RECENT: return viewer.reading.session_count;
        case READING_THUMBNAILS: return viewer.pdf?(size_t)viewer.page_count:0;
        default: return 0;
    }
}

static ReadingLayout reading_layout(void)
{
    Layout l = viewer.layout;
    float u = l.ui;
    float margin = 8 * u;
    float width = maxf(1, minf(456 * u, l.width - 2 * margin));
    bool bookmarks = viewer.reading.tab == READING_BOOKMARKS;
    size_t count = reading_item_count();
    float max_height = minf(432 * u, maxf(142 * u, l.body.height - 2 * margin));
    float header = (bookmarks ? 100 : 54) * u;
    float row_height=(viewer.reading.tab==READING_THUMBNAILS?100:viewer.reading.tab==READING_CONTENTS?44:51)*u;
    size_t slots = (size_t)maxf(1, floorf((max_height - header - 39 * u) / row_height));
    size_t rows = count == 0 ? 1 : count < slots ? count : slots;
    float height = minf(max_height, header + rows * row_height + (count > slots ? 39 : 8) * u);
    float x = l.width - width - margin;
    float y = l.body.y + margin;
    if (viewer.reading.context) {
        x = Clamp(viewer.reading.anchor.x, margin, l.width - width - margin);
        y = Clamp(viewer.reading.anchor.y, y, maxf(y, l.body.y + l.body.height - height - margin));
    }
    float inner = width - 2 * margin;
    float tab_width = (inner - 8 * u) / READING_TAB_COUNT;
    float row_y = y + 54 * u;
    Rectangle add = {0};
    if (bookmarks) {
        add = rect(x + margin, row_y, inner, 38 * u);
        row_y += 46 * u;
    }
    float footer = y + height - 39 * u;
    ReadingLayout layout = {
        .panel = rect(x, y, width, height),
        .list = rect(x + margin, row_y, inner, rows * row_height),
        .add = add,
        .previous = rect(x + margin, footer, 38 * u, 32 * u),
        .next = rect(x + width - margin - 38 * u, footer, 38 * u, 32 * u),
        .row_y = row_y, .row_height = row_height,
        .slots = slots
    };
    for (int i = 0; i < READING_TAB_COUNT; i++)
        layout.tabs[i] = rect(x + margin + i * (tab_width + 4 * u), y + margin, tab_width, 39 * u);
    return layout;
}

static void update_visible_contents(void)
{
    ReadingMenu *menu = &viewer.reading;
    menu->visible_count = 0;
    for (size_t i = 0; i < menu->contents.count;) {
        const PdfContentsEntry *entry = &menu->contents.entries[i];
        menu->visible_contents[menu->visible_count++] = i;
        i = entry->expanded ? i + 1 : entry->after;
    }
}

static void load_contents(void)
{
    ReadingMenu *menu = &viewer.reading;
    if (viewer.pdf == NULL || menu->contents_status != CONTENTS_UNREAD) return;
    if (!worker_contents(viewer.worker, &menu->contents)) {
        menu->contents_status = CONTENTS_FAILED;
        return;
    }
    if (menu->contents.count > 0) {
        menu->visible_contents = malloc(menu->contents.count * sizeof *menu->visible_contents);
        if (menu->visible_contents == NULL) {
            pdf_free_contents(&menu->contents);
            menu->contents_status = CONTENTS_FAILED;
            return;
        }
    }
    menu->contents_status = CONTENTS_READY;
    update_visible_contents();
}

static void load_reading_menu(ReadingTab tab)
{
    ReadingMenu *menu = &viewer.reading;
    menu->tab = tab;
    menu->scroll = 0;
    menu->selected = 0;
    menu->scrollbar.active = false;
    if (tab == READING_CONTENTS) {
        load_contents();
    } else if (tab == READING_RECENT) {
        menu->session_count = reading_load_sessions(menu->sessions, RECENT_LIMIT);
    } else if(tab==READING_THUMBNAILS) {
        menu->selected=viewer.page;
        menu->scroll=viewer.page;
    } else {
        free(menu->bookmarks);
        menu->bookmarks = NULL;
        menu->bookmark_count = 0;
        if (viewer.pdf != NULL) {
            menu->bookmark_count = reading_load_bookmarks(pdf_key(viewer.pdf), &menu->bookmarks);
            while (menu->bookmark_count > 0 &&
                menu->bookmarks[menu->bookmark_count - 1].position.location.page >= viewer.page_count)
                menu->bookmark_count--;
        }
    }
}

static void show_reading_menu(ReadingTab tab, bool context)
{
    viewer.menu = MENU_READING;
    viewer.reading.context = context;
    viewer.reading.anchor = GetMousePosition();
    load_reading_menu(tab);
}

static size_t reading_item_end(ReadingLayout layout)
{
    size_t end = (size_t)ceil(viewer.reading.scroll + layout.slots);
    size_t count = reading_item_count();
    return end < count ? end : count;
}

static Rectangle reading_row(ReadingLayout layout, size_t index)
{
    float u = viewer.layout.ui;
    float scrollbar_space = reading_item_count() > layout.slots ? 14 * u : 0;
    return rect(layout.panel.x + 8 * u, layout.row_y + (float)(index - viewer.reading.scroll) * layout.row_height,
                layout.panel.width - 16 * u - scrollbar_space, layout.row_height - 4 * u);
}

static Scrollbar reading_scrollbar(ReadingLayout layout)
{
    float u = viewer.layout.ui;
    Rectangle track = rect(layout.panel.x + layout.panel.width - 20 * u, layout.row_y, 14 * u,
                           layout.row_height * layout.slots - 4 * u);
    return scrollbar_geometry(track, viewer.reading.scroll,
                              (double)reading_item_count(), (double)layout.slots);
}

static PdfContentsEntry *contents_entry(size_t row)
{
    ReadingMenu *menu = &viewer.reading;
    return &menu->contents.entries[menu->visible_contents[row]];
}

static bool contents_branch(size_t row)
{
    return contents_entry(row)->after > viewer.reading.visible_contents[row] + 1;
}

static Rectangle contents_disclosure(Rectangle row, const PdfContentsEntry *entry)
{
    float u = viewer.layout.ui;
    float indent = minf(entry->depth * 13 * u, maxf(0, minf(104 * u, row.width - 180 * u)));
    return rect(row.x + indent, row.y, 32 * u, row.height);
}

static void toggle_contents_branch(size_t row)
{
    if (!contents_branch(row)) return;
    PdfContentsEntry *entry = contents_entry(row);
    entry->expanded = !entry->expanded;
    update_visible_contents();
    viewer.reading.selected = row;
}

static void jump_to_contents(size_t row)
{
    const PdfContentsEntry *entry = contents_entry(row);
    if (entry->page < 0) {
        toggle_contents_branch(row);
        return;
    }

    int index = entry->page;
    ReadingPage *page = &viewer.pages[index];
    PdfBox bounds = reading_bounds(page);
    double y = isfinite(entry->y) ? Clamp((entry->y - bounds.y0) / (bounds.y1 - bounds.y0), 0, 1) : 0;
    double x = 0.5;
    Vector2 anchor = reading_point();
    if (isfinite(entry->x)) {
        x = Clamp((entry->x - bounds.x0) / (bounds.x1 - bounds.x0), 0, 1);
        anchor.x = viewer.layout.body.x + page_margin();
    }
    history_push();
    viewer.page=index;
    restore_position((ReadingPosition){index,x,y},anchor);
    viewer.page=index;
    viewer.menu = MENU_NONE;
    refresh(0);
}

static Rectangle reading_remove(Rectangle row)
{
    float u = viewer.layout.ui;
    return rect(row.x + row.width - 72 * u, row.y + 4 * u, 72 * u, row.height - 8 * u);
}

static Rectangle reading_edit(Rectangle row)
{
    float u = viewer.layout.ui;
    return rect(row.x + row.width - 122 * u, row.y + 4 * u, 46 * u, row.height - 8 * u);
}

static Rectangle reading_item(Rectangle row)
{
    float action_width = viewer.reading.tab == READING_BOOKMARKS ? 126 : 76;
    row.width = maxf(1, row.width - action_width * viewer.layout.ui);
    return row;
}

static const Bookmark *bookmark_for_page(int page)
{
    for (size_t i = 0; i < viewer.reading.bookmark_count; i++) {
        const Bookmark *bookmark = &viewer.reading.bookmarks[i];
        if (bookmark->position.location.page == page) return bookmark;
    }
    return NULL;
}

static void edit_bookmark(SavedPosition position, const Bookmark *bookmark)
{
    char initial[FOLIO_NAME_MAX];
    if (bookmark != NULL) snprintf(initial, sizeof initial, "%s", bookmark->title);
    else snprintf(initial, sizeof initial, "Page %d", position.location.page + 1);

    viewer.pending_bookmark = position;
    viewer.bookmark_document = pdf_key(viewer.pdf);
    viewer.bookmark_edit = bookmark != NULL;
    request_input(INPUT_BOOKMARK, initial);
}

static void bookmark_here(void)
{
    if (viewer.pdf == NULL) return;
    SavedPosition position = current_position();
    edit_bookmark(position, bookmark_for_page(position.location.page));
}

static void choose_new_pdf(void)
{
    viewer.resuming = false;
    viewer.menu = MENU_NONE;
    platform_choose_file();
}

static void resume_session(const RecentSession *session)
{
    viewer.menu = MENU_NONE;
    if (viewer.pdf != NULL && pdf_key(viewer.pdf) == session->document) return;
    viewer.resume_document = session->document;
    viewer.resuming = true;
    if (!platform_reopen_file(session->locator)) {
        set_message("Choose the original PDF to resume this session.");
        platform_choose_file();
    }
}

static void open_reading_item(size_t index)
{
    if(viewer.reading.tab==READING_THUMBNAILS) {
        show_page((int)index);viewer.menu=MENU_NONE;
    } else if (viewer.reading.tab == READING_CONTENTS) {
        jump_to_contents(index);
    } else if (viewer.reading.tab == READING_RECENT) {
        resume_session(&viewer.reading.sessions[index]);
    } else {
        SavedPosition position = viewer.reading.bookmarks[index].position;
        history_push();
        viewer.fit_width=position.fit_width;
        viewer.zoom=position.zoom;
        viewer.view_mode=(ViewMode)position.layout;
        viewer.rotation=position.rotation;
        viewer.page=position.location.page;
        measure_document();
        restore_position(position.location, reading_point());
        viewer.menu = MENU_NONE;
        refresh(0);
    }
}

static void delete_reading_item(size_t index)
{
    ReadingMenu *menu = &viewer.reading;
    bool deleted;
    if (menu->tab == READING_BOOKMARKS) {
        int page = menu->bookmarks[index].position.location.page;
        deleted = reading_delete_bookmark(pdf_key(viewer.pdf), page);
    } else {
        deleted = reading_forget_session(menu->sessions[index].document);
    }
    if (!deleted) {
        set_message(menu->tab == READING_BOOKMARKS ? "Could not remove the bookmark."
                                                 : "Could not remove the recent document.");
        return;
    }
    double scroll = menu->scroll;
    load_reading_menu(menu->tab);
    size_t count = reading_item_count();
    menu->selected = count == 0 ? 0 : index < count ? index : count - 1;
    menu->scroll = scroll;
}

static void clamp_reading_scroll(size_t slots)
{
    ReadingMenu *menu = &viewer.reading;
    size_t count = reading_item_count();
    double maximum = count > slots ? (double)(count - slots) : 0;
    menu->scroll = fmax(0, fmin(maximum, menu->scroll));
    menu->selected = count == 0 ? 0 : menu->selected < count ? menu->selected : count - 1;
}

static void select_reading_item(size_t index, size_t slots)
{
    ReadingMenu *menu = &viewer.reading;
    size_t count = reading_item_count();
    if (count == 0) return;
    menu->selected = index < count ? index : count - 1;
    if (menu->selected < menu->scroll) menu->scroll = (double)menu->selected;
    else if (menu->selected + 1 > menu->scroll + slots) menu->scroll = (double)(menu->selected + 1 - slots);
    clamp_reading_scroll(slots);
}

static void navigate_contents_tree(size_t slots)
{
    ReadingMenu *menu = &viewer.reading;
    size_t row = menu->selected;
    PdfContentsEntry *entry = contents_entry(row);
    if (IsKeyPressed(KEY_RIGHT) && contents_branch(row)) {
        if (!entry->expanded) toggle_contents_branch(row);
        else select_reading_item(row + 1, slots);
    }
    if (IsKeyPressed(KEY_LEFT)) {
        if (entry->expanded && contents_branch(row)) toggle_contents_branch(row);
        else if (entry->parent != SIZE_MAX) {
            size_t parent = row;
            while (menu->visible_contents[parent] > entry->parent) parent--;
            select_reading_item(parent, slots);
        }
    }
}

static void update_reading_menu(void)
{
    ReadingMenu *menu = &viewer.reading;
    ReadingLayout layout = reading_layout();
    if (back_pressed() ||
        (viewer.pointer.released && !contains(layout.panel, viewer.pointer.start))) {
        viewer.menu = MENU_NONE;
        return;
    }
    for (int i = 0; i < READING_TAB_COUNT; i++) {
        if (clicked(layout.tabs[i])) {
            if ((ReadingTab)i != menu->tab) load_reading_menu((ReadingTab)i);
            return;
        }
    }
    if (IsKeyDown(KEY_LEFT_CONTROL)&&IsKeyPressed(KEY_TAB)) {
        load_reading_menu((ReadingTab)((menu->tab + 1) % READING_TAB_COUNT));
        return;
    }
    if (viewer.pdf != NULL && menu->tab == READING_BOOKMARKS && clicked(layout.add)) {
        bookmark_here();
        return;
    }

    size_t count = reading_item_count();
    if (count == 0) return;
    clamp_reading_scroll(layout.slots);
    double offset;
    if (drag_scrollbar(reading_scrollbar(layout), &menu->scrollbar, &offset)) {
        menu->scroll = offset;
        return;
    }

    Pointer *p = &viewer.pointer;
    if (p->down && contains(layout.list, p->start)) {
        if (Vector2Distance(p->start, p->point) > 5 * viewer.layout.ui) p->moved = true;
        if (p->moved) {
            menu->scroll -= p->delta.y / layout.row_height;
            clamp_reading_scroll(layout.slots);
            return;
        }
    }
    float wheel = contains(layout.panel, p->point) ? viewer.wheel.y : 0;
    if (wheel != 0) {
        menu->scroll -= wheel * wheel_scroll_distance * viewer.layout.ui / layout.row_height;
        clamp_reading_scroll(layout.slots);
    }
    size_t end = reading_item_end(layout);
    for (size_t i = (size_t)menu->scroll; i < end; i++) {
        Rectangle row = reading_row(layout, i);
        // Partially visible rows must not receive clicks through the tabs or footer.
        if (!contains(layout.list, p->start) || !contains(layout.list, p->point)) continue;
        if(menu->tab==READING_THUMBNAILS) {
            if(clicked(row)) {open_reading_item(i);return;}
            continue;
        }
        if (menu->tab == READING_CONTENTS) {
            if (contents_branch(i) && clicked(contents_disclosure(row, contents_entry(i)))) {
                toggle_contents_branch(i);
                return;
            }
            if (clicked(row)) {
                open_reading_item(i);
                return;
            }
            continue;
        }
        if (clicked(reading_item(row))) {
            open_reading_item(i);
            return;
        }
        if (clicked(reading_remove(row))) {
            delete_reading_item(i);
            return;
        }
        if (menu->tab == READING_BOOKMARKS && clicked(reading_edit(row))) {
            const Bookmark *bookmark = &menu->bookmarks[i];
            edit_bookmark(bookmark->position, bookmark);
            return;
        }
    }

    if (count > layout.slots && clicked(layout.previous)) {
        menu->scroll -= layout.slots;
        clamp_reading_scroll(layout.slots);
    } else if (clicked(layout.next) && end < count) {
        menu->scroll += layout.slots;
        clamp_reading_scroll(layout.slots);
    }

    if ((IsKeyPressed(KEY_UP) || IsKeyPressedRepeat(KEY_UP)) && menu->selected > 0)
        select_reading_item(menu->selected - 1, layout.slots);
    else if (IsKeyPressed(KEY_DOWN) || IsKeyPressedRepeat(KEY_DOWN))
        select_reading_item(menu->selected + 1, layout.slots);

    if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) open_reading_item(menu->selected);
    else if (menu->tab == READING_CONTENTS) navigate_contents_tree(layout.slots);
    else if(menu->tab!=READING_THUMBNAILS&&IsKeyPressed(KEY_DELETE)) delete_reading_item(menu->selected);
}

static void update_controls(void)
{
    Layout l = viewer.layout;
    if(viewer.menu==MENU_NONE&&viewer.input_kind==INPUT_NONE&&viewer.selection.active&&clicked(selection_copy_box())) {copy_selection();return;}
    if (viewer.input_kind != INPUT_NONE) {
        update_modal();
        return;
    }

    bool control=IsKeyDown(KEY_LEFT_CONTROL)||IsKeyDown(KEY_RIGHT_CONTROL);
#if defined(__APPLE__)
    control=control||IsKeyDown(KEY_LEFT_SUPER)||IsKeyDown(KEY_RIGHT_SUPER);
#endif
    if(viewer.opening&&(clicked(l.loading_cancel)||back_pressed())) {
        worker_close(viewer.opening);viewer.opening=NULL;
        platform_remove_temp(&viewer.opening_file);set_message("Loading canceled.");return;
    }
    if(viewer.pdf&&((control&&IsKeyPressed(KEY_F))||clicked(l.find))) { begin_find();return; }
    if ((control && IsKeyPressed(KEY_O)) || (!viewer.focused && clicked(l.open))) {
        choose_new_pdf();
        return;
    }
#if !defined(__ANDROID__) && !defined(__EMSCRIPTEN__)
    if (control && IsKeyPressed(KEY_L)) {
        viewer.resuming = false;
        request_input(INPUT_PATH, "");
        return;
    }
#endif
    if (control && IsKeyPressed(KEY_B) && viewer.pdf != NULL) {
        load_reading_menu(READING_BOOKMARKS);
        bookmark_here();
        return;
    }
    if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT) && contains(l.body, GetMousePosition())) {
        show_reading_menu(viewer.pdf == NULL ? READING_RECENT : READING_BOOKMARKS, true);
        return;
    }
    bool bookmarks = !control && IsKeyPressed(KEY_B);
    if ((!viewer.focused && clicked(l.reading)) || bookmarks) {
        if (viewer.menu == MENU_READING) viewer.menu = MENU_NONE;
        else show_reading_menu(viewer.pdf == NULL ? READING_RECENT : bookmarks ? READING_BOOKMARKS : viewer.reading.tab,
                               false);
        return;
    }
    if(!control&&IsKeyPressed(KEY_P)) {show_reading_menu(READING_THUMBNAILS,false);return;}
    if (!control && IsKeyPressed(KEY_C)) {
        show_reading_menu(READING_CONTENTS, false);
        return;
    }
    if (!control && IsKeyPressed(KEY_R)) {
        show_reading_menu(READING_RECENT, false);
        return;
    }
    if (!viewer.focused && clicked(l.settings)) {
        viewer.menu = viewer.menu == MENU_VIEW ? MENU_NONE : MENU_VIEW;
        return;
    }
    if ((!viewer.focused && clicked(l.theme)) || IsKeyPressed(KEY_T)) {
        open_theme_menu(MENU_THEME_TOP);
        return;
    }
    if (!viewer.focused && clicked(l.bottom_theme)) {
        open_theme_menu(MENU_THEME_BOTTOM);
        return;
    }
    if (theme_menu_open()) {
        update_theme_menu();
        return;
    }
    if (viewer.menu == MENU_VIEW) {
        update_view_menu();
        return;
    }
    if (viewer.menu == MENU_READING) {
        update_reading_menu();
        return;
    }

#if defined(__ANDROID__)
    if (IsKeyPressed(KEY_BACK)) {
        if(viewer.selection.active) viewer.selection=(TextSelection){0};
        else if(viewer.search_open) search_close();
        else if(viewer.history_cursor>0) history_move(-1);
        else if(viewer.focused) viewer.focused=false;
        else viewer.quit=true;
        return;
    }
#endif

    if(viewer.pdf&&control&&IsKeyPressed(KEY_F)) { begin_find();return; }
    if(viewer.pdf&&control&&IsKeyPressed(KEY_C)) { copy_selection();return; }
    if(viewer.pdf&&control&&IsKeyPressed(KEY_A)) { select_all_text();return; }
    if(viewer.pdf&&control&&IsKeyPressed(KEY_P)) { document_action(PLATFORM_PRINT);return; }
    if(viewer.pdf&&IsKeyPressed(KEY_F3)) { find_next(IsKeyDown(KEY_LEFT_SHIFT)?-1:1);return; }
    if(viewer.pdf&&(IsKeyDown(KEY_LEFT_ALT)||IsKeyDown(KEY_RIGHT_ALT))) {
        if(IsKeyPressed(KEY_LEFT)) { history_move(-1);return; }
        if(IsKeyPressed(KEY_RIGHT)) { history_move(1);return; }
    }
    if(viewer.pdf&&back_pressed()&&viewer.selection.active) { viewer.selection=(TextSelection){0};return; }
    if(viewer.pdf&&viewer.search_open) {
        if(clicked(l.search_field)) { begin_find();return; }
        if(clicked(l.search_prev)) { find_next(-1);return; }
        if(clicked(l.search_next)||IsKeyPressed(KEY_ENTER)) { find_next(1);return; }
        if(clicked(l.search_close)||back_pressed()) { search_close();return; }
    }
    if (viewer.pdf == NULL) {
        Rectangle action = rect(l.width * 0.5f - 82 * l.ui, l.height * 0.5f + 12 * l.ui, 164 * l.ui, 48 * l.ui);
        if (clicked(action)) choose_new_pdf();
        return;
    }

    if(clicked(l.body)&&follow_link_at(viewer.pointer.point)) return;
    if(viewer.selection.active&&clicked(l.body)) { viewer.selection=(TextSelection){0};return; }
    if ((!control&&IsKeyPressed(KEY_F)) || (viewer.focused && back_pressed()) || clicked(l.body)) {
        viewer.focused = !viewer.focused;
        return;
    }

    Vector2 origin = center(l.body);
    if ((!viewer.focused && clicked(l.prev)) || IsKeyPressed(KEY_LEFT) || IsKeyPressed(KEY_PAGE_UP))
        change_page(-1);
    if ((!viewer.focused && clicked(l.next)) || IsKeyPressed(KEY_RIGHT) || IsKeyPressed(KEY_PAGE_DOWN))
        change_page(1);
    if ((!viewer.focused && clicked(l.page)) || (control && IsKeyPressed(KEY_G))) go_to_page();
    if ((!viewer.focused && clicked(l.fit)) || IsKeyPressed(KEY_W)) fit(!viewer.fit_width);
    if (IsKeyPressed(KEY_ZERO)) fit(false);
    if ((!viewer.focused && clicked(l.out)) || IsKeyPressed(KEY_MINUS) || IsKeyPressed(KEY_KP_SUBTRACT))
        zoom_at(1 / 1.25f, origin, origin);
    if ((!viewer.focused && clicked(l.in)) || IsKeyPressed(KEY_EQUAL) || IsKeyPressed(KEY_KP_ADD))
        zoom_at(1.25f, origin, origin);
    if (IsKeyPressed(KEY_UP) || IsKeyPressedRepeat(KEY_UP)) pan_by(0, 36 * l.ui);
    if (IsKeyPressed(KEY_DOWN) || IsKeyPressedRepeat(KEY_DOWN)) pan_by(0, -36 * l.ui);

    Vector2 scroll = viewer.wheel;
    if (contains(l.body, viewer.pointer.point) && (scroll.x != 0 || scroll.y != 0)) {
        if (control) zoom_at(powf(1.18f, scroll.y), viewer.pointer.point, viewer.pointer.point);
        else pan_by(scroll.x * 88 * l.ui, scroll.y * wheel_scroll_distance * l.ui);
    }
}

static void label(const char *text, float x, float y, float size, Color color)
{
    DrawTextEx(viewer.font, text, (Vector2){x, y}, size, 0, color);
}

static void centered(const char *text, Rectangle target, float size, Color color)
{
    Vector2 measurements = MeasureTextEx(viewer.font, text, size, 0);
    label(text, target.x + (target.width - measurements.x) * 0.5f,
          target.y + (target.height - size) * 0.5f, size, color);
}

static void button_label(Rectangle target, const char *text, Color color)
{
    float size = 18 * viewer.layout.ui;
    float text_width = MeasureTextEx(viewer.font, text, size, 0).x;
    float available = target.width - 6 * viewer.layout.ui;
    if (text_width > available && text_width > 0)
        size = maxf(12 * viewer.layout.ui, size * available / text_width);
    centered(text, target, size, color);
}

static void button(Rectangle target, const char *text, bool active, bool enabled)
{
    const Palette *palette = &palettes[viewer.theme];
    register_element(target,text,enabled,active,false);
    Color fill = palette->button;
    if (active) fill = palette->accent;
    else if (enabled && contains(target, viewer.pointer.point)) fill = palette->hover;
    DrawRectangleRounded(target, 0.24f, 6, fill);
    button_label(target, text, active ? palette->accent_ink : enabled ? palette->ink : palette->muted);
}

static void draw_scrollbar(Scrollbar bar, ScrollbarDrag drag)
{
    if (bar.maximum <= 0) return;
    const Palette *palette = &palettes[viewer.theme];
    float u = viewer.layout.ui;
    bool highlighted = drag.active || contains(bar.track, viewer.pointer.point);
    float width = (highlighted ? 8 : 6) * u;
    float x = bar.track.x + (bar.track.width - width) * 0.5f;
    DrawRectangleRounded(rect(x, bar.track.y, width, bar.track.height), 1, 8, Fade(palette->line, 0.80f));
    DrawRectangleRounded(rect(x, bar.thumb.y, width, bar.thumb.height), 1, 8,
                         highlighted ? palette->accent : Fade(palette->muted, 0.73f));
}

static void draw_texture(PageTexture layer, PdfBox crop, Rectangle page, float scale)
{
    if (layer.texture.id == 0) return;
    PdfBox area = {
        maxf(crop.x0, layer.area.x0), maxf(crop.y0, layer.area.y0),
        minf(crop.x1, layer.area.x1), minf(crop.y1, layer.area.y1)
    };
    float width = area.x1 - area.x0;
    float height = area.y1 - area.y0;
    if (width <= 0 || height <= 0) return;
    Rectangle source = rect((area.x0 - layer.area.x0) * layer.scale, (area.y0 - layer.area.y0) * layer.scale,
                            width * layer.scale, height * layer.scale);
    float x=area.x0-crop.x0,y=area.y0-crop.y0;
    float w=crop.x1-crop.x0,h=crop.y1-crop.y0;
    Vector2 anchor={x,y};
    if(viewer.rotation==1) anchor=(Vector2){h-y,x};
    else if(viewer.rotation==2) anchor=(Vector2){w-x,h-y};
    else if(viewer.rotation==3) anchor=(Vector2){y,w-x};
    Rectangle target=rect(page.x+anchor.x*scale,page.y+anchor.y*scale,width*scale,height*scale);
    DrawTexturePro(layer.texture,source,target,(Vector2){0},90.0f*viewer.rotation,WHITE);
}

static void draw_document(void)
{
    const Palette *palette = &palettes[viewer.theme];
    Rectangle body = viewer.layout.body;
    int first = page_at(viewer.scroll);
    int end = visible_end(first);
    BeginScissorMode((int)body.x, (int)body.y, (int)body.width, (int)body.height);
    for (int i = first; i < end; i++) {
        Rectangle page = page_rect(i);
        DrawRectangleRec(rect(page.x - 1, page.y + 2, page.width + 2, page.height + 2),
                         Fade(BLACK, 0.10f));
        DrawRectangleRec(page, ColorBrightness(palette->paper, (viewer.brightness - 1) * 0.8f));
    }
    int theme = viewer.theme;
    float paper[] = {palette->paper.r / 255.0f, palette->paper.g / 255.0f, palette->paper.b / 255.0f};
    float ink[] = {palette->page_ink.r / 255.0f, palette->page_ink.g / 255.0f, palette->page_ink.b / 255.0f};
    SetShaderValue(viewer.shader, viewer.shader_theme, &theme, SHADER_UNIFORM_INT);
    SetShaderValue(viewer.shader, viewer.shader_brightness, &viewer.brightness, SHADER_UNIFORM_FLOAT);
    SetShaderValue(viewer.shader, viewer.shader_paper, paper, SHADER_UNIFORM_VEC3);
    SetShaderValue(viewer.shader, viewer.shader_ink, ink, SHADER_UNIFORM_VEC3);
    BeginShaderMode(viewer.shader);
    for (int i = first; i < end; i++) {
        ReadingPage *item = &viewer.pages[i];
        PdfBox crop = reading_bounds(item);
        Rectangle page = page_rect(i);
        float scale = item->scale * viewer.zoom;
        draw_texture(item->preview, crop, page, scale);
        draw_texture(item->detail, crop, page, scale);
    }
    EndShaderMode();
    EndScissorMode();
}

static void draw_empty(void)
{
    Layout l = viewer.layout;
    const Palette *palette = &palettes[viewer.theme];
    Vector2 position = {l.width * 0.5f, l.height * 0.5f};
    float u = l.ui;
    Rectangle icon = rect(position.x - 22 * u, position.y - 100 * u, 44 * u, 56 * u);
    DrawRectangleRounded(icon, 0.18f, 6, palette->panel);
    DrawRectangleLinesEx(icon, 1 * u, palette->line);
    for (int i = 0; i < 3; i++)
        DrawRectangleRec(rect(icon.x + 10 * u, icon.y + (18 + 8 * i) * u,
                              (i == 2 ? 15 : 24) * u, 2 * u), palette->accent);
    centered("Open a PDF", rect(0, position.y - 38 * u, l.width, 30 * u), 23 * u, palette->ink);
    button(rect(position.x - 82 * u, position.y + 12 * u, 164 * u, 48 * u), "Choose file", true, true);
    centered(platform_pick_hint(), rect(0, position.y + 70 * u, l.width, 24 * u), 13 * u, palette->muted);
}

static void draw_toolbar(void)
{
    Layout l = viewer.layout;
    const Palette *palette = &palettes[viewer.theme];
    DrawRectangleRec(rect(0, 0, l.width, l.bar), palette->panel);
    DrawRectangleRec(rect(0, l.bar - 1, l.width, 1), palette->line);
    DrawRectangleRec(rect(0, l.height - l.bar, l.width, l.bar), palette->panel);
    DrawRectangleRec(rect(0, l.height - l.bar, l.width, 1), palette->line);

    button(l.open,l.width<560*l.ui?"Open":"Open PDF",true,true);
    button(l.find,"Find",viewer.search_open,viewer.pdf!=NULL);
    button(l.reading, "Reading", viewer.menu == MENU_READING, true);
    bool top_theme = viewer.menu == MENU_THEME_TOP;
    button(l.theme, "", top_theme, true);
    float x = l.theme.x + l.theme.width - 12 * l.ui;
    float y = l.theme.y + l.theme.height * 0.5f;
    Color arrow = top_theme ? palette->accent_ink : palette->ink;
    Rectangle theme_label = rect(l.theme.x + 3 * l.ui, l.theme.y, l.theme.width - 18 * l.ui, l.theme.height);
    button_label(theme_label, "Theme", arrow);
    DrawLineEx((Vector2){x - 3 * l.ui, y - l.ui}, (Vector2){x, y + 2 * l.ui}, l.ui, arrow);
    DrawLineEx((Vector2){x, y + 2 * l.ui}, (Vector2){x + 3 * l.ui, y - l.ui}, l.ui, arrow);
    button(l.settings, "View", viewer.menu == MENU_VIEW, true);
    if (l.title.width > 25 * l.ui) {
        BeginScissorMode((int)l.title.x, (int)l.title.y, (int)l.title.width, (int)l.title.height);
        label(viewer.pdf == NULL ? "Folio" : viewer.title, l.title.x, l.title.y + 12 * l.ui,
              17 * l.ui, palette->ink);
        EndScissorMode();
    }
    bool bottom_theme = viewer.menu == MENU_THEME_BOTTOM;
    button(l.bottom_theme, "", bottom_theme, true);
    Vector2 theme_icon = center(l.bottom_theme);
    Color glyph = bottom_theme ? palette->accent_ink : palette->ink;
    DrawCircleSector(theme_icon, 6 * l.ui, 90, 270, 16, glyph);
    DrawCircleLinesV(theme_icon, 7 * l.ui, glyph);

    if (viewer.pdf == NULL) return;
    char pages[48];
    int count = pdf_page_count(viewer.pdf);
    int first=viewer.view_mode==VIEW_SPREAD?viewer.page/2*2:viewer.page;
    if(viewer.view_mode==VIEW_SPREAD&&first+1<count)
        snprintf(pages,sizeof pages,"%d-%d / %d",first+1,first+2,count);
    else snprintf(pages, sizeof pages, "%d / %d", first + 1, count);
    button(l.prev, "<", false, adjacent_page(-1)>=0);
    button(l.page, pages, false, true);
    button(l.next, ">", false, adjacent_page(1)<count);
    button(l.out, "-", false, true);
    button(l.fit, viewer.fit_width ? "Fit width" : "Fit page", false, true);
    button(l.in, "+", false, true);
}

static void draw_theme_menu(void)
{
    Layout l = viewer.layout;
    const Palette *palette = &palettes[viewer.theme];
    float u = l.ui;
    DrawRectangleRounded(l.theme_popup, 0.12f, 6, palette->panel);
    DrawRectangleLinesEx(l.theme_popup, 1, palette->line);
    for (int i = 0; i < THEME_COUNT; i++) {
        Rectangle row = l.themes[i];
        bool highlighted = i == viewer.theme_cursor || contains(row, viewer.pointer.point);
        if (highlighted) DrawRectangleRounded(row, 0.2f, 6, palette->hover);
        Rectangle swatch = rect(row.x + 10 * u, row.y + 13 * u, 16 * u, 16 * u);
        DrawRectangleRec(swatch, palettes[i].paper);
        DrawRectangleLinesEx(swatch, u, palettes[i].ink);
        label(theme_names[i], row.x + 34 * u, row.y + 12 * u, 17 * u, palette->ink);
        if (i == (int)viewer.theme) {
            float x = row.x + row.width - 14 * u;
            float y = row.y + row.height * 0.5f;
            DrawLineEx((Vector2){x - 5 * u, y}, (Vector2){x - 1 * u, y + 4 * u}, 1.7f * u, palette->accent);
            DrawLineEx((Vector2){x - 1 * u, y + 4 * u}, (Vector2){x + 5 * u, y - 4 * u}, 1.7f * u, palette->accent);
        }
    }
}

static void draw_settings(void)
{
    Layout l=viewer.layout;float u=l.ui;
    const Palette *p=&palettes[viewer.theme];
    DrawRectangleRounded(l.popup,.05f,6,p->panel);
    DrawRectangleLinesEx(l.popup,1,p->line);
    BeginScissorMode((int)l.popup.x,(int)l.popup.y,(int)l.popup.width,(int)l.popup.height);
    char theme[64];snprintf(theme,sizeof theme,"Theme   %s   >",theme_names[viewer.theme]);
    button(l.view_theme,theme,false,true);
    label("Page brightness",l.brightness.x,l.brightness.y-16*u,14*u,p->muted);
    button(l.brightness_less,"-",false,viewer.brightness>.4f);
    button(l.brightness_more,"+",false,viewer.brightness<1);
    float x=l.brightness.x+6*u,y=l.brightness.y+l.brightness.height*.5f,width=l.brightness.width-12*u;
    float active=width*(viewer.brightness-.4f)/.6f;
    DrawRectangleRec(rect(x,y-2*u,width,4*u),p->line);
    DrawRectangleRec(rect(x,y-2*u,active,4*u),p->accent);
    DrawCircleV((Vector2){x+active,y},7*u,p->accent);
    button(l.crop,viewer.crop?"Crop margins   On":"Crop margins   Off",viewer.crop,viewer.pdf!=NULL);
    button(l.focus,"Hide toolbar",false,viewer.pdf!=NULL);
    const char *modes[]={"Continuous","Single page","Two pages"};
    for(int i=0;i<3;i++) button(l.modes[i],modes[i],(int)viewer.view_mode==i,viewer.pdf!=NULL);
    button(l.rotate_left,"Rotate left",false,viewer.pdf!=NULL);
    button(l.rotate_right,"Rotate right",false,viewer.pdf!=NULL);
    button(l.history_back,"Back",false,viewer.history_cursor>0);
    button(l.history_forward,"Forward",false,viewer.history_cursor+1<viewer.history_count);
    button(l.print,"Print",false,viewer.pdf&&pdf_can_print(viewer.pdf));
    button(l.share,"Share",false,viewer.pdf!=NULL);
    button(l.export_copy,"Save a copy",false,viewer.pdf!=NULL);
    EndScissorMode();
}

static void clip_reading_row(ReadingLayout layout, Rectangle area)
{
    Rectangle clip = GetCollisionRec(layout.list, area);
    BeginScissorMode((int)ceilf(clip.x), (int)ceilf(clip.y),
                     (int)maxf(0, floorf(clip.x + clip.width) - ceilf(clip.x)),
                     (int)maxf(0, floorf(clip.y + clip.height) - ceilf(clip.y)));
}

static void draw_contents_row(ReadingLayout layout, Rectangle row, size_t index)
{
    const Palette *palette = &palettes[viewer.theme];
    const PdfContentsEntry *entry = contents_entry(index);
    char accessible[320];
    snprintf(accessible,sizeof accessible,"%s, page %d",entry->title,entry->page+1);
    register_element(GetCollisionRec(layout.list,row),accessible,true,index==viewer.reading.selected,false);
    Rectangle disclosure = contents_disclosure(row, entry);
    float u = viewer.layout.ui;
    bool selected = index == viewer.reading.selected || contains(row, viewer.pointer.point);
    DrawRectangleRounded(row, 0.18f, 6, selected ? palette->hover : palette->button);
    if (contents_branch(index)) {
        snprintf(accessible,sizeof accessible,"%s %s",entry->expanded?"Collapse":"Expand",entry->title);
        register_element(GetCollisionRec(layout.list,disclosure),accessible,true,entry->expanded,false);
        Vector2 point = center(disclosure);
        if (entry->expanded) {
            DrawLineEx((Vector2){point.x - 4 * u, point.y - 2 * u}, (Vector2){point.x, point.y + 2 * u}, 1.6f * u, palette->ink);
            DrawLineEx((Vector2){point.x, point.y + 2 * u}, (Vector2){point.x + 4 * u, point.y - 2 * u}, 1.6f * u, palette->ink);
        } else {
            DrawLineEx((Vector2){point.x - 2 * u, point.y - 4 * u}, (Vector2){point.x + 2 * u, point.y}, 1.6f * u, palette->ink);
            DrawLineEx((Vector2){point.x + 2 * u, point.y}, (Vector2){point.x - 2 * u, point.y + 4 * u}, 1.6f * u, palette->ink);
        }
    }
    float title_x = disclosure.x + disclosure.width + 2 * u;
    float end = row.x + row.width - 8 * u;
    if (entry->page >= 0) {
        char number[32];
        snprintf(number, sizeof number, "Page %d", entry->page + 1);
        float size = 14 * u;
        float width = MeasureTextEx(viewer.font, number, size, 0).x;
        label(number, end - width, row.y + (row.height - size) * 0.5f, size, palette->muted);
        end -= width + 12 * u;
    }
    clip_reading_row(layout, rect(title_x, row.y, maxf(1, end - title_x), row.height));
    label(entry->title, title_x, row.y + (row.height - 17 * u) * 0.5f, 17 * u,
          entry->page >= 0 || contents_branch(index) ? palette->ink : palette->muted);
    EndScissorMode();
}

static const char *reading_empty_message(void)
{
    switch (viewer.reading.tab) {
        case READING_RECENT: return "No recent PDFs";
        case READING_BOOKMARKS: return viewer.pdf == NULL ? "Open a PDF to add bookmarks" : "No bookmarks in this PDF";
        case READING_CONTENTS:
            if (viewer.pdf == NULL) return "Open a PDF to view its contents";
            return viewer.reading.contents_status == CONTENTS_FAILED ? "Could not read the PDF outline"
                                                                     : "This PDF has no embedded contents";
        default: return "";
    }
}

static void draw_reading_menu(void)
{
    const Palette *palette = &palettes[viewer.theme];
    ReadingMenu *menu = &viewer.reading;
    ReadingLayout layout = reading_layout();
    float u = viewer.layout.ui;
    DrawRectangleRounded(layout.panel, 0.08f, 6, palette->panel);
    DrawRectangleLinesEx(layout.panel, 1, palette->line);
    const char *tabs[]={"Contents","Bookmarks","Recent","Pages"};
    for (int i = 0; i < READING_TAB_COUNT; i++)
        button(layout.tabs[i], tabs[i], menu->tab == (ReadingTab)i, true);
    if (menu->tab == READING_BOOKMARKS && viewer.pdf != NULL) {
        int page = position_at(reading_point()).page;
        button(layout.add, bookmark_for_page(page) != NULL ? "Update bookmark" : "Bookmark this page", false, true);
    }

    size_t count = reading_item_count();
    if (count == 0) {
        centered(reading_empty_message(), rect(layout.panel.x, layout.row_y + 8 * u, layout.panel.width, 28 * u),
                 16 * u, palette->muted);
        return;
    }

    size_t end = reading_item_end(layout);
    for (size_t i = (size_t)menu->scroll; i < end; i++) {
        Rectangle row = reading_row(layout, i);
        clip_reading_row(layout, row);
        if(menu->tab==READING_THUMBNAILS) {
            draw_thumbnail_row(row,i);EndScissorMode();continue;
        }
        if (menu->tab == READING_CONTENTS) {
            draw_contents_row(layout, row, i);
            continue;
        }
        Rectangle item = reading_item(row);
        const char *title;
        int page;
        if (menu->tab == READING_BOOKMARKS) {
            title = menu->bookmarks[i].title;
            page = menu->bookmarks[i].position.location.page;
        } else {
            title = menu->sessions[i].title;
            page = menu->sessions[i].page;
        }

        bool selected = i == menu->selected || contains(item, viewer.pointer.point);
        DrawRectangleRounded(item, 0.20f, 6, selected ? palette->hover : palette->button);
        char number[48];
        snprintf(number, sizeof number, "Page %d", page + 1);
        float inset = 10 * u;
        clip_reading_row(layout, rect(item.x + inset, item.y, maxf(1, item.width - 2 * inset), item.height));
        label(title, item.x + inset, item.y + 4 * u, 17 * u, palette->ink);
        label(number, item.x + inset, item.y + 26 * u, 14 * u, palette->muted);
        EndScissorMode();
        clip_reading_row(layout, row);
        if (menu->tab == READING_BOOKMARKS) button(reading_edit(row), "Edit", false, true);
        button(reading_remove(row), menu->tab == READING_BOOKMARKS ? "Remove" : "Forget", false, true);
        EndScissorMode();
    }

    if (count > layout.slots) {
        draw_scrollbar(reading_scrollbar(layout), menu->scrollbar);
        button(layout.previous, "<", false, menu->scroll > 0);
        button(layout.next, ">", false, end < count);
        char range[64];
        snprintf(range, sizeof range, "%zu-%zu of %zu", (size_t)menu->scroll + 1, end, count);
        centered(range, rect(layout.panel.x, layout.previous.y, layout.panel.width, layout.previous.height),
                 14 * u, palette->muted);
    }
}

static void draw_modal(void)
{
    if(viewer.system_input) return;
    Layout l = viewer.layout;
    const Palette *palette = &palettes[viewer.theme];
    DrawRectangleRec(rect(0, 0, l.width, l.height), Fade(BLACK, 0.4f));
    if (viewer.system_input) return;
    DrawRectangleRounded(l.dialog, 0.08f, 6, palette->panel);
    label(input_label(viewer.input_kind), l.dialog.x + 16 * l.ui, l.dialog.y + 17 * l.ui,
          20 * l.ui, palette->ink);
    DrawRectangleRec(l.field, palette->button);
    DrawRectangleLinesEx(l.field, 1, palette->accent);
    char masked[FOLIO_NAME_MAX];
    const char *text = viewer.input;
    if (viewer.input_kind == INPUT_PASSWORD) {
        size_t count = strlen(text);
        if (count >= sizeof masked) count = sizeof masked - 1;
        memset(masked, '*', count);
        masked[count] = '\0';
        text = masked;
    }
    float size = 16 * l.ui;
    float text_width = MeasureTextEx(viewer.font, text, size, 0).x;
    float x = l.field.x + 8 * l.ui - maxf(0, text_width - l.field.width + 16 * l.ui);
    BeginScissorMode((int)l.field.x + 2, (int)l.field.y + 2, (int)l.field.width - 4, (int)l.field.height - 4);
    if (viewer.input_replace)
        DrawRectangleRec(rect(x - 1, l.field.y + 9 * l.ui, text_width + 2, 24 * l.ui), palette->accent);
    label(text, x, l.field.y + 12 * l.ui, size, viewer.input_replace ? palette->accent_ink : palette->ink);
    if (!viewer.input_replace && ((int)(GetTime() * 2)) % 2 == 0)
        DrawRectangleRec(rect(x + text_width + 1, l.field.y + 11 * l.ui, 1, 20 * l.ui), palette->ink);
    EndScissorMode();
    button(l.cancel, "Cancel", false, true);
    button(l.confirm, "OK", true, true);
}

static void draw_message(void)
{
    if (GetTime() >= viewer.message_until) return;
    Layout l = viewer.layout;
    const Palette *palette = &palettes[viewer.theme];
    DrawRectangleRounded(l.status, 0.22f, 6, palette->panel);
    DrawRectangleLinesEx(l.status, 1, palette->line);
    BeginScissorMode((int)l.status.x + 6, (int)l.status.y, (int)l.status.width - 12, (int)l.status.height);
    label(viewer.message, l.status.x + 10 * l.ui, l.status.y + 11 * l.ui, 14 * l.ui, palette->ink);
    EndScissorMode();
}

#include "reader_features.inc"

static void frame(void)
{
    if(!platform_frame_ready()) {PollInputEvents();return;}
    update_layout();
    char notice[320];if(platform_poll_notice(notice,sizeof notice)) set_message(notice);
    update_loading();
    if(viewer.worker) { worker_tick(viewer.worker);update_document_work();update_search(); }
    FileEvent file;
    if (platform_poll_file(&file)) open_pdf(&file);
    TextEvent text;
    bool completed_text = platform_poll_text(&text) && viewer.input_kind != INPUT_NONE;
    if (completed_text) finish_input(text.value, text.accepted);
#if !defined(__ANDROID__) && !defined(__EMSCRIPTEN__)
    if (IsFileDropped()) {
        FilePathList paths = LoadDroppedFiles();
        if (paths.count > 0 && strlen(paths.paths[0]) < sizeof file.path) {
            file = (FileEvent){0};
            viewer.resuming = false;
            snprintf(file.path, sizeof file.path, "%s", paths.paths[0]);
            open_pdf(&file);
        }
        UnloadDroppedFiles(paths);
    }
#endif

    pointer_update();
    update_commands();
    update_keyboard_focus();
    if (!completed_text) update_controls();
    if (viewer.pdf != NULL) update_visible_pages();
    if (viewer.save_due >= 0 && (GetTime() >= viewer.save_due || !IsWindowFocused())) {
        save_reading();
        viewer.save_due = -1;
    }

    viewer.element_count=0;
    BeginDrawing();
    ClearBackground(palettes[viewer.theme].background);
    if (viewer.pdf == NULL) draw_empty();
    else {
        draw_document();
        draw_scrollbar(document_scrollbar(), viewer.scrollbar);
    }
    if (!viewer.focused) draw_toolbar();
    if(viewer.search_open) draw_search();
    if(viewer.pdf) draw_interaction();
    viewer.building_menu=true;
    if (viewer.menu == MENU_VIEW) draw_settings();
    if (theme_menu_open()) draw_theme_menu();
    if (viewer.menu == MENU_READING) draw_reading_menu();
    if (viewer.input_kind != INPUT_NONE) draw_modal();
    viewer.building_menu=false;
    draw_message();
    if(viewer.opening) draw_loading();
    draw_focus();
    publish_accessibility();
    EndDrawing();
}

#if defined(__ANDROID__) || defined(__EMSCRIPTEN__)
#define SHADER_HEAD "#version 100\nprecision mediump float;\nvarying vec2 fragTexCoord;\n"
#define SHADER_SAMPLE "texture2D"
#define SHADER_OUT "gl_FragColor"
#else
#define SHADER_HEAD "#version 330\nin vec2 fragTexCoord;\nout vec4 finalColor;\n"
#define SHADER_SAMPLE "texture"
#define SHADER_OUT "finalColor"
#endif

static const char *page_shader =
    SHADER_HEAD
    "uniform sampler2D texture0;\n"
    "uniform int readingTheme;\n"
    "uniform float brightness;\n"
    "uniform vec3 paperColor;\n"
    "uniform vec3 inkColor;\n"
    "void main() {\n"
    "  vec3 ink = " SHADER_SAMPLE "(texture0, fragTexCoord).rgb;\n"
    "  float l = dot(ink, vec3(0.2126, 0.7152, 0.0722));\n"
    "  float sat = max(max(ink.r, ink.g), ink.b) - min(min(ink.r, ink.g), ink.b);\n"
    "  float color = smoothstep(0.04, 0.4, sat);\n"
    "  vec3 tinted = ink;\n"
    "  if (readingTheme != 0) {\n"
    "    tinted = mix(inkColor, paperColor, l);\n"
    "    bool night = dot(paperColor, vec3(0.2126, 0.7152, 0.0722)) <\n"
    "                 dot(inkColor, vec3(0.2126, 0.7152, 0.0722));\n"
    "    vec3 illustration = night ? mix(ink, vec3(l), 0.16) : ink;\n"
    "    tinted = mix(tinted, illustration, color * (night ? 0.72 : 0.75));\n"
    "  }\n"
    "  " SHADER_OUT " = vec4(tinted * brightness, 1.0);\n"
    "}\n";

int main(int argc, char **argv)
{
    viewer = (Viewer){0};
    viewer.zoom = 1;
    viewer.brightness = 1;
    viewer.fit_width = true;
    viewer.render_due = -1;
    viewer.save_due = -1;
    SetTraceLogLevel(LOG_WARNING);
    platform_preinit();
#if defined(__ANDROID__)
    InitWindow(0, 0, "Folio");
#elif defined(__EMSCRIPTEN__)
    InitWindow((int)round(web_width() * web_density()), (int)round(web_height() * web_density()), "Folio");
#else
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_WINDOW_HIGHDPI | FLAG_VSYNC_HINT);
    InitWindow(1050, 760, "Folio PDF Viewer");
    SetWindowMinSize(350, 370);
#endif
    if (!IsWindowReady()) return 1;
    SetExitKey(KEY_NULL);
    SetTargetFPS(60);

    platform_init(argc, argv);
    load_settings();
    viewer.layout = layout();
    int codepoints[191];
    for (int i = 0; i < 95; i++) codepoints[i] = i + 32;
    for (int i = 95; i < 191; i++) codepoints[i] = i + 65;
    viewer.font = LoadFontFromMemory(".otf", folio_font, folio_font_size, 52, codepoints, 191);
    if (!IsFontValid(viewer.font)) viewer.font = GetFontDefault();
    else SetTextureFilter(viewer.font.texture, TEXTURE_FILTER_BILINEAR);
    viewer.shader = LoadShaderFromMemory(NULL, page_shader);
    viewer.shader_theme = GetShaderLocation(viewer.shader, "readingTheme");
    viewer.shader_brightness = GetShaderLocation(viewer.shader, "brightness");
    viewer.shader_paper = GetShaderLocation(viewer.shader, "paperColor");
    viewer.shader_ink = GetShaderLocation(viewer.shader, "inkColor");

#ifdef __EMSCRIPTEN__
    emscripten_set_main_loop(frame, 0, true);
#else
    while (!viewer.quit && !WindowShouldClose()) frame();
    save_reading();
    close_document();
    free(viewer.reading.bookmarks);
    UnloadShader(viewer.shader);
    if (viewer.font.texture.id != GetFontDefault().texture.id) UnloadFont(viewer.font);
    worker_close(viewer.opening);
    platform_remove_temp(&viewer.opening_file);
    platform_shutdown();
    CloseWindow();
#endif
    return 0;
}
