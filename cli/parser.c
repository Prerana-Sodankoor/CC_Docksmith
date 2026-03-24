#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "parser.h"

int is_valid_instruction(char *type) {
    return (
        strcmp(type, "FROM") == 0 ||
        strcmp(type, "COPY") == 0 ||
        strcmp(type, "RUN") == 0 ||
        strcmp(type, "WORKDIR") == 0 ||
        strcmp(type, "ENV") == 0 ||
        strcmp(type, "CMD") == 0
    );
}
void print_instructions(InstructionList *list) {
    for (int i = 0; i < list->count; i++) {
        printf("%d: %s %s %s\n",
            i + 1,
            list->instructions[i].type,
            list->instructions[i].arg1,
            list->instructions[i].arg2
        );
    }
}

void parse_docksmithfile(const char *path, InstructionList *list) {
    FILE *file = fopen(path, "r");
    if (!file) {
        printf("Error: Cannot open Docksmithfile\n");
        return;
    }

    char line[512];
    int line_num = 0;

    while (fgets(line, sizeof(line), file)) {
        line_num++;

        if (line[0] == '\n') continue;

        Instruction inst = {0};

        sscanf(line, "%s %s %s", inst.type, inst.arg1, inst.arg2);

        if (!is_valid_instruction(inst.type)) {
            printf("Error at line %d: Invalid instruction %s\n", line_num, inst.type);
            continue;
        }

        list->instructions[list->count++] = inst;
    }

    fclose(file);
}