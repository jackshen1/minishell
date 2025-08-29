# Minishell

A lightweight Unix-style shell written in C.

## Features

- Built-in commands: cd, exit (with support for cd - and cd ~)

- Custom command prompt showing the current working directory in color

- Signal handling (SIGINT) to gracefully return to the prompt

- Fork/exec for launching external programs

- Error handling for all system calls

- Support for quoted arguments and directory names with spaces

- Redirection: input (<), output (>), and append (>>)

- Multi-stage pipelines (|) with up to 64 commands

- Argument limit: up to 2048 arguments per command

- Detection of invalid syntax for pipes and redirection operators

## Notes

Originally written as an Advanced Programming (Columbia CS3157) assignment.
