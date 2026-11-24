/*
 * check.c - compile-time/runtime proof for emit_ui.py string escaping.
 * Writes the two text strings (UTF-8 bytes) to the file given as argv[1].
 */
#include <stdio.h>

#include "ui_data.h"

int main(int argc, char **argv)
{
    FILE *file;
    if (argc < 2) {
        return 2;
    }
    file = fopen(argv[1], "wb");
    if (file == NULL) {
        return 3;
    }
    fprintf(file, "%s\n%s\n", kUiNodes[1].text, kUiNodes[2].text);
    fclose(file);
    printf("escape_check: node1='%s' node2='%s'\n", kUiNodes[1].text, kUiNodes[2].text);
    return 0;
}
