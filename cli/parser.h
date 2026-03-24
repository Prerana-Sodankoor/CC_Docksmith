#ifndef PARSER_H
#define PARSER_H

#define MAX_INSTRUCTIONS 100

typedef struct {
    char type[20];
    char arg1[256];
    char arg2[256];
} Instruction;

typedef struct {
    Instruction instructions[MAX_INSTRUCTIONS];
    int count;
} InstructionList;

void parse_docksmithfile(const char *path, InstructionList *list);

void print_instructions(InstructionList *list);
#endif