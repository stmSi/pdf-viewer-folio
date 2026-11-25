#include "pdf.h"
#include "pdf_worker.h"
#include "reading_state.h"
#include "json.h"
#include "text_util.h"
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(_WIN32)
#include <windows.h>
static void pause_ms(int ms) { Sleep(ms); }
#else
static void pause_ms(int ms) { struct timespec t={ms/1000,(ms%1000)*1000000L};nanosleep(&t,NULL); }
#endif
#define CHECK(x) do {if(!(x)) {fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x);exit(1);}} while(0)
static int assertions;
#define PASS(x) do {CHECK(x);assertions++;} while(0)
static char keys[32][80],values[32][1024];
bool platform_read(const char *key,char *value,size_t size) {for(int i=0;i<32;i++) if(!strcmp(keys[i],key)) {snprintf(value,size,"%s",values[i]);return true;}return false;}
bool platform_write(const char *key,const char *value) {for(int i=0;i<32;i++) if(!*keys[i]||!strcmp(keys[i],key)) {snprintf(keys[i],80,"%s",key);snprintf(values[i],1024,"%s",value);return true;}return false;}
bool platform_delete(const char *key) {for(int i=0;i<32;i++) if(!strcmp(keys[i],key)) *keys[i]=*values[i]=0;return true;}
void platform_list(const char *prefix,StateKeyCallback cb,void *data) {for(int i=0;i<32;i++) if(!strncmp(keys[i],prefix,strlen(prefix))) cb(keys[i],data);}

typedef struct {int matches;size_t quads;} Hits;
static bool match(const PdfBox *boxes,size_t count,void *data) {
    Hits *h=data;h->matches++;h->quads+=count;
    for(size_t i=0;i<count;i++) CHECK(boxes[i].x1>boxes[i].x0&&boxes[i].y1>boxes[i].y0);
    return true;
}
static WorkerStatus wait_ready(PdfWorker *w) {
    for(int i=0;i<2000;i++) {WorkerStatus status=worker_status(w,NULL,NULL,NULL,0);if(status!=WORKER_OPENING) return status;pause_ms(5);}
    CHECK(0);return WORKER_FAILED;
}
static PdfResult *wait_result(PdfWorker *w) {
    for(int i=0;i<2000;i++) {PdfResult *r=worker_poll(w);if(r) return r;pause_ms(5);}CHECK(0);return NULL;
}
static void encrypted_copy(const char *source,const char *destination) {
    fz_context *ctx=fz_new_context(NULL,NULL,FZ_STORE_DEFAULT);
    pdf_document *doc=NULL;fz_var(doc);
    fz_try(ctx) {
        doc=pdf_open_document(ctx,source);
        pdf_write_options options=pdf_default_write_options;
        options.do_encrypt=PDF_ENCRYPT_AES_256;options.permissions=-4;
        strcpy(options.upwd_utf8,"secret");strcpy(options.opwd_utf8,"owner");
        pdf_save_document(ctx,doc,destination,&options);
    }
    fz_always(ctx) {pdf_drop_document(ctx,doc);}
    fz_catch(ctx) {fprintf(stderr,"%s\n",fz_caught_message(ctx));CHECK(0);}
    fz_drop_context(ctx);
}
static bool cancel_print(void *unused) {(void)unused;return true;}
int main(int argc,char **argv) {
    CHECK(argc==2);char path[4096],large[4096],complex[4096],invalid[4096],encrypted[4096],printed[4096],annotated[4096],error[256]={0};
    snprintf(path,sizeof path,"%s/features.pdf",argv[1]);snprintf(large,sizeof large,"%s/large.pdf",argv[1]);
    snprintf(complex,sizeof complex,"%s/complex.pdf",argv[1]);snprintf(invalid,sizeof invalid,"%s/invalid.pdf",argv[1]);
    snprintf(encrypted,sizeof encrypted,"%s/password.pdf",argv[1]);snprintf(printed,sizeof printed,"%s/printed.pdf",argv[1]);
    snprintf(annotated,sizeof annotated,"%s/annotated.pdf",argv[1]);
    Pdf *pdf=pdf_open(path,error,sizeof error);PASS(pdf!=NULL);PASS(pdf_page_count(pdf)==4);PASS(pdf_can_copy(pdf)&&pdf_can_print(pdf));
    PdfBox bounds;PASS(pdf_measure_page(pdf,3,&bounds));PASS(bounds.x1-bounds.x0==600&&bounds.y1-bounds.y0==400);
    PdfText *text=NULL;PASS(pdf_extract_text(pdf,0,&text,error,sizeof error));
    PASS(text->count>40&&strstr(text->text,"Café naïve")!=NULL);PASS(text->link_count==2);
    int internal=0,external=0;for(size_t i=0;i<text->link_count;i++) {
        if(text->links[i].page==1) internal++;
        if(!strcmp(text->links[i].uri,"https://example.com/docs")) external++;
    }PASS(internal==1&&external==1);
    char *copy=pdf_copy_text(text,text->count-1,0);PASS(copy&&strcmp(copy,text->text)==0);free(copy);pdf_text_free(text);
    PASS(pdf_extract_text(pdf,2,&text,error,sizeof error));PASS(text->count==0&&!strcmp(text->text,""));pdf_text_free(text);
    Hits hits={0};PASS(pdf_search_page(pdf,0,"needle",match,&hits,error,sizeof error)==2);PASS(hits.matches==2);
    hits=(Hits){0};PASS(pdf_search_page(pdf,0,"across two",match,&hits,error,sizeof error)==1);PASS(hits.matches==1&&hits.quads==2);
    PdfContents contents;PASS(pdf_load_contents(pdf,&contents,error,sizeof error));PASS(contents.count==2);pdf_free_contents(&contents);pdf_close(pdf);
    int selected[]={1,3};PASS(pdf_print_pages(path,"",selected,2,printed,NULL,NULL,error,sizeof error));
    pdf=pdf_open(printed,error,sizeof error);PASS(pdf&&pdf_page_count(pdf)==2);PASS(pdf_extract_text(pdf,0,&text,error,sizeof error));
    PASS(strstr(text->text,"Chapter Two")!=NULL);pdf_text_free(text);
    PASS(pdf_measure_page(pdf,1,&bounds)&&bounds.x1-bounds.x0==600&&bounds.y1-bounds.y0==400);pdf_close(pdf);
    PASS(!pdf_print_pages(path,"",selected,2,printed,cancel_print,NULL,error,sizeof error));
    int first_page=0;PASS(pdf_print_pages(annotated,"",&first_page,1,printed,NULL,NULL,error,sizeof error));
    pdf=pdf_open(printed,error,sizeof error);PASS(pdf!=NULL);PASS(pdf_extract_text(pdf,0,&text,error,sizeof error));
    PASS(strstr(text->text,"Printable note.")!=NULL&&strstr(text->text,"Screen-only note.")==NULL);pdf_text_free(text);pdf_close(pdf);
    encrypted_copy(path,encrypted);
    PdfWorker *w=worker_open(encrypted);PASS(w!=NULL);PASS(wait_ready(w)==WORKER_PASSWORD);
    worker_password(w,"wrong");PASS(wait_ready(w)==WORKER_PASSWORD);
    worker_password(w,"secret");PASS(wait_ready(w)==WORKER_READY);
    PASS(worker_request(w,(PdfRequest){WORK_PREVIEW,0,1,{0,0,612,792},true}));
    PdfResult *r=wait_result(w);PASS(r->image.pixels&&r->image.width==612&&r->image.height==792&&r->cropped);worker_result_free(r);
    PASS(worker_request(w,(PdfRequest){.kind=WORK_TEXT,.page=0}));r=wait_result(w);
    PASS(r->text&&strstr(r->text->text,"Café naïve"));worker_result_free(r);
    worker_search(w,"needle");unsigned version=0;PdfSearch search={0};
    for(int i=0;i<2000;i++) {
        PdfSearch next;if(worker_search_snapshot(w,version,&next)) {worker_search_free(&search);search=next;version=next.version;}
        if(search.complete) break;pause_ms(5);
    }
    PASS(search.complete&&search.matches==3&&search.scanned==4);worker_search_free(&search);
    worker_search(w,"");PASS(worker_search_snapshot(w,0,&search));PASS(search.complete&&search.count==0);worker_search_free(&search);worker_close(w);
    w=worker_open(invalid);PASS(w!=NULL);PASS(wait_ready(w)==WORKER_FAILED);worker_close(w);
    w=worker_open(large);PASS(w!=NULL);PASS(wait_ready(w)==WORKER_READY);PASS(pdf_page_count(worker_document(w))==1500);
    worker_search(w,"needle");worker_search(w,"nothing matches this");worker_close(w);
    for(int i=0;i<6;i++) {w=worker_open(complex);PASS(w!=NULL);worker_close(w);}
    for(int i=0;i<2000&&worker_active_count();i++) pause_ms(5);
    PASS(worker_active_count()==0);
    SavedPosition position={{2,.4,.7},1.75f,false,3,2,3},restored;
    PASS(reading_save_position(12,position));PASS(reading_load_position(12,&restored));
    PASS(restored.layout==2&&restored.rotation==3&&restored.location.page==2&&restored.zoom==1.75f);
    PASS(platform_write("pdf-000000000000000c","2 1 1 1 0.5 0.25\n"));PASS(reading_load_position(12,&restored));
    PASS(restored.layout==0&&restored.rotation==0&&restored.location.page==1);
    PASS(platform_write("pdf-000000000000000c","3 1 1 1 0.5 0.25 99 0\n"));PASS(!reading_load_position(12,&restored));
    Json json={0};json_string(&json,"quote\" slash\\ newline\n café");PASS(!json.failed&&strstr(json.data,"\\\"")&&!strchr(json.data,'\n'));free(json.data);
    char unicode[]={ 'a',(char)0xf0,(char)0x9f,(char)0x98,0 };utf8_trim(unicode);PASS(!strcmp(unicode,"a"));
    printf("PASS %d PDF engine, async worker, printing, state, and UTF-8 checks\n",assertions);return 0;
}
