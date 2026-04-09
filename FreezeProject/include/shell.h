#ifndef SHELL_H
#define SHELL_H
#include "types.h"
int startswith(const char* s, const char* p);
int strcmp(const char* a, const char* b);
void shell();
void handle_command(char* buf, const uint buf_size);
extern unsigned char __bss_start;
extern unsigned char __bss_end;
#endif
