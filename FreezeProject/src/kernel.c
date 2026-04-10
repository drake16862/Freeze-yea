__attribute__((section(".multiboot")))
const unsigned int multiboot_header[] = {
    0x1BADB002,
    0x00000003,
    -(0x1BADB002 + 0x00000003)
};

#include "memory.h"
#include "timer.h"
#include "input.h"
#include "vga.h"
#include "rtc.h"
#include "shell.h"
#include "fs.h"
#include "disk.h"
#include "net.h"
#include <stdint.h>

void kernel_main(void){
    clear();

    net_scan_pci();

    disk_init();
    fs_init();
    fs_mount();

    print("___________                                    \n");
    print("\\_   _____/______   ____   ____ ________ ____  \n");
    print(" |    __) \\_  __ \\_/ __ \\_/ __ \\\\___   // __ \\ \n");
    print(" |     \\   |  | \\/\\  ___/\\  ___/ /    /\\  ___/ \n");
    print(" \\___  /   |__|    \\___  >\\___  >_____ \\\\___  >\n");
    print("     \\/                \\/     \\/      \\/    \\/ \n");
    print("\033[92mhttps://freezeos.org/\033[0m\n");
    print("\033[93mType 'help' for help on learning commands\033[0m\n\n");
    print("\033[94m--------------------------------\033[0m\n");
    print("\033[92mCurrently: \033[93mVersion 0.64\033[0m\n");

    shell();
}