/*
 * parser.c — Docksmithfile parser
 *
 * Parses the Docksmithfile line by line into an InstructionList.
 *
 * Design decisions:
 *   - CMD and RUN store the ENTIRE rest-of-line in arg1 so that
 *     "CMD [\"python\", \"main.py\"]" and "RUN pip install flask"
 *     are stored intact without losing tokens.
 *   - COPY stores src in arg1, dest in arg2 (two tokens).
 *   - ENV KEY=VALUE stores the full "KEY=VALUE" in arg1.
 *     ENV KEY VALUE  stores KEY in arg1, VALUE in arg2.
 *   - Comment lines (#) and blank lines are silently skipped.
 *   - Unknown instructions exit immediately with line number (spec requirement).
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "parser.h"

static int is_valid_instruction(const char *type)
{
    return (strcmp(type, "FROM")    == 0 ||
            strcmp(type, "COPY")    == 0 ||
            strcmp(type, "RUN")     == 0 ||
            strcmp(type, "WORKDIR") == 0 ||
            strcmp(type, "ENV")     == 0 ||
            strcmp(type, "CMD")     == 0);
}

static void strip_newline(char *s)
{
    size_t len = strlen(s);
    while (len > 0 && (s[len-1] == '\n' || s[len-1] == '\r'))
        s[--len] = '\0';
}

static int is_blank(const char *s)
{
    while (*s) { if (!isspace((unsigned char)*s)) return 0; s++; }
    return 1;
}

void print_instructions(const InstructionList *list)
{
    for (int i = 0; i < list->count; i++) {
        printf("%d: %s %s%s%s\n",
               i + 1,
               list->instructions[i].type,
               list->instructions[i].arg1,
               list->instructions[i].arg2[0] ? " " : "",
               list->instructions[i].arg2);
    }
}

void parse_docksmithfile(const char *path, InstructionList *list)
{
    FILE *file = fopen(path, "r");
    if (!file) {
        fprintf(stderr, "Error: Cannot open Docksmithfile at '%s'\n", path);
        return;
    }

    char line[512];
    int  line_num = 0;

    while (fgets(line, sizeof(line), file)) {
        line_num++;
        strip_newline(line);
        if (is_blank(line)) continue;

        /* Skip comment lines */
        const char *p = line;
        while (isspace((unsigned char)*p)) p++;
        if (*p == '#') continue;

        /* Read instruction type token */
        Instruction inst = {0};
        int type_len = 0;
        while (*p && !isspace((unsigned char)*p) && type_len < 19)
            inst.type[type_len++] = *p++;
        inst.type[type_len] = '\0';

        /* Validate — spec requires immediate fail + line number */
        if (!is_valid_instruction(inst.type)) {
            fprintf(stderr,
                    "Error at line %d: unrecognised instruction '%s'.\n"
                    "  Supported: FROM, COPY, RUN, WORKDIR, ENV, CMD\n",
                    line_num, inst.type);
            fclose(file);
            exit(1);
        }

        /* Skip whitespace between keyword and arguments */
        while (isspace((unsigned char)*p)) p++;
        const char *rest = p;

        /*
         * COPY takes two space-separated args: src and dest.
         * ENV without '=' takes two args: key and value.
         * Everything else stores the entire rest-of-line in arg1.
         */
        if (strcmp(inst.type, "COPY") == 0 ||
            (strcmp(inst.type, "ENV") == 0 && strchr(rest, '=') == NULL))
        {
            int i = 0;
            while (*rest && !isspace((unsigned char)*rest) && i < 255)
                inst.arg1[i++] = *rest++;
            inst.arg1[i] = '\0';
            while (isspace((unsigned char)*rest)) rest++;
            i = 0;
            while (*rest && i < 255) inst.arg2[i++] = *rest++;
            inst.arg2[i] = '\0';
            /* strip trailing whitespace from arg2 */
            i = (int)strlen(inst.arg2) - 1;
            while (i >= 0 && isspace((unsigned char)inst.arg2[i]))
                inst.arg2[i--] = '\0';
        }
        else
        {
            /* arg1 = entire rest-of-line */
            int i = 0;
            while (*rest && i < 255) inst.arg1[i++] = *rest++;
            inst.arg1[i] = '\0';
            i = (int)strlen(inst.arg1) - 1;
            while (i >= 0 && isspace((unsigned char)inst.arg1[i]))
                inst.arg1[i--] = '\0';
        }

        if ((strcmp(inst.type, "FROM") == 0 ||
             strcmp(inst.type, "WORKDIR") == 0 ||
             strcmp(inst.type, "RUN") == 0 ||
             strcmp(inst.type, "CMD") == 0) &&
            inst.arg1[0] == '\0') {
            fprintf(stderr, "Error at line %d: '%s' requires an argument.\n",
                    line_num, inst.type);
            fclose(file);
            exit(1);
        }

        if ((strcmp(inst.type, "COPY") == 0 || strcmp(inst.type, "ENV") == 0) &&
            (inst.arg1[0] == '\0' || inst.arg2[0] == '\0') &&
            !(strcmp(inst.type, "ENV") == 0 && strchr(inst.arg1, '=') != NULL)) {
            fprintf(stderr, "Error at line %d: '%s' requires two arguments.\n",
                    line_num, inst.type);
            fclose(file);
            exit(1);
        }

        if (strcmp(inst.type, "CMD") == 0 && inst.arg1[0] != '[') {
            fprintf(stderr,
                    "Error at line %d: CMD must use JSON array form.\n",
                    line_num);
            fclose(file);
            exit(1);
        }

        if (list->count < MAX_INSTRUCTIONS)
            list->instructions[list->count++] = inst;
        else {
            fprintf(stderr, "Error: too many instructions (max %d)\n",
                    MAX_INSTRUCTIONS);
            break;
        }
    }

    fclose(file);
}
