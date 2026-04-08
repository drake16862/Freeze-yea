#ifndef INPUT_H
#define INPUT_H
#include "types.h"
unsigned char inb(unsigned short port);
void outb(unsigned short port, unsigned char val);
void get_input(char* buffer, const uint size);
char scancode_to_ascii(unsigned char sc);
#endif
