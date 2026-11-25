#ifndef FOLIO_MACOS_H
#define FOLIO_MACOS_H

#include <stddef.h>
#include <stdbool.h>

char *macos_state_directory(void);
int macos_choose_file(char *path, size_t size);

void macos_prepare(void);
void macos_init(void);
bool macos_request_text(const char *label,const char *initial,int input);
bool macos_copy_text(const char *text);
bool macos_open_link(const char *uri);
bool macos_document_action(int command,const char *path,const char *title,const char *password);
void macos_accessibility(const char *json);
#endif
