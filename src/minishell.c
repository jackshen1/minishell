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
#include <ctype.h>

#define CMD_BUFFER_SIZE 1024
#define BRIGHTBLUE "\x1b[34;1m"
#define DEFAULT "\x1b[0m"
#define MAX_ARGS 2048
#define MAX_PIPE_CMDS 64

typedef struct {
    char *args[MAX_ARGS];    // command + arguments
    int num_args;
    char *input_file;        // NULL if no input redirection
    char *output_file;       // NULL if no output redirection
    int append_mode;         // 0 for >, 1 for >>
} command_t;

volatile sig_atomic_t interrupted = 0;

// Tokenize input string by delimiter, handling quotes
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
        } else if (c == quote_char && in_quotes) {
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
        // Need space for one more token plus the trailing NULL slot
        if (num_tokens >= max_tokens - 1) {
            tokens[num_tokens] = NULL;
            return num_tokens;  // truncated: no room for final token
        }
        
        int token_len = len - token_start;
        // Trim trailing whitespace from the final token
        while (token_len > 0 && isspace((unsigned char)input[token_start + token_len - 1])) {
            token_len--;
        }
        
        tokens[num_tokens] = (char *)malloc((size_t)token_len + 1);
        if (!tokens[num_tokens]) {
            tokens[num_tokens] = NULL;
            return num_tokens;  // OOM: return what we have
        }
        
        memcpy(tokens[num_tokens], input + token_start, (size_t)token_len);
        tokens[num_tokens][token_len] = '\0';
        num_tokens++;
    }
    
    tokens[num_tokens] = NULL;
    return num_tokens;
}

// Free token array allocated by tokenize()
static void free_tokens(char **toks) {
    if (!toks) return;
    for (int i = 0; toks[i]; i++) {
        free(toks[i]);
    }
}

// Check if string is empty or contains only whitespace
int is_empty(const char *s) {
    if (!s) return 1;
    for (; *s; ++s) {
        if (!isspace((unsigned char)*s)) return 0;
    }
    return 1;
}

// SIGINT handler - sets flag for main loop
void handle_sigint(int sig) {
    interrupted = 1;
    write(STDOUT_FILENO, "\n", 1);
}

// Insert spaces around |, <, >, and >> while respecting quotes.
// out is always NUL-terminated.
static void space_operators(const char *in, char *out, size_t outsz) {
    int in_quotes = 0;
    char quote = 0;
    size_t k = 0;

    if (!in || !out || outsz == 0) return;
    // Keep one char for the trailing NUL
    outsz--;

    for (size_t i = 0; in[i] != '\0' && k < outsz; ++i) {
        char c = in[i];

        // Quote handling
        if ((c == '"' || c == '\'') && !in_quotes) {
            in_quotes = 1;
            quote = c;
            out[k++] = c;
            continue;
        } else if (in_quotes) {
            out[k++] = c;
            if (c == quote) in_quotes = 0;
            continue;
        }

        // Outside quotes: space out operators
        if (c == '|') {
            if (k > 0 && !isspace((unsigned char)out[k - 1]) && k < outsz) out[k++] = ' ';
            if (k < outsz) out[k++] = '|';
            if (in[i + 1] != '\0' && !isspace((unsigned char)in[i + 1]) && k < outsz) out[k++] = ' ';
            continue;
        }
        if (c == '<') {
            if (k > 0 && !isspace((unsigned char)out[k - 1]) && k < outsz) out[k++] = ' ';
            if (k < outsz) out[k++] = '<';
            if (in[i + 1] != '\0' && !isspace((unsigned char)in[i + 1]) && k < outsz) out[k++] = ' ';
            continue;
        }
        if (c == '>') {
            // Handle >>
            if (in[i + 1] == '>') {
                if (k > 0 && !isspace((unsigned char)out[k - 1]) && k < outsz) out[k++] = ' ';
                if (k < outsz) out[k++] = '>';
                if (k < outsz) out[k++] = '>';
                i++;
                if (in[i + 1] != '\0' && !isspace((unsigned char)in[i + 1]) && k < outsz) out[k++] = ' ';
                continue;
            } else {
                if (k > 0 && !isspace((unsigned char)out[k - 1]) && k < outsz) out[k++] = ' ';
                if (k < outsz) out[k++] = '>';
                if (in[i + 1] != '\0' && !isspace((unsigned char)in[i + 1]) && k < outsz) out[k++] = ' ';
                continue;
            }
        }

        // Default: copy byte
        out[k++] = c;
    }
    out[k] = '\0';
}

// Execute a pipeline of commands separated by pipes
void execute_pipeline(char *input) {
    // NEW: normalize operators so tokenize sees them as separate tokens
    char spaced_input[CMD_BUFFER_SIZE];
    space_operators(input, spaced_input, sizeof(spaced_input));

    char *cmds[MAX_PIPE_CMDS];
    int num_cmds = tokenize(spaced_input, cmds, "|", MAX_PIPE_CMDS);
    int prev_fd[2] = {-1, -1};
    
    for (int i = 0; i < num_cmds; i++) {
        int pipefd[2];
        
        // Create pipe for all but last command
        if (i < num_cmds - 1 && pipe(pipefd) == -1) {
            fprintf(stderr, "Error: pipe() failed. %s.\n", strerror(errno));
            free_tokens(cmds);
            return;
        }
        
        command_t cmd;
        cmd.input_file = NULL;
        cmd.output_file = NULL;
        cmd.append_mode = 0;
        cmd.num_args = 0;
        
        char *tokens[MAX_ARGS];
        int num_tokens = tokenize(cmds[i], tokens, " \t\r\n", MAX_ARGS);
        
        if (num_tokens == 0) {
            fprintf(stderr, "Error: Empty Command.\n");
            if (i < num_cmds - 1) {
                close(pipefd[0]);
                close(pipefd[1]);
            }
            continue;
        }
        
        // Check for invalid command starting with redirection
        if (strcmp(tokens[0], ">") == 0 || strcmp(tokens[0], ">>") == 0 || strcmp(tokens[0], "<") == 0) {
            fprintf(stderr, "Error: Invalid Command.\n");
            if (i < num_cmds - 1) {
                close(pipefd[0]);
                close(pipefd[1]);
            }
            free_tokens(tokens);
            continue;
        }
        
        // Parse tokens for redirections and arguments
        int parse_error = 0;
        for (int j = 0; j < num_tokens; j++) {
            if (strcmp(tokens[j], "<") == 0) {
                if (cmd.input_file) {
                    fprintf(stderr, "Error: Multiple input redirections not allowed.\n");
                    parse_error = 1;
                    break;
                }
                if (j + 1 >= num_tokens) {
                    fprintf(stderr, "Error: Missing filename after '<'.\n");
                    parse_error = 1;
                    break;
                }
                if (is_empty(tokens[j + 1]) || 
                    strcmp(tokens[j + 1], "<") == 0 || 
                    strcmp(tokens[j + 1], ">") == 0 || 
                    strcmp(tokens[j + 1], ">>") == 0) {
                    fprintf(stderr, "Error: Invalid filename after '<'.\n");
                    parse_error = 1;
                    break;
                }
                cmd.input_file = tokens[j + 1];
                j++;
            } else if (strcmp(tokens[j], ">") == 0) {
                if (cmd.output_file) {
                    fprintf(stderr, "Error: Multiple output redirections not allowed.\n");
                    parse_error = 1;
                    break;
                }
                if (j + 1 >= num_tokens) {
                    fprintf(stderr, "Error: Missing filename after '>'.\n");
                    parse_error = 1;
                    break;
                }
                if (is_empty(tokens[j + 1]) || 
                    strcmp(tokens[j + 1], "<") == 0 || 
                    strcmp(tokens[j + 1], ">") == 0 || 
                    strcmp(tokens[j + 1], ">>") == 0) {
                    fprintf(stderr, "Error: Invalid filename after '>'.\n");
                    parse_error = 1;
                    break;
                }
                cmd.output_file = tokens[j + 1];
                j++;
            } else if (strcmp(tokens[j], ">>") == 0) {
                if (cmd.output_file) {
                    fprintf(stderr, "Error: Multiple output redirections not allowed.\n");
                    parse_error = 1;
                    break;
                }
                if (j + 1 >= num_tokens) {
                    fprintf(stderr, "Error: Missing filename after '>>'.\n");
                    parse_error = 1;
                    break;
                }
                if (is_empty(tokens[j + 1]) || 
                    strcmp(tokens[j + 1], "<") == 0 || 
                    strcmp(tokens[j + 1], ">") == 0 || 
                    strcmp(tokens[j + 1], ">>") == 0) {
                    fprintf(stderr, "Error: Invalid filename after '>>'.\n");
                    parse_error = 1;
                    break;
                }
                cmd.output_file = tokens[j + 1];
                cmd.append_mode = 1;
                j++;
            } else {
                cmd.args[cmd.num_args++] = tokens[j];
            }
        }
        
        cmd.args[cmd.num_args] = NULL;

        if (parse_error || cmd.num_args == 0) {
            if (i < num_cmds - 1) {
                close(pipefd[0]);
                close(pipefd[1]);
            }
            free_tokens(tokens);
            continue;
        }
        
        pid_t pid = fork();
        if (pid == 0) {
            // Child process - reset SIGINT to default
            signal(SIGINT, SIG_DFL);
            
            // Set up input from previous pipe
            if (i > 0) {
                dup2(prev_fd[0], STDIN_FILENO);
                close(prev_fd[0]);
                close(prev_fd[1]);
            }
            
            // Set up output to next pipe
            if (i < num_cmds - 1) {
                close(pipefd[0]);
                dup2(pipefd[1], STDOUT_FILENO);
                close(pipefd[1]);
            }
            
            // Handle input redirection
            if (cmd.input_file) {
                int input_fd = open(cmd.input_file, O_RDONLY);
                if (input_fd == -1) {
                    fprintf(stderr, "Error: Cannot open input file '%s'. %s.\n", 
                           cmd.input_file, strerror(errno));
                    _exit(EXIT_FAILURE);
                }
                dup2(input_fd, STDIN_FILENO);
                close(input_fd);
            }
            
            // Handle output redirection
            if (cmd.output_file) {
                int output_fd;
                if (cmd.append_mode) {
                    output_fd = open(cmd.output_file, O_WRONLY | O_CREAT | O_APPEND, 0644);
                } else {
                    output_fd = open(cmd.output_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                }
                if (output_fd == -1) {
                    fprintf(stderr, "Error: Cannot open output file '%s'. %s.\n", 
                           cmd.output_file, strerror(errno));
                    _exit(EXIT_FAILURE);
                }
                dup2(output_fd, STDOUT_FILENO);
                close(output_fd);
            }
            
            execvp(cmd.args[0], cmd.args);
            fprintf(stderr, "Error: exec() failed. %s.\n", strerror(errno));
            _exit(EXIT_FAILURE);
        } else if (pid > 0) {
            // Parent process - close previous pipe
            if (i > 0) {
                close(prev_fd[0]);
                close(prev_fd[1]);
            }
            
            // Set up for next iteration
            if (i < num_cmds - 1) {
                prev_fd[0] = pipefd[0];
                prev_fd[1] = pipefd[1];
                close(pipefd[1]);  // Close write end in parent
            }
            
            free_tokens(tokens);
        } else {
            fprintf(stderr, "Error: fork() failed. %s.\n", strerror(errno));
            if (i < num_cmds - 1) {
                close(pipefd[0]);
                close(pipefd[1]);
            }
            free_tokens(tokens);
            continue;
        }
    }
    
    // Wait for all children to finish
    for (int i = 0; i < num_cmds; i++) {
        wait(NULL);
    }
    
    free_tokens(cmds);
}

// Print shell prompt with current directory
void print_prompt() {
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)) == NULL) {
        fprintf(stderr, "Error: Cannot get current working directory. %s\n", 
               strerror(errno));
        strcpy(cwd, "?");
    }
    printf("[%s%s%s]$ ", BRIGHTBLUE, cwd, DEFAULT);
    fflush(stdout);
}

// Parse cd argument, handling quotes
char *parse_cd_argument(char *input) {
    static char parsed[PATH_MAX];
    int j = 0;
    int quote_count = 0;
    
    for (int i = 0; input[i] != '\0'; i++) {
        if (input[i] == '"') {
            quote_count++;
            continue;  // skip quotes
        }
        parsed[j++] = input[i];
    }
    
    if (quote_count % 2 != 0) {
        fprintf(stderr, "Error: Missing closing quote in cd path.\n");
        return NULL;
    }
    
    parsed[j] = '\0';
    return parsed;
}

int main() {
    // Set up SIGINT handler
    struct sigaction sa;
    sa.sa_handler = handle_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    
    if (sigaction(SIGINT, &sa, NULL) == -1) {
        fprintf(stderr, "Error: Cannot register signal handler. %s.\n", 
               strerror(errno));
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
                // SIGINT hit during read
                interrupted = 0;
                continue;
            }
            if (feof(stdin)) {
                // Clean EOF (Ctrl-D)
                putchar('\n');
                exit(EXIT_SUCCESS);
            }
            if (ferror(stdin)) {
                // Real read error
                int err = errno;
                fprintf(stderr, "Error: Failed to read from stdin. %s.\n", 
                       strerror(err));
                exit(EXIT_FAILURE);
            }
        }
        
        // Remove newline
        command[strcspn(command, "\n")] = '\0';
        
        char input_copy[CMD_BUFFER_SIZE];
        strncpy(input_copy, command, CMD_BUFFER_SIZE);
        input_copy[CMD_BUFFER_SIZE - 1] = '\0';
        
        if (strcmp(input_copy, "exit") == 0) {
            exit(EXIT_SUCCESS);
        }
        
        char *cmd = strtok(input_copy, " ");
        if (cmd == NULL) continue;
        
        // Handle built-in cd command
        if (strcmp(cmd, "cd") == 0) {
            char *raw_arg = strtok(NULL, "");
            
            if (raw_arg == NULL || strcmp(raw_arg, "~") == 0) {
                // cd with no args or cd ~ - go to home directory
                struct passwd *pw = getpwuid(getuid());
                if (!pw || chdir(pw->pw_dir) != 0) {
                    fprintf(stderr, "Error: Cannot change directory to home. %s.\n", 
                           strerror(errno));
                }
            } else {
                char *arg = parse_cd_argument(raw_arg);
                if (!arg) {
                    // parse error already reported; keep shell running
                    continue;
                }
                
                if (arg[0] == '~') {
                    // Handle ~/path syntax
                    struct passwd *pw = getpwuid(getuid());
                    char full_path[PATH_MAX];
                    snprintf(full_path, sizeof(full_path), "%s%s", pw->pw_dir, arg + 1);
                    
                    if (chdir(full_path) != 0) {
                        fprintf(stderr, "Error: Cannot change directory to '%s'. %s.\n", 
                               full_path, strerror(errno));
                    }
                } else {
                    if (chdir(arg) != 0) {
                        fprintf(stderr, "Error: Cannot change directory to '%s'. %s.\n", 
                               arg, strerror(errno));
                    }
                }
            }
        } else {
            // Handle external commands via unified pipeline executor
            execute_pipeline(command);
        }
    }
}



