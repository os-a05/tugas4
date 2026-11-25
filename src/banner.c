#include "banner.h"

#include <stdio.h>

void banner_print(const char *group, const char *course, const char *title)
{
    printf("=== Kelompok 05 | Tugas 4 - Filesystem dan I/O ===\n");
}

void banner_print_menu(const char *const options[], unsigned int count)
{   
    unsigned int i;
    if (options == NULL || count == 0) return;
    puts("\nMenu:");
    for (i = 0; i < count; ++i) {
        printf("  %u. %s\n", i + 1, options[i] != NULL ? options[i] : "(opsi tidak tersedia)");
    }
}
