#include "swish_funcs.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "string_vector.h"

#define MAX_ARGS 10

/*
 * Helper function to run a single command within a pipeline. You should make
 * use of the provided 'run_command' function here.
 * tokens: String vector containing the tokens representing the command to be
 * executed, possible redirection, and the command's arguments.
 * pipes: An array of pipe file descriptors.
 * n_pipe_fds: Length of the 'pipes' array
 * in_idx: Index of the file descriptor in the array from which the program
 *         should read its input, or -1 if input should not be read from a pipe.
 * out_idx: Index of the file descriptor in the array to which the program
 *          should write its output, or -1 if output should not be written to
 *          a pipe.
 * Returns 0 on success or -1 on error.
 */
int run_piped_command(strvec_t *tokens, int *pipes, int n_pipe_fds, int in_idx, int out_idx)
{
    // TODO Complete this function's implementation
    if (in_idx != -1)
    {
        if (dup2(pipes[in_idx], STDIN_FILENO) == -1)
        {
            perror("dup2");
            return -1;
        }
    }
    if (out_idx != -1)
    {
        if (dup2(pipes[out_idx], STDOUT_FILENO) == -1)
        {
            perror("dup2");
            return -1;
        }
    }

    for (int i = 0; i < n_pipe_fds; i++)
    {
        close(pipes[i]);
    }
    return run_command(tokens);
}

int run_pipelined_commands(strvec_t *tokens)
{
    int n_pipes = strvec_num_occurrences(tokens, "|");
    int n_commands = n_pipes + 1;
    int n_pipe_fds = n_pipes * 2;

    int *pipes = malloc(n_pipe_fds * sizeof(int));
    pid_t *pids = malloc(n_commands * sizeof(pid_t));

    if ((pipes == NULL && n_pipe_fds > 0) || pids == NULL)
    {
        perror("malloc");
        return -1;
    }

    for (int i = 0; i < n_pipes; i++)
    {
        if (pipe(&pipes[i * 2]) == -1)
        {
            perror("pipe");
            for (int j = 0; j < 2 * i; j++)
            {
                close(pipes[j]);
            }

            free(pipes);
            free(pids);
            return -1;
        }
    }

    int start = 0;
    int launched = 0;
    int error = 0;

    for (int i = 0; i < n_commands; i++)
    {
        int end = start;
        while (end < (int)tokens->length)
        {
            char *token = strvec_get(tokens, end);

            if (token == NULL)
            {
                error = 1;
                break;
            }

            if (token[0] == '|' && token[1] == '\0')
            {
                break;
            }

            end++;
        }

        if (error)
        {
            break;
        }

        if (start == end)
        {
            fprintf(stderr, "Empty pipeline command\n");
            error = 1;
            break;
        }

        strvec_t command;

        if (strvec_slice(tokens, &command, start, end) == -1)
        {
            fprintf(stderr, "strvec_slice failed\n");
            error = 1;
            break;
        }

        int in_idx = -1;

        if (i > 0)
        {
            in_idx = 2 * (i - 1);
        }

        int out_idx = -1;

        if (i < n_commands - 1)
        {
            out_idx = 2 * i + 1;
        }

        pid_t pid = fork();

        if (pid == -1)
        {
            perror("fork");
            strvec_clear(&command);
            error = 1;
            break;
        }

        if (pid == 0)
        {
            // Child process
            run_piped_command(
                &command,
                pipes,
                n_pipe_fds,
                in_idx,
                out_idx);

            // Only runs if the command fails
            strvec_clear(&command);
            _exit(1);
        }

        pids[launched] = pid;
        launched++;

        strvec_clear(&command);
        start = end + 1;
    }

    for (int i = 0; i < n_pipe_fds; i++)
    {
        if (close(pipes[i]) == -1)
        {
            perror("close");
            error = 1;
        }
    }

    for (int i = 0; i < launched; i++)
    {
        pid_t result = waitpid(pids[i], NULL, 0);

        if (result == -1)
        {
            perror("waitpid");
            error = 1;
        }
    }

    free(pipes);
    free(pids);

    if (error)
    {
        return -1;
    }

    return 0;
}
