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

// Description for each pipeline command
typedef struct {
    char *args[MAX_ARGS];
    int num_args;
    char *input_file;
    char *output_file;
    int append_mode;
} command_t;

volatile sig_atomic_t interrupted = 0;
static char prev_dir[PATH_MAX] = "";

// Count number of pipes
static int count_pipes_outside_quotes(const char *s) {
    int count = 0;
    int in_q = 0;
    char q = 0;
    size_t i;

    for (i = 0; s[i]; ++i) {
        char ch = s[i];
        if (ch == '\'' || ch == '"') {
            if (!in_q) { in_q = 1; q = ch; }
            else if (ch == q) { in_q = 0; }
        } else if (!in_q && ch == '|') {
            count++;
        }
    }
    return count;
}

static int bad_pipe_syntax_raw(const char *s) {
    int in_q = 0;
    char q = 0;
    int saw_token = 0;
    int saw_pipe = 0;
    size_t i;

    for (i = 0; s[i]; ++i) {
        char ch = s[i];
        if (ch == '\'' || ch == '"') {
            if (!in_q) { in_q = 1; q = ch; saw_token = 1; }
            else if (ch == q) { in_q = 0; }
        } else if (!in_q && ch == '|') {
            if (!saw_token) return 1;
            saw_token = 0;
            saw_pipe = 1;
        } else if (!isspace((unsigned char)ch)) {
            saw_token = 1;
        }
    }
    if (saw_pipe && !saw_token) return 1;
    return 0;
}

// Frees memory allocated by tokenize.
static void free_tokens(char **toks) {
    int i;
    if (!toks) return;
    for (i = 0; toks[i]; i++) {
        free(toks[i]);
    }
}

int is_empty(const char *s) {
    if (!s) return 1;
    for (; *s; ++s) {
        if (!isspace((unsigned char)*s)) return 0;
    }
    return 1;
}

// Frees partially built token arrays on error.
static void free_partial_tokens(char **tokens, int count) {
    int i;
    for (i = 0; i < count; ++i) {
        free(tokens[i]);
        tokens[i] = NULL;
    }
    if (count > 0) tokens[0] = NULL;
}

// Creates a token out of a slice
static int make_token(const char *input, int start, int end,
                      char **tokens, int *p_num_tokens, int max_tokens) {
    int token_len;
    char *buf;
    if (*p_num_tokens >= max_tokens - 1) {
        fprintf(stderr, "Error: Too many tokens (limit %d).\n", max_tokens - 1);
        free_partial_tokens(tokens, *p_num_tokens);
        *p_num_tokens = 0;
        return -1;
    }
    token_len = end - start;
    if (token_len < 0) token_len = 0;
    buf = (char *)malloc((size_t)token_len + 1);
    if (!buf) {
        fprintf(stderr, "Error: Out of memory while tokenizing.\n");
        free_partial_tokens(tokens, *p_num_tokens);
        *p_num_tokens = 0;
        return -1;
    }
    if (token_len > 0) memcpy(buf, input + start, (size_t)token_len);
    buf[token_len] = '\0';
    tokens[*p_num_tokens] = buf;
    (*p_num_tokens)++;
    return 0;
}

// Tokenizer
int tokenize(const char *input, char **tokens, const char *delim, int max_tokens) {
    int num_tokens = 0;
    int in_quotes = 0;
    char quote_char = 0;
    int token_start = -1;
    int len = (int)strlen(input);
    int i;

    for (i = 0; i < len; i++) {
        char c = input[i];

        if ((c == '"' || c == '\'') && !in_quotes) {
            in_quotes = 1;
            quote_char = c;
            if (token_start == -1) {
                token_start = i + 1;
            }
        } else if (c == quote_char && in_quotes) {
            int end;
            in_quotes = 0;
            if (token_start != -1) {
                end = i;
                if (strchr(delim, ' ')) {
                    while (end > token_start &&
                           isspace((unsigned char)input[end - 1])) {
                        end--;
                    }
                }
                if (make_token(input, token_start, end, tokens, &num_tokens, max_tokens) != 0) {
                    tokens[0] = NULL;
                    return 0;
                }
                token_start = -1;
            }
        }
        else if (!in_quotes && strchr(delim, c)) {
            if (token_start != -1) {
                if (make_token(input, token_start, i, tokens, &num_tokens, max_tokens) != 0) {
                    tokens[0] = NULL;
                    return 0;
                }
                token_start = -1;
            }
        }
        else if (token_start == -1 && !isspace((unsigned char)c)) {
            token_start = i;
        }
    }

    if (in_quotes) {
        fprintf(stderr, "Error: Missing closing quote.\n");
        free_partial_tokens(tokens, num_tokens);
        return 0;
    }

    if (token_start != -1) {
        int end = len;
        if (strchr(delim, ' ')) {
            while (end > token_start &&
                   isspace((unsigned char)input[end - 1])) {
                end--;
            }
        }
        if (make_token(input, token_start, end, tokens, &num_tokens, max_tokens) != 0) {
            tokens[0] = NULL;
            return 0;
        }
    }

    tokens[num_tokens] = NULL;
    return num_tokens;
}

void handle_sigint(int sig) {
    (void)sig;
    interrupted = 1;
    write(STDOUT_FILENO, "\n", 1);
}

// Inserts spaces around arrows and pipes
static int space_operators(const char *in, char *out, size_t outsz) {
    int in_quotes = 0;
    char quote = 0;
    size_t k = 0;
    size_t limit;

    if (!in || !out || outsz == 0) return 0;
    limit = outsz - 1;

    #define PUT_CH(CH) do { \
        if (k >= limit) { out[0] = '\0'; fprintf(stderr, "Error: Command too long.\n"); return 0; } \
        out[k++] = (CH); \
    } while (0)

    #define PUT_SPACE_IF_NEEDED() do { \
        if (k > 0 && !isspace((unsigned char)out[k - 1])) { PUT_CH(' '); } \
    } while (0)

    for (size_t i = 0; in[i] != '\0'; ++i) {
        char c = in[i];

        if ((c == '"' || c == '\'') && !in_quotes) {
            in_quotes = 1;
            quote = c;
            PUT_CH(c);
            continue;
        } else if (in_quotes) {
            PUT_CH(c);
            if (c == quote) in_quotes = 0;
            continue;
        }

        if (c == '|') {
            PUT_SPACE_IF_NEEDED();
            PUT_CH('|');
            if (in[i + 1] != '\0' && !isspace((unsigned char)in[i + 1])) {
                PUT_CH(' ');
            }
            continue;
        }
        if (c == '<') {
            PUT_SPACE_IF_NEEDED();
            PUT_CH('<');
            if (in[i + 1] != '\0' && !isspace((unsigned char)in[i + 1])) {
                PUT_CH(' ');
            }
            continue;
        }
        if (c == '>') {
            if (in[i + 1] == '>') {
                PUT_SPACE_IF_NEEDED();
                PUT_CH('>');
                PUT_CH('>');
                i++;
                if (in[i + 1] != '\0' && !isspace((unsigned char)in[i + 1])) {
                    PUT_CH(' ');
                }
                continue;
            } else {
                PUT_SPACE_IF_NEEDED();
                PUT_CH('>');
                if (in[i + 1] != '\0' && !isspace((unsigned char)in[i + 1])) {
                    PUT_CH(' ');
                }
                continue;
            }
        }

        PUT_CH(c);
    }

    out[k] = '\0';
    return 1;

    #undef PUT_CH
    #undef PUT_SPACE_IF_NEEDED
}

// Parses everything
void execute_pipeline(char *input) {
    if (bad_pipe_syntax_raw(input)) {
        fprintf(stderr, "Error: Invalid pipeline syntax.\n");
        return;
    }
    if (count_pipes_outside_quotes(input) + 1 > MAX_PIPE_CMDS) {
        fprintf(stderr, "Error: Too many pipeline commands (limit %d).\n", MAX_PIPE_CMDS);
        return;
    }

    char spaced_input[CMD_BUFFER_SIZE];
    if (!space_operators(input, spaced_input, sizeof(spaced_input))) {
        return;
    }

    char *cmds[MAX_PIPE_CMDS];
    int num_cmds = tokenize(spaced_input, cmds, "|", MAX_PIPE_CMDS);
    if (num_cmds == 0) {
        return;
    }

    pid_t children[MAX_PIPE_CMDS];
    int nchildren = 0;

    int prev_fd[2] = {-1, -1};

    for (int i = 0; i < num_cmds; i++) {
        int pipefd[2] = {-1, -1};

        // Pipe for everything except last stage
        if (i < num_cmds - 1) {
            if (pipe(pipefd) == -1) {
                fprintf(stderr, "Error: pipe() failed. %s.\n", strerror(errno));
                if (prev_fd[0] != -1) { close(prev_fd[0]); prev_fd[0] = -1; }
                free_tokens(cmds);
                return;
            }
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
                if (pipefd[0] != -1) close(pipefd[0]);
                if (pipefd[1] != -1) close(pipefd[1]);
            }
            if (prev_fd[0] != -1) { close(prev_fd[0]); prev_fd[0] = -1; }
            continue;
        }

        // Catch commands that start with a redirection operator
        if (strcmp(tokens[0], ">") == 0 || strcmp(tokens[0], ">>") == 0 || strcmp(tokens[0], "<") == 0) {
            fprintf(stderr, "Error: Invalid Command.\n");
            if (i < num_cmds - 1) {
                if (pipefd[0] != -1) close(pipefd[0]);
                if (pipefd[1] != -1) close(pipefd[1]);
            }
            if (prev_fd[0] != -1) { close(prev_fd[0]); prev_fd[0] = -1; }
            free_tokens(tokens);
            continue;
        }

        // Parse argv and redirections for this stage
        int parse_error = 0;
        for (int j = 0; j < num_tokens; j++) {
            if (strcmp(tokens[j], "<") == 0) {
                if (cmd.input_file) {
                    fprintf(stderr, "Error: Multiple input redirections not allowed.\n");
                    parse_error = 1; break;
                }
                if (j + 1 >= num_tokens) {
                    fprintf(stderr, "Error: Missing filename after '<'.\n");
                    parse_error = 1; break;
                }
                if (is_empty(tokens[j + 1]) ||
                    strcmp(tokens[j + 1], "<") == 0 ||
                    strcmp(tokens[j + 1], ">") == 0 ||
                    strcmp(tokens[j + 1], ">>") == 0) {
                    fprintf(stderr, "Error: Invalid filename after '<'.\n");
                    parse_error = 1; break;
                }
                cmd.input_file = tokens[j + 1];
                j++;
            } else if (strcmp(tokens[j], ">") == 0) {
                if (cmd.output_file) {
                    fprintf(stderr, "Error: Multiple output redirections not allowed.\n");
                    parse_error = 1; break;
                }
                if (j + 1 >= num_tokens) {
                    fprintf(stderr, "Error: Missing filename after '>'.\n");
                    parse_error = 1; break;
                }
                if (is_empty(tokens[j + 1]) ||
                    strcmp(tokens[j + 1], "<") == 0 ||
                    strcmp(tokens[j + 1], ">") == 0 ||
                    strcmp(tokens[j + 1], ">>") == 0) {
                    fprintf(stderr, "Error: Invalid filename after '>'.\n");
                    parse_error = 1; break;
                }
                cmd.output_file = tokens[j + 1];
                j++;
            } else if (strcmp(tokens[j], ">>") == 0) {
                if (cmd.output_file) {
                    fprintf(stderr, "Error: Multiple output redirections not allowed.\n");
                    parse_error = 1; break;
                }
                if (j + 1 >= num_tokens) {
                    fprintf(stderr, "Error: Missing filename after '>>'.\n");
                    parse_error = 1; break;
                }
                if (is_empty(tokens[j + 1]) ||
                    strcmp(tokens[j + 1], "<") == 0 ||
                    strcmp(tokens[j + 1], ">") == 0 ||
                    strcmp(tokens[j + 1], ">>") == 0) {
                    fprintf(stderr, "Error: Invalid filename after '>>'.\n");
                    parse_error = 1; break;
                }
                cmd.output_file = tokens[j + 1];
                cmd.append_mode = 1;
                j++;
            } else {
                cmd.args[cmd.num_args++] = tokens[j];
                if (cmd.num_args >= MAX_ARGS - 1) {
                    fprintf(stderr, "Error: Too many arguments (limit %d).\n", MAX_ARGS - 1);
                    parse_error = 1; break;
                }
            }
        }

        cmd.args[cmd.num_args] = NULL;

        if (parse_error || cmd.num_args == 0) {
            if (i < num_cmds - 1) {
                if (pipefd[0] != -1) close(pipefd[0]);
                if (pipefd[1] != -1) close(pipefd[1]);
            }
            if (prev_fd[0] != -1) { close(prev_fd[0]); prev_fd[0] = -1; }
            free_tokens(tokens);
            continue;
        }

        pid_t pid = fork();
        if (pid == 0) {
            // Run the command with exec
            signal(SIGINT, SIG_DFL);

            if (i > 0) {
                if (prev_fd[0] != -1) {
                    dup2(prev_fd[0], STDIN_FILENO);
                }
                if (prev_fd[0] != -1) close(prev_fd[0]);
                if (prev_fd[1] != -1) close(prev_fd[1]);
            }

            if (i < num_cmds - 1) {
                if (pipefd[0] != -1) close(pipefd[0]);
                if (pipefd[1] != -1) {
                    dup2(pipefd[1], STDOUT_FILENO);
                    close(pipefd[1]);
                }
            }

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
            // Close pipes
            if (nchildren < MAX_PIPE_CMDS) {
                children[nchildren++] = pid;
            }

            if (i > 0) {
                if (prev_fd[0] != -1) { close(prev_fd[0]); prev_fd[0] = -1; }
                if (prev_fd[1] != -1) { close(prev_fd[1]); prev_fd[1] = -1; }
            }

            if (i < num_cmds - 1) {
                prev_fd[0] = pipefd[0];
                if (pipefd[1] != -1) close(pipefd[1]);
                prev_fd[1] = -1;
            }

            free_tokens(tokens);
        } else {
            fprintf(stderr, "Error: fork() failed. %s.\n", strerror(errno));
            if (i < num_cmds - 1) {
                if (pipefd[0] != -1) close(pipefd[0]);
                if (pipefd[1] != -1) close(pipefd[1]);
            }
            if (prev_fd[0] != -1) { close(prev_fd[0]); prev_fd[0] = -1; }
            free_tokens(tokens);
            continue;
        }
    }

    // Waits for correct number of children
    for (int i = 0; i < nchildren; i++) {
        int status;
        while (waitpid(children[i], &status, 0) == -1 && errno == EINTR) {}
    }

    free_tokens(cmds);
}

void print_prompt(void) {
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)) == NULL) {
        fprintf(stderr, "Error: Cannot get current working directory. %s\n",
                strerror(errno));
        strcpy(cwd, "?");
    }
    printf("[%s%s%s]$ ", BRIGHTBLUE, cwd, DEFAULT);
    fflush(stdout);
}

// Main
int main(void) {
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
                interrupted = 0;
                continue;
            }
            if (feof(stdin)) {
                putchar('\n');
                exit(EXIT_SUCCESS);
            }
            if (ferror(stdin)) {
                int err = errno;
                fprintf(stderr, "Error: Failed to read from stdin. %s.\n",
                        strerror(err));
                exit(EXIT_FAILURE);
            }
        }

        command[strcspn(command, "\n")] = '\0';
        if (command[0] == '\0') continue;

        if (bad_pipe_syntax_raw(command)) {
            fprintf(stderr, "Error: Invalid pipeline syntax.\n");
            continue;
        }
        if (count_pipes_outside_quotes(command) + 1 > MAX_PIPE_CMDS) {
            fprintf(stderr, "Error: Too many pipeline commands (limit %d).\n", MAX_PIPE_CMDS);
            continue;
        }

        char *argv_tokens[MAX_ARGS];
        int argc_tokens = tokenize(command, argv_tokens, " \t\r\n", MAX_ARGS);
        if (argc_tokens == 0) {
            continue;
        }

        if (strcmp(argv_tokens[0], "exit") == 0) {
            if (argc_tokens == 1) {
                free_tokens(argv_tokens);
                exit(0);
            } else if (argc_tokens == 2) {
                char *endp = NULL;
                long val;
                errno = 0;
                val = strtol(argv_tokens[1], &endp, 10);
                if (errno != 0 || endp == argv_tokens[1] || *endp != '\0') {
                    fprintf(stderr, "exit: %s: numeric argument required\n", argv_tokens[1]);
                    free_tokens(argv_tokens);
                    exit(2);
                }
                free_tokens(argv_tokens);
                exit((int)val);
            } else {
                fprintf(stderr, "exit: too many arguments\n");
                free_tokens(argv_tokens);
                continue;
            }
        }

        if (strcmp(argv_tokens[0], "cd") == 0) {
            int rc = 0;

            if (argc_tokens == 1 || (argc_tokens == 2 && strcmp(argv_tokens[1], "~") == 0)) {
                struct passwd *pw = getpwuid(getuid());
                if (!pw) {
                    fprintf(stderr, "Error: Cannot resolve home directory. %s.\n", strerror(errno));
                    free_tokens(argv_tokens);
                    continue;
                }
                char old[PATH_MAX];
                if (!getcwd(old, sizeof(old))) old[0] = '\0';

                rc = chdir(pw->pw_dir);
                if (rc != 0) {
                    fprintf(stderr, "Error: Cannot change directory to home. %s.\n", strerror(errno));
                } else if (old[0]) {
                    strncpy(prev_dir, old, sizeof(prev_dir));
                    prev_dir[sizeof(prev_dir)-1] = '\0';
                }
                free_tokens(argv_tokens);
                continue;
            }

            if (argc_tokens > 2) {
                fprintf(stderr, "cd: too many arguments\n");
                free_tokens(argv_tokens);
                continue;
            }

            const char *arg_in = argv_tokens[1];

            if (strcmp(arg_in, "-") == 0) {
                if (prev_dir[0] == '\0') {
                    fprintf(stderr, "cd: OLDPWD not set\n");
                    free_tokens(argv_tokens);
                    continue;
                }
                char old[PATH_MAX];
                if (!getcwd(old, sizeof(old))) old[0] = '\0';

                rc = chdir(prev_dir);
                if (rc != 0) {
                    fprintf(stderr, "Error: Cannot change directory to '%s'. %s.\n", prev_dir, strerror(errno));
                } else {
                    printf("%s\n", prev_dir);
                    fflush(stdout);
                    if (old[0]) {
                        strncpy(prev_dir, old, sizeof(prev_dir));
                        prev_dir[sizeof(prev_dir)-1] = '\0';
                    }
                }
                free_tokens(argv_tokens);
                continue;
            }

            char target[PATH_MAX];
            if (arg_in[0] == '~') {
                struct passwd *pw = getpwuid(getuid());
                if (!pw) {
                    fprintf(stderr, "Error: Cannot resolve home directory. %s.\n", strerror(errno));
                    free_tokens(argv_tokens);
                    continue;
                }
                snprintf(target, sizeof(target), "%s%s", pw->pw_dir, arg_in + 1);
            } else {
                snprintf(target, sizeof(target), "%s", arg_in);
            }

            char old[PATH_MAX];
            if (!getcwd(old, sizeof(old))) old[0] = '\0';

            rc = chdir(target);
            if (rc != 0) {
                fprintf(stderr, "Error: Cannot change directory to '%s'. %s.\n", target, strerror(errno));
            } else if (old[0]) {
                strncpy(prev_dir, old, sizeof(prev_dir));
                prev_dir[sizeof(prev_dir)-1] = '\0';
            }

            free_tokens(argv_tokens);
            continue;
        }

        free_tokens(argv_tokens);
        execute_pipeline(command);
    }
}


