#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <limits.h>
#include <errno.h>
#include <sys/wait.h>
#include <pwd.h>
#include <signal.h>
#include <fcntl.h>

#define CMD_BUFFER_SIZE 1024
#define BRIGHTBLUE "\x1b[34;1m"
#define DEFAULT    "\x1b[0m"

#define MAX_ARGS 2048
#define MAX_PIPE_CMDS 64

typedef struct {
    char *args[MAX_ARGS];  // command + arguments
    int num_args;
    char *input_file;      // NULL if no input redirection
    char *output_file;     // NULL if no output redirection
    int append_mode;       // 0 for >, 1 for >>
} command_t;

volatile sig_atomic_t interrupted = 0;

int tokenize(const char *input, char **tokens, const char *delim, int max_tokens) {
    int num_tokens = 0;
    int in_quotes = 0;
    char quote_char = 0;
    int token_start = -1;
    int len = strlen(input);
    
    for (int i = 0; i < len && num_tokens < max_tokens - 1; i++) {
        char c = input[i];
        
        // Handle quotes
        if ((c == '"' || c == '\'') && !in_quotes) {
            in_quotes = 1;
            quote_char = c;
            token_start = i + 1;  // Start token after quote
        }
        else if (c == quote_char && in_quotes) {
            in_quotes = 0;
            // End current token
            if (token_start != -1) {
                int token_len = i - token_start;
                tokens[num_tokens] = malloc(token_len + 1);
                strncpy(tokens[num_tokens], input + token_start, token_len);
                tokens[num_tokens][token_len] = '\0';
                num_tokens++;
                token_start = -1;
            }
        }
        // Handle delimiters (only when not in quotes)
        else if (!in_quotes && strchr(delim, c)) {
            if (token_start != -1) {
                // End current token
                int token_len = i - token_start;
                tokens[num_tokens] = malloc(token_len + 1);
                strncpy(tokens[num_tokens], input + token_start, token_len);
                tokens[num_tokens][token_len] = '\0';
                num_tokens++;
                token_start = -1;
            }
        }
        // Start new token if needed
        else if (token_start == -1 && !isspace(c)) {
            token_start = i;
        }
    }
    
    // Handle last token
    if (token_start != -1) {
        int token_len = len - token_start;
        tokens[num_tokens] = malloc(token_len + 1);
        strncpy(tokens[num_tokens], input + token_start, token_len);
        tokens[num_tokens][token_len] = '\0';
        num_tokens++;
    }
    
    tokens[num_tokens] = NULL;
    return num_tokens;
}

int is_empty(const char *str) {
    if (!str) return 1;
    for (int i = 0; str[i]; i++) {
        if (str[i] != ' ') return 0;
    }
    return 1;
}

void handle_sigint(int sig) {
    interrupted = 1;
    write(STDOUT_FILENO, "\n", 1);
}

void execute_pipeline(char *input) {
    char *cmds[MAX_PIPE_CMDS];
    int num_cmds = 0;
    num_cmds = tokenize(input, cmds, "|", MAX_PIPE_CMDS);
    int prev_fd[2] = {-1, -1};

    for (int i = 0; i < num_cmds; i++) {
        int pipefd[2];
        if (i < num_cmds - 1 && pipe(pipefd) == -1) {
            fprintf(stderr, "Error: pipe() failed. %s.\n", strerror(errno));
            return;
        }
        
        command_t cmd;
        cmd.input_file = NULL;
        cmd.output_file = NULL;
        cmd.append_mode = 0;
        cmd.num_args = 0;
            
        char *tokens[MAX_ARGS];
        int num_tokens = tokenize(cmds[i], tokens, " ", MAX_ARGS);
        
        if (num_tokens == 0) {
            fprintf(stderr, "Error: Empty Command.\n");
            exit(EXIT_FAILURE);
        }
        if (strcmp(tokens[0], ">") == 0 || strcmp(tokens[0], ">>") == 0 || strcmp(tokens[0], "<") == 0) {
            fprintf(stderr, "Error: Invalid Command.\n");
            exit(EXIT_FAILURE);
        }
        
        for (int j = 0; j < num_tokens - 1; j++) {
            if (strcmp(tokens[j], "<") == 0) {
                if (cmd.input_file) {
                    fprintf(stderr, "Error: Multiple input redirections not allowed.\n");
                    exit(EXIT_FAILURE);
                }

                if (is_empty(tokens[j + 1]) || strcmp(tokens[j + 1], "<") == 0 || strcmp(tokens[j + 1], ">") == 0 || strcmp(tokens[j + 1], ">>") == 0) {
                    fprintf(stderr, "Error: Invalid filename after '<'.\n");
                    exit(EXIT_FAILURE);
                }
                cmd.input_file = tokens[j + 1];
                j++;
            }
            else if (strcmp(tokens[j], ">") == 0) {
                if (cmd.output_file) {
                    fprintf(stderr, "Error: Multiple output redirections not allowed.\n");
                    exit(EXIT_FAILURE);
                }

                if (is_empty(tokens[j + 1]) || strcmp(tokens[j + 1], "<") == 0 || strcmp(tokens[j + 1], ">") == 0 || strcmp(tokens[j + 1], ">>") == 0) {
                    fprintf(stderr, "Error: Invalid filename after '>'.\n");
                    exit(EXIT_FAILURE);
                }
                cmd.output_file = tokens[j + 1];
                j++;
            }
            else if (strcmp(tokens[j], ">>") == 0) {
                if (cmd.output_file) {
                    fprintf(stderr, "Error: Multiple output redirections not allowed.\n");
                    exit(EXIT_FAILURE);
                }

                if (is_empty(tokens[j + 1]) || strcmp(tokens[j + 1], "<") == 0 || strcmp(tokens[j + 1], ">") == 0 || strcmp(tokens[j + 1], ">>") == 0) {
                    fprintf(stderr, "Error: Invalid filename after '>>'.\n");
                    exit(EXIT_FAILURE);
                }
                cmd.output_file = tokens[j + 1];
                cmd.append_mode = 1;
                j++;
            }
            else {
                cmd.args[cmd.num_args++] = tokens[j];
            }
        }
        cmd.args[cmd.num_args] = NULL;
       
        pid_t pid = fork();
        if (pid == 0) {
            if (i > 0) {
                dup2(prev_fd[0], STDIN_FILENO);
                close(prev_fd[0]);
                close(prev_fd[1]);
            }
            if (i < num_cmds - 1) {
                close(pipefd[0]);
                dup2(pipefd[1], STDOUT_FILENO);
                close(pipefd[1]);
            }
            if (cmd.input_file) {
                int input_fd = open(cmd.input_file, O_RDONLY);
                if (input_fd == -1) {
                    fprintf(stderr, "Error: Cannot open input file '%s'. %s.\n", cmd.input_file, strerror(errno));
                    exit(EXIT_FAILURE);
                }
                dup2(input_fd, STDIN_FILENO);
                close(input_fd);
            }
            if (cmd.output_file) {
                int output_fd;
                if (cmd.append_mode) {
                    output_fd = open(cmd.output_file, O_WRONLY | O_CREAT | O_APPEND, 0644);
                } else {
                    output_fd = open(cmd.output_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                }
                if (output_fd == -1) {
                    fprintf(stderr, "Error: Cannot open output file '%s'. %s.\n", cmd.output_file, strerror(errno));
                    exit(EXIT_FAILURE);
                }
                dup2(output_fd, STDOUT_FILENO);
                close(output_fd);
            }
            execvp(cmd.args[0], cmd.args);
            fprintf(stderr, "Error: exec() failed. %s.\n", strerror(errno));
            exit(EXIT_FAILURE);
        } else if (pid > 0) {
            if (i > 0) {
                close(prev_fd[0]);
                close(prev_fd[1]);
            }
            if (i < num_cmds - 1) {
                prev_fd[0] = pipefd[0];
                prev_fd[1] = pipefd[1];
                close(pipefd[1]);
            }
        } else {
            fprintf(stderr, "Error: fork() failed. %s.\n", strerror(errno));
            return;
        }
    }

    for (int i = 0; i < num_cmds; i++) {
        wait(NULL);
    }
}


void print_prompt() {
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)) == NULL) {
        fprintf(stderr, "Error: Cannot get current working directory. %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }
    printf("[%s%s%s]$ ", BRIGHTBLUE, cwd, DEFAULT);
    fflush(stdout);
}

char *parse_cd_argument(char *input) {
    static char parsed[PATH_MAX];
    int j = 0;
    int quote_count = 0;

    for (int i = 0; input[i] != '\0'; i++) {
        if (input[i] == '"') {
            quote_count++;
            continue; // skip quotes
        }
        parsed[j++] = input[i];
    }

    if (quote_count % 2 != 0) {
        fprintf(stderr, "Error: Missing closing quote in cd path.\n");
        exit(EXIT_FAILURE);
    }

    parsed[j] = '\0';
    return parsed;
}


int split_args_quoted(char *line, char **args, int max_args) {
    int i = 0;
    int in_quote = 0;
    char *p = line;
    char *start = NULL;

    while (*p && i < max_args - 1) {
        while (*p == ' ') p++;

        if (*p == '\0') break;

        start = p;
        in_quote = 0;

        while (*p && (in_quote || *p != ' ')) {
            if (*p == '"') in_quote = !in_quote;
            p++;
        }

        if (*p) *p++ = '\0';

        if (strchr(start, '"')) {
            args[i++] = parse_cd_argument(start);
        } else {
            args[i++] = start;
        }
    }

    args[i] = NULL;
    return i;
}

int main() {
    struct sigaction sa;
    sa.sa_handler = handle_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    if (sigaction(SIGINT, &sa, NULL) == -1) {
        fprintf(stderr, "Error: Cannot register signal handler. %s.\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    while (1) {
        if (interrupted) {
            interrupted = 0;
            continue;
        }
	print_prompt();


        char command[CMD_BUFFER_SIZE];
        if (fgets(command, sizeof(command), stdin) == NULL) {
            if (interrupted) {
                interrupted = 0;
                
        	continue;
            }
            fprintf(stderr, "Error: Failed to read from stdin. %s.\n", strerror(errno));
            exit(EXIT_FAILURE);
        }

	command[strcspn(command, "\n")] = '\0';

    	char input_copy[CMD_BUFFER_SIZE];
    	strncpy(input_copy, command, CMD_BUFFER_SIZE);
    	input_copy[CMD_BUFFER_SIZE - 1] = '\0';

    	if (strcmp(input_copy, "exit") == 0) {
        	exit(EXIT_SUCCESS);
    	}

        char *cmd = strtok(input_copy, " ");
        if (cmd == NULL) continue;

        if (strcmp(cmd, "cd") == 0) {
            char *raw_arg = strtok(NULL, "");
            if (raw_arg == NULL || strcmp(raw_arg, "~") == 0) {
                struct passwd *pw = getpwuid(getuid());
                if (!pw || chdir(pw->pw_dir) != 0) {
                    fprintf(stderr, "Error: Cannot change directory to home. %s.\n", strerror(errno));
                }
            } else {
                char *arg = parse_cd_argument(raw_arg);
                if (arg[0] == '~') {
                    struct passwd *pw = getpwuid(getuid());
                    char full_path[PATH_MAX];
                    snprintf(full_path, sizeof(full_path), "%s%s", pw->pw_dir, arg + 1);
                    if (chdir(full_path) != 0) {
                        fprintf(stderr, "Error: Cannot change directory to '%s'. %s.\n", full_path, strerror(errno));
                    }
                } else {
                    if (chdir(arg) != 0) {
                        fprintf(stderr, "Error: Cannot change directory to '%s'. %s.\n", arg, strerror(errno));
                    }
                }
            }
        } else {
            char input_copy[CMD_BUFFER_SIZE];
            strncpy(input_copy, command, CMD_BUFFER_SIZE);
            input_copy[CMD_BUFFER_SIZE - 1] = '\0';

            if (strchr(input_copy, '|')) {
                execute_pipeline(input_copy);
                continue;
            }

	    
	    char *args[MAX_ARGS];
	    split_args_quoted(command, args, MAX_ARGS);

            signal(SIGINT, SIG_IGN);
            pid_t pid = fork();
            if (pid == 0) {
		signal(SIGINT, SIG_DFL);
                execvp(args[0], args);
                fprintf(stderr, "Error: exec() failed. %s.\n", strerror(errno));
                exit(EXIT_FAILURE);
            } else if (pid > 0) {
                int status;
                if (waitpid(pid, &status, 0) == -1) {
                    fprintf(stderr, "Error: wait() failed. %s.\n", strerror(errno));
                }
		signal(SIGINT, handle_sigint);
            } else {
                fprintf(stderr, "Error: fork() failed. %s.\n", strerror(errno));
                exit(EXIT_FAILURE);
            }
        }
    }
}

