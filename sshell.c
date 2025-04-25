#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>   // for pid_t
#include <sys/wait.h>    // for waitpid() and WEXITSTATUS()
#include <fcntl.h>       // for open(), O_CREAT, O_TRUNC

#define CMDLINE_MAX    512
#define MAX_ARGUMENT   16
#define MAX_PIPES       3
#define MAX_COMMANDS   (MAX_PIPES + 1)

typedef struct {
    char *argv[MAX_ARGUMENT + 1];
    int   argc;
} Command;

typedef struct {
    Command cmds[MAX_COMMANDS];
    int n_commands;
    int out_redirect;   // 1 if '>' was found (output redirection)
    int in_redirect;    // 1 if '<' was found for INPUT redirection
    char *outfile;  // filename for OUTPUT redirection
    char *infile;   // filename for INPUT redirection
    int background;     // 1 if job ends with &
} Job;

// builtin cd
int cd_builtin(char *args[]) {
    if (!args[1]) {
        fprintf(stderr, "Error: no directory specified\n");
        return 1;
    }
    if (chdir(args[1]) != 0) {
        fprintf(stderr, "Error: cannot cd into directory\n");
        return 1;
    }
    return 0;
}

// insert spaces around '>'(output redirection), '|' (piping), 
//'<'(input redirection), and '&'(background job) so strtok sees them as tokens
void pad_meta(char *cmd) {
    char tmp[CMDLINE_MAX];
    int read_index = 0, write_index = 0;
    while (cmd[read_index] && write_index < CMDLINE_MAX - 1) {
        if (cmd[read_index] == '>' || cmd[read_index] == '|' || cmd[read_index] =='<' || cmd[read_index] =='&') {
            tmp[write_index++] = ' ';
            tmp[write_index++] = cmd[read_index++];
            tmp[write_index++] = ' ';
        } else {
            tmp[write_index++] = cmd[read_index++];
        }
    }
    tmp[write_index] = '\0';
    strncpy(cmd, tmp, CMDLINE_MAX);
}

// open and dup2 for output redirection
void redirect_output(const char *outfile) {
    int fd = open(outfile, O_WRONLY|O_CREAT|O_TRUNC, 0644);
    if (fd < 0) {
        fprintf(stderr, "Error: cannot open output file\n");
        exit(1);
    }
    if (dup2(fd, STDOUT_FILENO) < 0) {
        perror("dup2");
        exit(1);
    }
    close(fd);
}

void redirect_input(const char *infile){
    int fd = open(infile, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "Error: cannot open input file\n");
        exit(1);
    }
    if (dup2(fd, STDIN_FILENO) < 0) {
        perror("dup2");
        exit(1);
    }
    close(fd);
}

// parse a raw command line into a Job struct
// returns 0 on success, -1 on parse error (error message already printed)
int parse_job(char *line, Job *job) {
    pad_meta(line);
    char *tokens[CMDLINE_MAX];
    int ntok = 0;
    char *t = strtok(line, " ");
    while (t) {
        tokens[ntok++] = t;
        t = strtok(NULL, " ");
    }
    if (ntok == 0) {
        job->n_commands = 0;
        return 0;
    }
    for (int i = 0; i < ntok; i++) {
        if (strcmp(tokens[i], "&") == 0 && i != ntok - 1) {
            fprintf(stderr, "Error: mislocated background sign\n");
            return -1;
        }
        if (strchr(tokens[i], '&') && strcmp(tokens[i], "&") != 0) {
            fprintf(stderr, "Error: mislocated background sign\n");
            return -1;
        }
    }

    if (ntok > MAX_ARGUMENT) {
        fprintf(stderr, "Error: too many process arguments\n");
        return -1;
    }

    // handle '&' for background job
    job->background = 0;
    if (ntok > 0 && strcmp(tokens[ntok - 1], "&") == 0) {
        job->background = 1;
        ntok--; // Remove '&' from tokens
        if (ntok == 0) {
            fprintf(stderr, "Error: missing command\n");
            return -1;
        }
    }    

    // handle '>' output redirection
    job->out_redirect = 0;
    job->outfile = NULL;
    int outRedir_pos = -1;
    for (int i = 0; i < ntok; i++) {
        if (strcmp(tokens[i], ">") == 0) {
            outRedir_pos = i;
            break;
        }
    }
    if (outRedir_pos >= 0) {
        if (outRedir_pos == 0) {
            fprintf(stderr, "Error: missing command\n");
            return -1;
        }
        if (outRedir_pos == ntok - 1) {
            fprintf(stderr, "Error: no output file\n");
            return -1;
        }
        job->out_redirect = 1;
        job->outfile  = tokens[outRedir_pos + 1];
        // check mislocated: no '|' after redir
        for (int j = outRedir_pos + 2; j < ntok; j++) {
            if (strcmp(tokens[j], "|") == 0) {
                fprintf(stderr, "Error: mislocated output redirection\n");
                return -1;
            }
        }
        // remove ">" and filename
        for (int j = outRedir_pos; j + 2 < ntok; j++) {
            tokens[j] = tokens[j + 2];
        }
        ntok -= 2;
    }

    // handle '<' input redirection
    job->in_redirect = 0;
    job->infile = NULL;
    int inRedir_pos = -1;
    for (int i = 0; i < ntok; i++) {
        if (strcmp(tokens[i], "<") == 0) {
            inRedir_pos = i;
            break;
        }
    }
    if (inRedir_pos >= 0) {
        if (inRedir_pos == 0) {
            fprintf(stderr, "Error: missing command\n");
            return -1;
        }
        if (inRedir_pos == ntok - 1) {
            fprintf(stderr, "Error: no input file\n");
            return -1;
        }
        // checking for input redirection after a "|"
        for (int j = 0; j < inRedir_pos; j++) {
            if (strcmp(tokens[j], "|") == 0) {
                fprintf(stderr, "Error: mislocated input redirection\n");
                return -1;
            }
        }
        job->in_redirect = 1;
        job->infile = tokens[inRedir_pos + 1];
        // check mislocated: no '|' after redir
        for (int j = inRedir_pos + 2; j < ntok; j++) {
            if (strcmp(tokens[j], "|") == 0) {
                fprintf(stderr, "Error: mislocated input redirection\n");
                return -1;
            }
        }
        // remove "<" and filename
        for (int j = inRedir_pos; j + 2 < ntok; j++) {
            tokens[j] = tokens[j + 2];
        }
        ntok -= 2;
    }

    // detect pipes
    int pipe_pos[MAX_PIPES];
    int pipecount = 0;
    for (int i = 0; i < ntok; i++) {
        if (strcmp(tokens[i], "|") == 0) {
            if (pipecount >= MAX_PIPES) {
                fprintf(stderr, "Error: too many pipes\n");
                return -1;
            } 
            pipe_pos[pipecount++] = i;
        }  
    }
    // validate pipe placement
    for (int p = 0; p < pipecount; p++) {
        int pos = pipe_pos[p];
        if (pos == 0 || pos == ntok - 1) {
            fprintf(stderr, "Error: missing command\n");
            return -1;
        }
        if (p + 1 < pipecount && pipe_pos[p + 1] == pos + 1) {
            fprintf(stderr, "Error: missing command\n");
            return -1;
        }
    }

    // fill Job.commands
    job->n_commands = pipecount + 1;
    int start = 0;
    for (int c = 0; c < job->n_commands; c++) {
        int end = (c < pipecount ? pipe_pos[c] : ntok);
        int argc = end - start;
        for (int j = 0; j < argc; j++) {
            job->cmds[c].argv[j] = tokens[start + j];
        }
        job->cmds[c].argv[argc] = NULL;
        job->cmds[c].argc = argc;
        start = end + 1;
    }

    return 0;
}

// execute a parsed Job: fork/exec single or pipeline
void exec_job(const Job *job, const char *cmdBuffer) {
    int n = job->n_commands;
    if (n == 0) return;

    // single command
    if (n == 1) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            return;
        }
        if (pid == 0) {
            if (job->out_redirect) {
                // this part
                redirect_output(job->outfile);
            }
            if (job->in_redirect){
                redirect_input(job->infile);
            }
            execvp(job->cmds[0].argv[0], job->cmds[0].argv);
            fprintf(stderr, "Error: command not found\n");
            exit(1);
        }
        
        int status;
        waitpid(pid, &status, 0);
        fprintf(stderr, "+ completed '%s' [%d]\n", cmdBuffer, WEXITSTATUS(status));
        /*if (job->background) {
            fprintf(stderr, "+ background pid: %d\n", pid);
        } else {
            int status;
            waitpid(pid, &status, 0);
            fprintf(stderr, "+ completed '%s' [%d]\n", cmdBuffer, WEXITSTATUS(status));
        }*/
        return;
    }

    // pipeline: n >= 2
    // create pipes
    int pipes[MAX_PIPES][2];
    for (int i = 0; i < n - 1; i++) {
        if (pipe(pipes[i]) < 0) {
            perror("pipe");
            exit(1);
        }
    }

    pid_t pids[MAX_COMMANDS];
    // i = the ordering of commands in a pipeline, i = 0 is the first command, i = 1 is the second command etc. 
    for (int i = 0; i < n; i++) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            exit(1);
        }
        if (pid == 0) {
            // child i
            if (i > 0) {
                dup2(pipes[i - 1][0], STDIN_FILENO);
            }
            if (i < n - 1) {
                dup2(pipes[i][1], STDOUT_FILENO);
            } else if (job->out_redirect) {
                redirect_output(job->outfile);
            }
            if (i == 0 && job-> in_redirect) {
                redirect_input(job->infile);
            }
            // close all pipes
            for (int j = 0; j < n - 1; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }
            execvp(job->cmds[i].argv[0], job->cmds[i].argv);
            fprintf(stderr, "Error: command not found\n");
            exit(1);
        }
        pids[i] = pid;
    }

    // parent: close pipes
    for (int i = 0; i < n - 1; i++) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }

    // wait and collect statuses
    int statuses[MAX_COMMANDS];
    for (int i = 0; i < n; i++) {
        int st;
        waitpid(pids[i], &st, 0);
        statuses[i] = WEXITSTATUS(st);
        /*if (job->background) {
            fprintf(stderr, "+ background pid: %d\n", pids[n - 1]);  // or track all if needed
        } else {
            int st;
            for (int i = 0; i < n; i++) {
                waitpid(pids[i], &st, 0);
            }
            fprintf(stderr, "+ completed '%s' [%d]\n", cmdBuffer, WEXITSTATUS(st));
        }*/
    }

    // print combined completion
    fprintf(stderr, "+ completed '%s'", cmdBuffer);
    for (int i = 0; i < n; i++) {
        fprintf(stderr, " [%d]", statuses[i]);
    }
    fprintf(stderr, "\n");
}

int main(void) {
    char cmd[CMDLINE_MAX];

    while (1) {
        printf("sshell@ucd$ ");
        fflush(stdout);

        if (!fgets(cmd, CMDLINE_MAX, stdin)) {
            strncpy(cmd, "exit\n", CMDLINE_MAX);
        }
        if (!isatty(STDIN_FILENO)) {
            printf("%s", cmd);
            fflush(stdout);
        }

        char *nl = strchr(cmd, '\n');
        if (nl) *nl = '\0';

        char cmdBuffer[CMDLINE_MAX];
        strncpy(cmdBuffer, cmd, CMDLINE_MAX);

        if (strcmp(cmd, "exit") == 0) {
            fprintf(stderr, "Bye...\n");
            fprintf(stderr, "+ completed 'exit' [0]\n");
            break;
        }
        if (strcmp(cmd, "pwd") == 0) {
            char *cwd = getcwd(NULL, 0);
            if (cwd) {
                printf("%s\n", cwd);
                free(cwd);
            } else {
                perror("getcwd");
            }
            fprintf(stderr, "+ completed 'pwd' [0]\n");
            continue;
        }

        Job job;
        if (parse_job(cmd, &job) != 0) {
            // parse_job already printed error
            continue;
        }
        if (job.n_commands == 0) {
            // empty line
            continue;
        }
        // builtin cd only when single command
        if (job.n_commands == 1 &&
            strcmp(job.cmds[0].argv[0], "cd") == 0) {
            int st = cd_builtin(job.cmds[0].argv);
            fprintf(stderr, "+ completed '%s' [%d]\n",
                    cmdBuffer, st);
            continue;
        }

        exec_job(&job, cmdBuffer);
    }

    return EXIT_SUCCESS;
}