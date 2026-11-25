#ifndef FOLIO_JSON_H
#define FOLIO_JSON_H
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct { char *data; size_t used, capacity; int failed; } Json;
static inline void json_add(Json *j,const char *format,...) {
    if(j->failed) return;
    va_list args,copy;va_start(args,format);va_copy(copy,args);
    int length=vsnprintf(NULL,0,format,copy);va_end(copy);
    if(length<0||(size_t)length>4000000-j->used) {j->failed=1;va_end(args);return;}
    size_t needed=j->used+(size_t)length+1;
    if(needed>j->capacity) {
        size_t capacity=needed*2;
        char *data=realloc(j->data,capacity);
        if(!data) {j->failed=1;va_end(args);return;}
        j->data=data;j->capacity=capacity;
    }
    vsnprintf(j->data+j->used,j->capacity-j->used,format,args);
    j->used+=(size_t)length;va_end(args);
}
static inline void json_string(Json *j,const char *s) {
    json_add(j,"\"");
    for(const unsigned char *p=(const unsigned char *)(s?s:"");*p;) {
        const unsigned char *start=p;
        while(*p>=32&&*p!='"'&&*p!='\\') p++;
        if(p>start) json_add(j,"%.*s",(int)(p-start),(const char *)start);
        if(!*p) break;
        if(*p=='"'||*p=='\\') json_add(j,"\\%c",*p);
        else if(*p<32) json_add(j,"\\u%04x",*p);
        p++;
    }
    json_add(j,"\"");
}
#endif
