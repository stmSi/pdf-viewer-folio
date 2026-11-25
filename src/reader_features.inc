/* Reader interactions share the same PDF coordinates at every zoom and rotation. */
static Vector2 pdf_to_screen(int index,Vector2 point)
{
    ReadingPage *item=&viewer.pages[index];
    PdfBox crop=reading_bounds(item);
    float x=point.x-crop.x0,y=point.y-crop.y0;
    float w=crop.x1-crop.x0,h=crop.y1-crop.y0;
    Vector2 p={x,y};
    if(viewer.rotation==1) p=(Vector2){h-y,x};
    else if(viewer.rotation==2) p=(Vector2){w-x,h-y};
    else if(viewer.rotation==3) p=(Vector2){y,w-x};
    Rectangle page=page_rect(index);
    return (Vector2){page.x+p.x*item->scale*viewer.zoom,page.y+p.y*item->scale*viewer.zoom};
}

static Vector2 screen_to_pdf(int index,Vector2 point)
{
    ReadingPage *item=&viewer.pages[index];
    PdfBox crop=reading_bounds(item);
    Rectangle page=page_rect(index);
    float scale=item->scale*viewer.zoom;
    float x=(point.x-page.x)/scale,y=(point.y-page.y)/scale;
    float w=crop.x1-crop.x0,h=crop.y1-crop.y0;
    Vector2 p={x,y};
    if(viewer.rotation==1) p=(Vector2){y,h-x};
    else if(viewer.rotation==2) p=(Vector2){w-x,h-y};
    else if(viewer.rotation==3) p=(Vector2){w-y,x};
    return (Vector2){p.x+crop.x0,p.y+crop.y0};
}

static Rectangle pdf_box_screen(int page,PdfBox box)
{
    Vector2 a=pdf_to_screen(page,(Vector2){box.x0,box.y0});
    Vector2 b=pdf_to_screen(page,(Vector2){box.x1,box.y1});
    return rect(minf(a.x,b.x),minf(a.y,b.y),fabsf(a.x-b.x),fabsf(a.y-b.y));
}

static void restore_pdf_point(int page,Vector2 source,Vector2 target)
{
    Vector2 current=pdf_to_screen(page,source);
    viewer.pan+=target.x-current.x;
    viewer.scroll+=current.y-target.y;
    clamp_view();
    viewer.page=page;
}

static void history_push(void)
{
    if(!viewer.pdf||viewer.history_restoring) return;
    SavedPosition position=current_position();
    if(viewer.history_cursor>0) {
        SavedPosition old=viewer.history[viewer.history_cursor-1];
        if(old.location.page==position.location.page&&fabs(old.location.y-position.location.y)<.00001&&
           old.rotation==position.rotation&&old.layout==position.layout&&fabsf(old.zoom-position.zoom)<.0001) return;
    }
    if(viewer.history_cursor==HISTORY_LIMIT) {
        memmove(viewer.history,viewer.history+1,(HISTORY_LIMIT-1)*sizeof *viewer.history);
        viewer.history_cursor--;
    }
    viewer.history[viewer.history_cursor++]=position;
    viewer.history_count=viewer.history_cursor;
}

static void history_move(int direction)
{
    if(!viewer.pdf) return;
    if(direction<0) {
        if(viewer.history_cursor==0) return;
        if(viewer.history_cursor==viewer.history_count) {
            SavedPosition here=current_position();
            if(viewer.history_count==HISTORY_LIMIT) {
                memmove(viewer.history,viewer.history+1,(HISTORY_LIMIT-1)*sizeof *viewer.history);
                viewer.history_count--;viewer.history_cursor--;
            }
            viewer.history[viewer.history_count++]=here;
        }
        viewer.history_cursor--;
    } else {
        if(viewer.history_cursor+1>=viewer.history_count) return;
        viewer.history_cursor++;
    }
    SavedPosition position=viewer.history[viewer.history_cursor];
    viewer.history_restoring=true;
    viewer.view_mode=(ViewMode)position.layout;viewer.rotation=position.rotation;
    viewer.zoom=position.zoom;viewer.fit_width=position.fit_width;
    viewer.page=position.location.page;
    measure_document();restore_position(position.location,reading_point());
    viewer.page=position.location.page;
    viewer.selection=(TextSelection){0};viewer.menu=MENU_NONE;
    viewer.history_restoring=false;refresh(0);
}

static void set_view_mode(ViewMode mode)
{
    if(!viewer.pdf) return;
    int page=viewer.page;
    history_push();viewer.view_mode=mode;viewer.zoom=1;
    viewer.fit_width=mode==VIEW_CONTINUOUS;
    measure_document();
    viewer.history_restoring=true;show_page(page);viewer.history_restoring=false;
    viewer.menu=MENU_NONE;
}

static void rotate_document(int direction)
{
    if(!viewer.pdf) return;
    int page=viewer.page;
    history_push();viewer.rotation=(viewer.rotation+direction+4)%4;
    measure_document();viewer.history_restoring=true;show_page(page);viewer.history_restoring=false;
    viewer.menu=MENU_NONE;
}

static void update_loading(void)
{
    if(!viewer.opening) return;
    worker_tick(viewer.opening);
    char error[256]={0};
    WorkerStatus status=worker_status(viewer.opening,&viewer.opening_done,&viewer.opening_total,error,sizeof error);
    if(status==WORKER_PASSWORD&&!viewer.password_requested) {
        viewer.password_requested=true;
        if(*error) set_message(error);
        request_input(INPUT_PASSWORD,"");
    } else if(status==WORKER_READY) {
        PdfWorker *ready=viewer.opening;
        FileEvent file=viewer.opening_file;
        char password[256];snprintf(password,sizeof password,"%s",viewer.opening_password);
        viewer.opening=NULL;viewer.opening_file=(FileEvent){0};
        memset(viewer.opening_password,0,sizeof viewer.opening_password);
        viewer.input_kind=INPUT_NONE;viewer.system_input=false;
        if(viewer.resuming&&pdf_key(worker_document(ready))!=viewer.resume_document) {
            worker_close(ready);platform_remove_temp(&file);
            set_message("Choose the same PDF to resume this session.");
        } else {
            use_document(ready,&file);
            snprintf(viewer.password,sizeof viewer.password,"%s",password);
        }
        memset(password,0,sizeof password);viewer.resuming=false;
    } else if(status==WORKER_FAILED) {
        worker_close(viewer.opening);viewer.opening=NULL;
        platform_remove_temp(&viewer.opening_file);viewer.opening_file=(FileEvent){0};
        viewer.input_kind=INPUT_NONE;viewer.system_input=false;viewer.resuming=false;
        set_message(*error?error:"Could not open this PDF.");
    }
}

static void update_document_work(void)
{
    int anchor_page=viewer.page;
    Vector2 anchor=reading_point(),source=screen_to_pdf(anchor_page,anchor);
    bool cropped=false;
    for(PdfResult *result=worker_poll(viewer.worker);result;result=worker_poll(viewer.worker)) {
        int index=result->request.page;
        if(index>=0&&index<viewer.page_count) {
            ReadingPage *page=&viewer.pages[index];
            WorkKind kind=result->request.kind;
            bool visible=index>=viewer.cache_start&&index<viewer.cache_end;
            if(kind==WORK_TEXT) {
                page->text_pending=false;
                if(visible||(viewer.selection.active&&viewer.selection.page==index)) {
                    pdf_text_free(page->text);page->text=result->text;result->text=NULL;
                    page->text_failed=page->text==NULL;
                }
            } else if(kind==WORK_THUMBNAIL) {
                page->thumb_pending=false;
                if(index>=viewer.thumb_start&&index<viewer.thumb_end&&result->image.pixels)
                    upload_texture(&page->thumbnail,result->image,result->request.scale,result->request.pixels);
            } else {
                if(kind==WORK_PREVIEW) page->preview_pending=false;else page->detail_pending=false;
                if(visible&&result->image.pixels) {
                    upload_texture(kind==WORK_PREVIEW?&page->preview:&page->detail,
                                   result->image,result->request.scale,result->request.pixels);
                    page->preview_attempted=true;
                } else if(visible&&kind==WORK_PREVIEW) page->preview_attempted=true;
                if(result->cropped) {page->crop=result->crop;page->crop_ready=true;cropped=true;}
                if(visible) viewer.render_due=GetTime();
            }
            if(result->error[0]&&visible) set_message(result->error);
        }
        worker_result_free(result);
    }
    if(cropped&&viewer.crop) {measure_document();restore_pdf_point(anchor_page,source,anchor);refresh(0);}
}

static void begin_find(void)
{
    if(!viewer.pdf) return;
    viewer.focused=false;viewer.search_open=true;viewer.focus_id=0;
    request_input(INPUT_FIND,viewer.query);
}

static void search_close(void)
{
    if(viewer.worker) worker_search(viewer.worker,"");
    worker_search_free(&viewer.search);viewer.search_open=false;viewer.search_index=-1;
}

static void find_next(int direction)
{
    if(!viewer.pdf||viewer.search.matches==0) return;
    int count=viewer.search.matches;
    viewer.search_index=(viewer.search_index+direction+count)%count;
    for(size_t i=0;i<viewer.search.count;i++) {
        PdfMatch hit=viewer.search.hits[i];
        if(hit.match!=viewer.search_index) continue;
        history_push();viewer.menu=MENU_NONE;
        viewer.history_restoring=true;show_page(hit.page);viewer.history_restoring=false;
        Vector2 target={viewer.layout.body.x+viewer.layout.body.width*.5f,
                        viewer.layout.body.y+viewer.layout.body.height*.35f};
        restore_pdf_point(hit.page,(Vector2){(hit.box.x0+hit.box.x1)*.5f,(hit.box.y0+hit.box.y1)*.5f},target);
        refresh(0);return;
    }
}

static void update_search(void)
{
    if(!viewer.search_open) return;
    PdfSearch next={0};
    if(!worker_search_snapshot(viewer.worker,viewer.search.version,&next)) return;
    worker_search_free(&viewer.search);viewer.search=next;
    if(viewer.search_auto_jump&&viewer.search.matches>0) {
        viewer.search_auto_jump=false;viewer.search_index=-1;find_next(1);
    }
}

static int hit_page(Vector2 point)
{
    if(!contains(viewer.layout.body,point)) return -1;
    int first=page_at(viewer.scroll),end=visible_end(first);
    for(int i=first;i<end;i++) if(contains(page_rect(i),point)) return i;
    return -1;
}

static int hit_glyph(int page,Vector2 point,bool nearest)
{
    PdfText *text=viewer.pages[page].text;
    if(!text||!text->count) return -1;
    point=screen_to_pdf(page,point);
    double best=1e30;int found=-1;
    for(size_t i=0;i<text->count;i++) {
        PdfBox b=text->glyphs[i].box;
        float dx=maxf(b.x0-point.x,maxf(0,point.x-b.x1));
        float dy=maxf(b.y0-point.y,maxf(0,point.y-b.y1));
        double distance=dx*dx+dy*dy*3;
        if(distance<best) {best=distance;found=(int)i;}
        if(distance==0) return (int)i;
    }
    return nearest?found:best<=9?found:-1;
}

static bool word_character(int cp)
{
    return cp>127||(cp>='0'&&cp<='9')||(cp>='A'&&cp<='Z')||(cp>='a'&&cp<='z')||cp=='_';
}

static Rectangle selection_copy_box(void)
{
    if(!viewer.selection.active||viewer.selection.page<0||viewer.selection.page>=viewer.page_count) return (Rectangle){0};
    PdfText *text=viewer.pages[viewer.selection.page].text;
    if(!text||!text->count) return (Rectangle){0};
    size_t first=viewer.selection.first<text->count?viewer.selection.first:text->count-1;
    Rectangle word=pdf_box_screen(viewer.selection.page,text->glyphs[first].box);
    Rectangle body=viewer.layout.body;
    float u=viewer.layout.ui;
    return rect(Clamp(word.x,body.x+8*u,body.x+body.width-100*u),
                Clamp(word.y-48*u,body.y+4*u,body.y+body.height-46*u),92*u,40*u);
}

static bool selection_pointer(void)
{
    Pointer *p=&viewer.pointer;
    TextSelection *selection=&viewer.selection;
    if(IsKeyDown(KEY_SPACE)||IsMouseButtonDown(MOUSE_BUTTON_MIDDLE)) return false;
    if(p->pressed) {
        if(selection->active) {
            PdfText *text=viewer.pages[selection->page].text;
            if(text&&text->count) {
                Rectangle first=pdf_box_screen(selection->page,text->glyphs[selection->first].box);
                Rectangle last=pdf_box_screen(selection->page,text->glyphs[selection->last].box);
                Vector2 a={first.x,first.y+first.height},b={last.x+last.width,last.y+last.height};
                if(contains(selection_copy_box(),p->start)) return true;
                if(p->touches&&minf(Vector2Distance(a,p->point),Vector2Distance(b,p->point))<20*viewer.layout.ui) {
                    selection->handle=Vector2Distance(a,p->point)<Vector2Distance(b,p->point)?1:2;
                    selection->dragging=true;p->moved=true;return true;
                }
            }
        }
        int page=hit_page(p->start);
        int glyph=page>=0?hit_glyph(page,p->start,false):-1;
        selection->dragging=false;
        if(!p->touches&&glyph>=0&&pdf_can_copy(viewer.pdf)) {
            *selection=(TextSelection){page,(size_t)glyph,(size_t)glyph,false,true,0};
            return true;
        }
    }
    if(p->down&&selection->dragging) {
        int glyph=hit_glyph(selection->page,p->point,true);
        if(glyph<0) return true;
        bool moved=Vector2Distance(p->start,p->point)>4*viewer.layout.ui;
        if(selection->handle==3&&!moved) return true;
        if(moved||selection->handle) {
            if(selection->handle==1) selection->first=(size_t)glyph>selection->last?selection->last:(size_t)glyph;
            else selection->last=(size_t)glyph;
            selection->active=true;p->moved=true;
        }
        return true;
    }
    if(p->released&&selection->dragging) {
        selection->dragging=false;
        if(selection->first>selection->last) {size_t swap=selection->first;selection->first=selection->last;selection->last=swap;}
        return selection->active;
    }
    if(p->touches&&p->down&&!p->moved&&GetTime()-p->began>.5) {
        int page=hit_page(p->start);
        int glyph=page>=0?hit_glyph(page,p->start,false):-1;
        if(glyph>=0&&pdf_can_copy(viewer.pdf)) {
            PdfText *text=viewer.pages[page].text;
            size_t first=(size_t)glyph,last=first;
            while(first>0&&text->glyphs[first-1].line==text->glyphs[glyph].line&&word_character(text->glyphs[first-1].codepoint)) first--;
            while(last+1<text->count&&text->glyphs[last+1].line==text->glyphs[glyph].line&&word_character(text->glyphs[last+1].codepoint)) last++;
            *selection=(TextSelection){page,first,last,true,true,3};p->moved=true;return true;
        }
    }
    if(selection->active&&p->down&&contains(selection_copy_box(),p->start)) return true;
    return false;
}

static void copy_selection(void)
{
    if(!viewer.pdf||!viewer.selection.active) {set_message("Select text to copy.");return;}
    if(!pdf_can_copy(viewer.pdf)) {set_message("This PDF does not allow text copying.");return;}
    PdfText *text=viewer.pages[viewer.selection.page].text;
    char *copy=pdf_copy_text(text,viewer.selection.first,viewer.selection.last);
    if(copy) {set_message(platform_copy_text(copy)?"Copied text.":"Could not copy text.");free(copy);}
}

static void select_all_text(void)
{
    if(!viewer.pdf) return;
    if(!pdf_can_copy(viewer.pdf)) {set_message("This PDF does not allow text copying.");return;}
    PdfText *text=viewer.pages[viewer.page].text;
    if(!text) {set_message("Loading selectable text. Try again in a moment.");return;}
    if(!text->count) {set_message("This page has no selectable text.");return;}
    viewer.selection=(TextSelection){viewer.page,0,text->count-1,true,false,0};
    viewer.menu=MENU_NONE;
}

static bool follow_link_at(Vector2 point)
{
    int page=hit_page(point);
    if(page<0||!viewer.pages[page].text) return false;
    PdfText *text=viewer.pages[page].text;
    for(size_t i=0;i<text->link_count;i++) {
        PdfLink *link=&text->links[i];
        if(!contains(pdf_box_screen(page,link->box),point)) continue;
        if(link->page>=0) {
            show_page(link->page);
            PdfBox bounds=reading_bounds(&viewer.pages[link->page]);
            restore_pdf_point(link->page,(Vector2){isfinite(link->x)?link->x:bounds.x0,
                                                   isfinite(link->y)?link->y:bounds.y0},
                               (Vector2){viewer.layout.body.x+page_margin(),viewer.layout.body.y+page_margin()});
            refresh(0);
        } else if(!platform_open_link(link->uri)) set_message("This link type cannot be opened.");
        return true;
    }
    return false;
}

static void draw_interaction(void)
{
    const Palette *palette=&palettes[viewer.theme];
    Rectangle body=viewer.layout.body;
    BeginScissorMode((int)body.x,(int)body.y,(int)body.width,(int)body.height);
    for(size_t i=0;i<viewer.search.count;i++) {
        PdfMatch match=viewer.search.hits[i];
        Rectangle box=pdf_box_screen(match.page,match.box);
        if(!CheckCollisionRecs(body,box)) continue;
        Color color=match.match==viewer.search_index?(Color){230,145,35,255}:(Color){245,204,55,255};
        DrawRectangleRec(box,Fade(color,match.match==viewer.search_index?.50f:.28f));
        if(match.match==viewer.search_index) DrawRectangleLinesEx(box,maxf(1,viewer.layout.ui),color);
    }
    if(viewer.menu==MENU_NONE) {
        int first=page_at(viewer.scroll),end=visible_end(first);
        for(int page=first;page<end;page++) {
            PdfText *text=viewer.pages[page].text;if(!text) continue;
            for(size_t i=0;i<text->link_count;i++) {
                PdfLink *link=&text->links[i];Rectangle box=GetCollisionRec(body,pdf_box_screen(page,link->box));
                char name[320];
                if(link->page>=0) snprintf(name,sizeof name,"Go to linked page %d",link->page+1);
                else snprintf(name,sizeof name,"Open link: %s",link->uri);
                register_element(box,name,true,false,false);
            }
        }
    }
    TextSelection *selection=&viewer.selection;
    if(selection->active) {
        PdfText *text=viewer.pages[selection->page].text;
        if(text&&text->count) {
            size_t first=selection->first,last=selection->last;
            if(first>last) {size_t swap=first;first=last;last=swap;}
            for(size_t i=first;i<=last&&i<text->count;i++)
                DrawRectangleRec(pdf_box_screen(selection->page,text->glyphs[i].box),Fade(palette->accent,.32f));
#if defined(__ANDROID__) || defined(__EMSCRIPTEN__)
            Rectangle a=pdf_box_screen(selection->page,text->glyphs[first].box);
            Rectangle b=pdf_box_screen(selection->page,text->glyphs[last].box);
            DrawCircleV((Vector2){a.x,a.y+a.height},7*viewer.layout.ui,palette->accent);
            DrawCircleV((Vector2){b.x+b.width,b.y+b.height},7*viewer.layout.ui,palette->accent);
#endif
        }
    }
    EndScissorMode();
    if(selection->active&&viewer.menu==MENU_NONE) button(selection_copy_box(),"Copy",false,true);
}

static void draw_search(void)
{
    Layout l=viewer.layout;float u=l.ui;
    const Palette *p=&palettes[viewer.theme];
    DrawRectangleRec(rect(0,l.bar,l.width,52*u),p->panel);
    DrawRectangleRec(l.search_field,p->button);
    BeginScissorMode((int)l.search_field.x,(int)l.search_field.y,(int)l.search_field.width,(int)l.search_field.height);
    label(*viewer.query?viewer.query:"Find in document",l.search_field.x+8*u,l.search_field.y+3*u,16*u,p->ink);
    char status[128];
    if(!*viewer.query) snprintf(status,sizeof status,"Enter text to search");
    else if(viewer.search.complete&&viewer.search.matches==0) snprintf(status,sizeof status,"No matches%s",viewer.search.errors?" (some pages could not be searched)":"");
    else snprintf(status,sizeof status,"%d / %d%s%s",viewer.search_index+1,viewer.search.matches,
                  viewer.search.limited?"+":"",viewer.search.complete?"":" · Searching...");
    label(status,l.search_field.x+8*u,l.search_field.y+25*u,12*u,p->muted);
    EndScissorMode();
    register_element(l.search_field,"Edit search query",true,false,false);
    button(l.search_prev,"<",false,viewer.search.matches>0);
    button(l.search_next,">",false,viewer.search.matches>0);
    button(l.search_close,"X",false,true);
}

static void draw_loading(void)
{
    Layout l=viewer.layout;float u=l.ui;
    const Palette *p=&palettes[viewer.theme];
    Rectangle banner=rect(8*u,l.body.y+4*u,l.width-16*u,48*u);
    DrawRectangleRec(banner,p->panel);
    char text[320];
    if(viewer.opening_total>0) snprintf(text,sizeof text,"Opening PDF · %d%%",100*viewer.opening_done/viewer.opening_total);
    else snprintf(text,sizeof text,"Opening PDF...");
    label(text,banner.x+10*u,banner.y+13*u,16*u,p->ink);
    button(l.loading_cancel,"Cancel",false,true);
}

static void draw_thumbnail_row(Rectangle row,size_t index)
{
    ReadingPage *page=&viewer.pages[index];
    const Palette *p=&palettes[viewer.theme];float u=viewer.layout.ui;
    DrawRectangleRounded(row,.08f,6,(int)index==viewer.page?p->hover:p->button);
    char label_text[64];snprintf(label_text,sizeof label_text,"Page %zu",index+1);
    register_element(GetCollisionRec(reading_layout().list,row),label_text,true,(int)index==viewer.page,false);
    if((int)index<viewer.thumb_start|| (int)index>=viewer.thumb_end) {
        int first=(int)viewer.reading.scroll;
        int last=(int)reading_item_end(reading_layout());
        for(int i=viewer.thumb_start;i<viewer.thumb_end;i++) if(i<first||i>=last) release_texture(&viewer.pages[i].thumbnail);
        viewer.thumb_start=first;viewer.thumb_end=last;
    }
    if(!page->thumbnail.texture.id&&!page->thumb_pending) {
        PdfBox b=page->bounds;
        float scale=180/maxf(b.x1-b.x0,b.y1-b.y0);
        page->thumb_pending=worker_request(viewer.worker,(PdfRequest){WORK_THUMBNAIL,(int)index,scale,pixel_box(b,scale),false});
    }
    PdfBox b=page->bounds;
    float width=b.x1-b.x0,height=b.y1-b.y0;
    if(viewer.rotation%2) {float swap=width;width=height;height=swap;}
    float scale=minf(66*u/width,(row.height-12*u)/height);
    Rectangle preview=rect(row.x+10*u,row.y+6*u,width*scale,height*scale);
    DrawRectangleRec(preview,p->paper);
    if(page->thumbnail.texture.id) {BeginShaderMode(viewer.shader);draw_texture(page->thumbnail,b,preview,scale);EndShaderMode();}
    label(label_text,row.x+90*u,row.y+row.height*.5f-9*u,18*u,p->ink);
    if((int)index==viewer.page) label("Current page",row.x+90*u,row.y+row.height*.5f+15*u,13*u,p->muted);
}

static void document_action(int action)
{
    if(!viewer.pdf) return;
    if(action==PLATFORM_PRINT&&!pdf_can_print(viewer.pdf)) {set_message("This PDF does not allow printing.");return;}
    viewer.menu=MENU_NONE;
    if(!platform_document_action((PlatformCommand)action,viewer.source.path,viewer.title,viewer.password,viewer.page_count))
        set_message("This action is unavailable on this platform.");
}

static bool same_box(Rectangle a,Rectangle b)
{
    return a.width>0&&b.width>0&&fabsf(a.x-b.x)<.5f&&fabsf(a.y-b.y)<.5f&&fabsf(a.width-b.width)<.5f;
}

static void register_element(Rectangle box,const char *text,bool enabled,bool selected,bool is_text)
{
    if(viewer.element_count==UI_LIMIT||box.width<1||box.height<1) return;
    if((viewer.menu!=MENU_NONE||viewer.input_kind!=INPUT_NONE)&&!viewer.building_menu) return;
    Rectangle screen=rect(0,0,viewer.layout.width,viewer.layout.height);
    if(viewer.menu==MENU_VIEW&&viewer.building_menu) screen=viewer.layout.popup;
    box=GetCollisionRec(box,screen);
    if(box.width<1||box.height<1) return;
    Layout l=viewer.layout;
    const char *label_text=text;
    if(same_box(box,l.theme)||same_box(box,l.bottom_theme)) label_text="Choose reading theme";
    else if(same_box(box,l.prev)) label_text="Previous page";
    else if(same_box(box,l.next)) label_text="Next page";
    else if(same_box(box,l.page)) label_text="Go to page";
    else if(same_box(box,l.in)) label_text="Zoom in";
    else if(same_box(box,l.out)) label_text="Zoom out";
    else if(same_box(box,l.brightness_less)) label_text="Decrease page brightness";
    else if(same_box(box,l.brightness_more)) label_text="Increase page brightness";
    else if(same_box(box,l.search_prev)) label_text="Previous search match";
    else if(same_box(box,l.search_next)) label_text="Next search match";
    else if(same_box(box,l.search_close)) label_text="Close search";
    if(!label_text||!*label_text) return;
    UiElement *element=&viewer.elements[viewer.element_count++];
    *element=(UiElement){.box=box,.enabled=enabled,.selected=selected,.text=is_text};
    snprintf(element->label,sizeof element->label,"%s",label_text);
    utf8_trim(element->label);
    unsigned hash=2166136261u;
    for(const unsigned char *c=(const unsigned char *)label_text;*c;c++) hash=(hash^*c)*16777619u;
    element->id=(hash^(unsigned)viewer.element_count*16777619u)&0x7fffffffu;
    if(!element->id) element->id=1;
}

static void activate_element(unsigned id)
{
    for(size_t i=0;i<viewer.element_count;i++) {
        UiElement *element=&viewer.elements[i];
        if(element->id!=id||!element->enabled||element->text) continue;
        Pointer *p=&viewer.pointer;
        p->point=p->start=center(element->box);p->released=true;p->pressed=false;p->moved=false;p->down=false;
        viewer.focus_id=id;viewer.keyboard_activation=true;
        return;
    }
}

static void update_keyboard_focus(void)
{
    viewer.keyboard_activation=false;
    if(IsKeyPressed(KEY_TAB)) {
        int direction=(IsKeyDown(KEY_LEFT_SHIFT)||IsKeyDown(KEY_RIGHT_SHIFT))?-1:1;
        int current=direction>0?-1:(int)viewer.element_count;
        for(size_t i=0;i<viewer.element_count;i++) if(viewer.elements[i].id==viewer.focus_id) current=(int)i;
        for(size_t attempt=0;attempt<viewer.element_count;attempt++) {
            current=(current+direction+(int)viewer.element_count)%(int)viewer.element_count;
            if(viewer.elements[current].enabled&&!viewer.elements[current].text) {viewer.focus_id=viewer.elements[current].id;break;}
        }
    }
    if(viewer.focus_id&&(IsKeyPressed(KEY_ENTER)||IsKeyPressed(KEY_SPACE))) activate_element(viewer.focus_id);
}

static void draw_focus(void)
{
    for(size_t i=0;i<viewer.element_count;i++) if(viewer.elements[i].id==viewer.focus_id) {
        Rectangle box=viewer.elements[i].box;
        DrawRectangleLinesEx(rect(box.x-2,box.y-2,box.width+4,box.height+4),2*viewer.layout.ui,palettes[viewer.theme].accent);
        break;
    }
}

static void update_commands(void)
{
    CommandEvent event;
    while(platform_poll_command(&event)) {
        if(event.kind==PLATFORM_ACCESSIBILITY) {activate_element(event.element);continue;}
        if(event.kind==PLATFORM_OPEN) choose_new_pdf();
        else if(event.kind==PLATFORM_FIND) begin_find();
        else if(event.kind==PLATFORM_COPY) copy_selection();
        else if(event.kind==PLATFORM_SELECT_ALL) select_all_text();
        else if(event.kind==PLATFORM_FIND_NEXT) find_next(1);
        else if(event.kind==PLATFORM_FIND_PREVIOUS) find_next(-1);
        else if(event.kind==PLATFORM_GO_PAGE&&viewer.pdf) go_to_page();
        else if(event.kind==PLATFORM_ZOOM_IN&&viewer.pdf) zoom_at(1.25f,center(viewer.layout.body),center(viewer.layout.body));
        else if(event.kind==PLATFORM_ZOOM_OUT&&viewer.pdf) zoom_at(.8f,center(viewer.layout.body),center(viewer.layout.body));
        else if(event.kind==PLATFORM_FIT&&viewer.pdf) fit(false);
        else if(event.kind==PLATFORM_BACK) history_move(-1);
        else if(event.kind==PLATFORM_FORWARD) history_move(1);
        else if(event.kind==PLATFORM_NEXT_PAGE&&viewer.pdf) change_page(1);
        else if(event.kind==PLATFORM_PREVIOUS_PAGE&&viewer.pdf) change_page(-1);
        else if(event.kind==PLATFORM_SCROLL_DOWN||event.kind==PLATFORM_SCROLL_UP) {
            viewer.wheel.y=event.kind==PLATFORM_SCROLL_DOWN?-3:3;
            viewer.pointer.point=center(viewer.menu==MENU_VIEW?viewer.layout.popup:
                                        viewer.menu==MENU_READING?reading_layout().list:viewer.layout.body);
        }
        else if(event.kind==PLATFORM_PRINT||event.kind==PLATFORM_SHARE||event.kind==PLATFORM_EXPORT) document_action(event.kind);
    }
}

static void publish_accessibility(void)
{
    if(GetTime()<viewer.accessibility_due) return;
    viewer.accessibility_due=GetTime()+.2;
    Json j={0};json_add(&j,"{\"width\":%.2f,\"height\":%.2f,\"scrollable\":%s,\"hasDocument\":%s,\"canCopy\":%s,\"canPrint\":%s,\"selection\":%s,\"nodes\":[",
        viewer.layout.width,viewer.layout.height,(viewer.menu==MENU_VIEW||viewer.menu==MENU_READING)?"true":"false",
        viewer.pdf?"true":"false",pdf_can_copy(viewer.pdf)?"true":"false",pdf_can_print(viewer.pdf)?"true":"false",viewer.selection.active?"true":"false");
    for(size_t i=0;i<viewer.element_count;i++) {
        UiElement *e=&viewer.elements[i];
        json_add(&j,"%s{\"id\":%u,\"x\":%.2f,\"y\":%.2f,\"w\":%.2f,\"h\":%.2f,\"enabled\":%s,\"selected\":%s,\"text\":%s,\"label\":",
                 i?",":"",e->id,e->box.x,e->box.y,e->box.width,e->box.height,e->enabled?"true":"false",e->selected?"true":"false",e->text?"true":"false");
        json_string(&j,e->label);json_add(&j,"}");
    }
    json_add(&j,"],\"document\":");
    if(viewer.pdf&&viewer.menu==MENU_NONE&&viewer.input_kind==INPUT_NONE) {
        PdfText *text=viewer.pages[viewer.page].text;
        char title[320];snprintf(title,sizeof title,"%s. Page %d of %d. ",viewer.title,viewer.page+1,viewer.page_count);
        Json reading={0};json_add(&reading,"%s",title);
        if(text&&text->text&&*text->text) json_add(&reading,"%.60000s",text->text);
        else json_add(&reading,"%s",text?"This page has no selectable text.":"Loading page text.");
        utf8_trim(reading.data);
        json_string(&j,reading.data);free(reading.data);
    } else json_string(&j,"");
    Rectangle b=viewer.layout.body;
    json_add(&j,",\"body\":[%.2f,%.2f,%.2f,%.2f]}",b.x,b.y,b.width,b.height);
    if(!j.failed) platform_accessibility(j.data);
    free(j.data);
}
