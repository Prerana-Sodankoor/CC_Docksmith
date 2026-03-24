#include <stdio.h>
#include <string.h>
#include "cli.h"
#include "parser.h"

void handle_build(int argc, char *argv[]) {
    if (argc < 5) {
    
        printf("Error: build requires -t <name:tag> <context>\n");
        return;
    }

    char *tag = argv[3];
    char *context = argv[4];

    printf("Building image: %s\n", tag);

    InstructionList list = {0};
    parse_docksmithfile("Docksmithfile", &list);

    printf("Parsed %d instructions\n", list.count);

    print_instructions(&list);
}

void handle_images() {
    printf("Listing images...\n");
}

void handle_rmi(char *image) {
    if (!image) {
        printf("Error: Missing image name\n");
        return;
    }
    printf("Removing image: %s\n", image);
}

void handle_run(int argc, char *argv[]) {
    if (argc < 3) {
        printf("Error: Missing image name\n");
        return;
    }

    printf("Running image: %s\n", argv[2]);
}

void handle_cli(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: docksmith <command>\n");
        return;
    }

    if (strcmp(argv[1], "build") == 0) {
        handle_build(argc, argv);

    } else if (strcmp(argv[1], "images") == 0) {
        handle_images();

    } else if (strcmp(argv[1], "rmi") == 0) {
        handle_rmi(argv[2]);

    } else if (strcmp(argv[1], "run") == 0) {
        handle_run(argc, argv);

    } else {
        printf("Unknown command: %s\n", argv[1]);
    }
}