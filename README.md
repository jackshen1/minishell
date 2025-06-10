# Minishell

A lightweight Unix-style shell written in C.

## Features

- Built-in commands: `cd`, `exit`
- Custom command prompt showing current working directory in color
- Signal handling (`SIGINT`) to return gracefully to prompt
- Fork/exec for external commands
- Error handling for all system calls
- Support for quoted arguments and directory names with spaces
- Support for multi-stage pipes (up to 64)

## Notes

Originally written as a systems programming assignment (Columbia CS3157).
