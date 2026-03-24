Output Format (CLI + Parser)

The CLI and parser module reads the Docksmithfile and converts it into a structured list called InstructionList. Each instruction is stored with its type (like COPY, RUN) and its arguments. All valid instructions are kept in the same order as in the file.

If an invalid instruction is found, an error message with the line number is shown. Only valid instructions are stored and passed to the build engine, so it can execute them directly without needing to parse again.