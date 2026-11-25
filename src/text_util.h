#ifndef FOLIO_TEXT_UTIL_H
#define FOLIO_TEXT_UTIL_H
#include <string.h>
// Keep a bounded UTF-8 string valid when a title or input is truncated.
static inline void utf8_trim(char *text) {
    size_t length=strlen(text),start=length;
    while(start>0&&((unsigned char)text[start-1]&0xc0)==0x80) start--;
    if(start==0) {if(length) text[0]='\0';return;}
    unsigned char lead=(unsigned char)text[start-1];
    size_t expected=lead<0x80?1:lead>=0xf0?4:lead>=0xe0?3:lead>=0xc0?2:1;
    if(length-(start-1)<expected) text[start-1]='\0';
}
#endif
