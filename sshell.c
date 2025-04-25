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
    int commands;
    int outputRed;   
    int InputRed;  
    char *outfile;  
    char *infile;  
    int background;     
} Job;


int cd_build(char *args[]) {
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


void pad(char *cmd) {
    char copyc[CMDLINE_MAX];
    int firstI = 0, SecondI = 0;
    while (cmd[firstI] && SecondI < CMDLINE_MAX - 1) {
        if (cmd[firstI] == '>' || cmd[firstI] == '|' || cmd[firstI] =='<' || cmd[firstI] =='&') {
            copyc[SecondI++] = ' ';
            copyc[SecondI++] = cmd[firstI++];
            copyc[SecondI++] = ' ';
        } else {
            copyc[SecondI++] = cmd[firstI++];
        }
    }
    copyc[SecondI] = '\0';
    strncpy(cmd, copyc, CMDLINE_MAX);
}


void Outputredirection(const char *outfile) {
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

void Inputredirection(const char *infile){
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

int ParseAll(char *line, Job *job) {
    pad(line);
    char *tokens[CMDLINE_MAX];
    int ntok = 0;
    char *t = strtok(line, " ");
    while (t) {
        tokens[ntok++] = t;
        t = strtok(NULL, " ");
    }
    if (ntok == 0) {
        job->commands = 0;
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

  
    job->background = 0;
    if (ntok > 0 && strcmp(tokens[ntok - 1], "&") == 0) {
        job->background = 1;
        ntok--; 
        if (ntok == 0) {
            fprintf(stderr, "Error: missing command\n");
            return -1;
        }
    }    


    job->outputRed = 0;
    job->outfile = NULL;
    int OutRedChecker = -1;
    for (int i = 0; i < ntok; i++) {
        if (strcmp(tokens[i], ">") == 0) {
            OutRedChecker = i;
            break;
        }
    }
    if (OutRedChecker >= 0) {
        if (OutRedChecker == 0) {
            fprintf(stderr, "Error: missing command\n");
            return -1;
        }
        if (OutRedChecker == ntok - 1) {
            fprintf(stderr, "Error: no output file\n");
            return -1;
        }
        job->outputRed = 1;
        job->outfile  = tokens[OutRedChecker + 1];
      
        for (int j = OutRedChecker + 2; j < ntok; j++) {
            if (strcmp(tokens[j], "|") == 0) {
                fprintf(stderr, "Error: mislocated output redirection\n");
                return -1;
            }
        }
      
        for (int j = OutRedChecker; j + 2 < ntok; j++) {
            tokens[j] = tokens[j + 2];
        }
        ntok -= 2;
    }

  
    job->InputRed = 0;
    job->infile = NULL;
    int InputRedChecker = -1;
    for (int i = 0; i < ntok; i++) {
        if (strcmp(tokens[i], "<") == 0) {
            InputRedChecker = i;
            break;
        }
    }
    if (InputRedChecker >= 0) {
        if (InputRedChecker == 0) {
            fprintf(stderr, "Error: missing command\n");
            return -1;
        }
        if (InputRedChecker == ntok - 1) {
            fprintf(stderr, "Error: no input file\n");
            return -1;
        }
        // checking for input redirection after a "|"
        for (int j = 0; j < InputRedChecker; j++) {
            if (strcmp(tokens[j], "|") == 0) {
                fprintf(stderr, "Error: mislocated input redirection\n");
                return -1;
            }
        }
        job->InputRed = 1;
        job->infile = tokens[InputRedChecker + 1];
    
        for (int j = InputRedChecker + 2; j < ntok; j++) {
            if (strcmp(tokens[j], "|") == 0) {
                fprintf(stderr, "Error: mislocated input redirection\n");
                return -1;
            }
        }
   
        for (int j = InputRedChecker; j + 2 < ntok; j++) {
            tokens[j] = tokens[j + 2];
        }
        ntok -= 2;
    }

    // detect pipes
    int pipev[MAX_PIPES];
    int pipecount = 0;
    for (int i = 0; i < ntok; i++) {
        if (strcmp(tokens[i], "|") == 0) {
            if (pipecount >= MAX_PIPES) {
                fprintf(stderr, "Error: too many pipes\n");
                return -1;
            } 
            pipev[pipecount++] = i;
        }  
    }

    for (int p = 0; p < pipecount; p++) {
        int pos = pipev[p];
        if (pos == 0 || pos == ntok - 1) {
            fprintf(stderr, "Error: missing command\n");
            return -1;
        }
        if (p + 1 < pipecount && pipev[p + 1] == pos + 1) {
            fprintf(stderr, "Error: missing command\n");
            return -1;
        }
    }

   
    job->commands = pipecount + 1;
    int start = 0;
    for (int c = 0; c < job->commands; c++) {
        int end = (c < pipecount ? pipev[c] : ntok);
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

void executionALL(const Job *job, const char *cmdBuffer) {
    int n = job->commands;
    if (n == 0) return;

  
    if (n == 1) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            return;
        }
        if (pid == 0) {
            if (job->outputRed) {
              
                Outputredirection(job->outfile);
            }
            if (job->InputRed){
                Inputredirection(job->infile);
            }
            execvp(job->cmds[0].argv[0], job->cmds[0].argv);
            fprintf(stderr, "Error: command not found\n");
            exit(1);
        }
        
        int status;
        waitpid(pid, &status, 0);
        if (WEXITSTATUS(status) == 0){
            fprintf(stderr, "+ completed '%s' [%d]\n", cmdBuffer, WEXITSTATUS(status));
        }

        /*if (job->background) {
            fprintf(stderr, "+ background pid: %d\n", pid);
        } else {
            int status;
            waitpid(pid, &status, 0);
            fprintf(stderr, "+ completed '%s' [%d]\n", cmdBuffer, WEXITSTATUS(status));
        }*/
        return;
    }


    int pipes[MAX_PIPES][2];
    for (int i = 0; i < n - 1; i++) {
        if (pipe(pipes[i]) < 0) {
            perror("pipe");
            exit(1);
        }
    }

    pid_t pids[MAX_COMMANDS];
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
            } else if (job->outputRed) {
                Outputredirection(job->outfile);
            }
            if (i == 0 && job-> InputRed) {
                Inputredirection(job->infile);
            }
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

    for (int i = 0; i < n - 1; i++) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }


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
        if (ParseAll(cmd, &job) != 0) {
            continue;
        }
        if (job.commands == 0) {
            continue;
        }
        if (job.commands == 1 &&
            strcmp(job.cmds[0].argv[0], "cd") == 0) {
            int st = cd_build(job.cmds[0].argv);
            fprintf(stderr, "+ completed '%s' [%d]\n",
                    cmdBuffer, st);
            continue;
        }

        executionALL(&job, cmdBuffer);
    }

    return EXIT_SUCCESS;
}