#ifndef CLI_H
#define CLI_H

/*
 * cli.h — Public interface for the Docksmith CLI
 */

void handle_cli(int argc, char *argv[]);

void handle_build(int argc, char *argv[]);
void handle_images(void);
void handle_rmi(const char *tag);
void handle_run(int argc, char *argv[]);

#endif
