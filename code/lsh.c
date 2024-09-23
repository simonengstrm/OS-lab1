/*
 * Main source code file for lsh shell program
 *
 * You are free to add functions to this file.
 * If you want to add functions in a separate file(s)
 * you will need to modify the CMakeLists.txt to compile
 * your additional file(s).
 *
 * Add appropriate comments in your code to make it
 * easier for us while grading your assignment.
 *
 * Using assert statements in your code is a great way to catch errors early and make debugging easier.
 * Think of them as mini self-checks that ensure your program behaves as expected.
 * By setting up these guardrails, you're creating a more robust and maintainable solution.
 * So go ahead, sprinkle some asserts in your code; they're your friends in disguise!
 *
 * All the best!
 */
#include <assert.h>
#include <ctype.h>
#include <readline/readline.h>
#include <readline/history.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <limits.h>
#include <fcntl.h>

// The <unistd.h> header is your gateway to the OS's process management facilities.
#include <unistd.h>

#include "parse.h"

static void print_cmd(Command *cmd);
static void print_pgm(Pgm *p);
void stripwhite(char *);

// Current foreground pid
static pid_t fg_pid = 0;

#define READ_END 0
#define WRITE_END 1

void handle_child_finished(int sig)
{
  // Get status of child process using waitpid
  int status;
  // -1 for any child process
  pid_t pid = waitpid(-1, &status, WNOHANG);
  if (status == 0)
  {
    printf("\nChild process %d exited normally\n", pid);
  }
  else
  {
    printf("\nChild process %d exited with status %d\n", pid, status);
  }
  return;
}

void change_directory(Command *cmd)
{
  if (cmd->pgm->pgmlist[1] == NULL)
  {
    printf("No path provided\n");
    const char *home = getenv("HOME");
    if (home == NULL)
    {
      printf("Home variable not set\n");
      return;
    }
    if (chdir(home) != 0)
    {
      printf("Chdir failed\n");
      return;
    }
  }
  else
  {
    if (chdir(cmd->pgm->pgmlist[1]) != 0)
    {
      printf("Chdir failed\n");
      return;
    }
  }

  return;
}

void handle_sigtstp(int sig)
{
  printf("Caught SIGTSTP\n");
  exit(0);
  return;
}

void handle_sigint(int sig)
{
  if (fg_pid != 0)
  {
    printf("Killing foreground process %d\n", fg_pid);
    kill(fg_pid, SIGINT);
    fg_pid = 0;
  }
  else
  {
    printf("\n");
  }
  return;
}

void pipeline(Command *cmd, int fd_old_write)
{
  if (fd_old_write >= 0)
  {
    // Reroute STDOUT to old pipe write
    dup2(fd_old_write, STDOUT_FILENO);
    close(fd_old_write);
  }

  if (cmd->pgm->next == NULL)
  {
    // Check for file redirect
    if (cmd->rstdin != NULL)
    {
      // Redirect stdout to open(file)
      int in = open(cmd->rstdin, O_RDONLY);
      if (in == -1)
      {
        fprintf(stderr, "Cloud not redirect input from file %s\n", cmd->rstdin);
        _exit(1);
      }

      dup2(in, STDIN_FILENO);
      close(in);
    }

    // If only one command (fd_old_write = -1 in base case) redirect stdout here too
    if (cmd->rstdout != NULL && fd_old_write == -1)
    {
      int out = open(cmd->rstdout, O_TRUNC | O_WRONLY | O_CREAT);
      if (out == -1)
      {
        fprintf(stderr, "Could not redirect output to file %s\n", cmd->rstdout);
        _exit(-1);
      }
      dup2(out, STDOUT_FILENO);
      close(out);
    }

    execvp(cmd->pgm->pgmlist[0], cmd->pgm->pgmlist);
    fprintf(stderr, "Could not execute program %s\n", cmd->pgm->pgmlist[0]);
    _exit(-1);
  }

  int fd[2];
  pipe(fd);

  pid_t pid = fork();

  if (pid == 0) // Process to execute next command
  {
    if (cmd->background == 1) // Change process group if background
    {
      setpgid(0, 0);
    }

    if (cmd->pgm->next != NULL)
    {
      cmd->pgm = cmd->pgm->next;
      pipeline(cmd, fd[WRITE_END]);
    }
  }
  else if (pid == -1)
  {
    fprintf(stderr, "Fork error");
    kill(0, SIGINT);
  }
  else
  {
    // Reroute stdin to read end of pipe
    dup2(fd[READ_END], STDIN_FILENO);
    close(fd[READ_END]);
    close(fd[WRITE_END]);

    // If rightmost command, check rstdout and redirect if needed
    // First command always gets -1 as fd_old_write
    if (fd_old_write == -1 && cmd->rstdout != NULL)
    {
      int out = open(cmd->rstdout, O_TRUNC | O_WRONLY | O_CREAT);
      if (out == -1)
      {
        fprintf(stderr, "Could not redirect output to file %s\n", cmd->rstdout);
        _exit(-1);
      }
      dup2(out, STDOUT_FILENO);
      close(out);
    }

    waitpid(pid, NULL, 0);
    execvp(cmd->pgm->pgmlist[0], cmd->pgm->pgmlist);
  }
}

void handle_cmd2(Command *cmd)
{
  pid_t pid = fork();

  if (pid == 0)
  {
    if (cmd->background == 1)
    {
      setpgid(0, 0);
    }

    pipeline(cmd, -1);
  }
  else if (pid == -1)
  {
    fprintf(stderr, "Fork error");
    kill(0, SIGINT);
  }
  else
  {
    if (cmd->background == 1)
    {
      printf("pid: %d\n", pid);
      waitpid(pid, NULL, WNOHANG);
    }
    else
    {
      fg_pid = pid;
      waitpid(pid, NULL, 0);
    }
  }
}

int main(void)
{
  // Setup signal handling
  signal(SIGTSTP, handle_sigtstp);
  signal(SIGINT, handle_sigint);
  signal(SIGCHLD, handle_child_finished);

  for (;;)
  {
    char *line;
    char cwd[PATH_MAX];
    getcwd(cwd, sizeof(cwd));
    printf("%s", cwd);
    line = readline("> ");

    // Check EOF
    if (line == NULL)
    {
      kill(0, SIGINT);
      return 0;
    }

    // Remove leading and trailing whitespace from the line
    stripwhite(line);

    // If stripped line not blank
    if (*line)
    {
      add_history(line);

      Command cmd;
      if (parse(line, &cmd) == 1)
      {
        // Just prints cmd
        print_cmd(&cmd);

        // Check for built-in commands
        if (strcmp(cmd.pgm->pgmlist[0], "cd") == 0)
        {
          change_directory(&cmd);
          continue;
        }
        else if (strcmp(cmd.pgm->pgmlist[0], "exit") == 0)
        {
          kill(0, SIGINT);
          exit(0);
          return 0;
        }

        // Handle commands
        handle_cmd2(&cmd);
      }
      else
      {
        printf("Parse ERROR\n");
      }
    }

    // Clear memory
    free(line);
  }

  return 0;
}

/*
 * Print a Command structure as returned by parse on stdout.
 *
 * Helper function, no need to change. Might be useful to study as inspiration.
 */
static void print_cmd(Command *cmd_list)
{
  printf("------------------------------\n");
  printf("Parse OK\n");
  printf("stdin:      %s\n", cmd_list->rstdin ? cmd_list->rstdin : "<none>");
  printf("stdout:     %s\n", cmd_list->rstdout ? cmd_list->rstdout : "<none>");
  printf("background: %s\n", cmd_list->background ? "true" : "false");
  printf("Pgms:\n");
  print_pgm(cmd_list->pgm);
  printf("------------------------------\n");
}

/* Print a (linked) list of Pgm:s.
 *
 * Helper function, no need to change. Might be useful to study as inpsiration.
 */
static void print_pgm(Pgm *p)
{
  if (p == NULL)
  {
    return;
  }
  else
  {
    char **pl = p->pgmlist;

    /* The list is in reversed order so print
     * it reversed to get right
     */
    print_pgm(p->next);
    printf("            * [ ");
    while (*pl)
    {
      printf("%s ", *pl++);
    }
    printf("]\n");
  }
}

/* Strip whitespace from the start and end of a string.
 *
 * Helper function, no need to change.
 */
void stripwhite(char *string)
{
  size_t i = 0;

  while (isspace(string[i]))
  {
    i++;
  }

  if (i)
  {
    memmove(string, string + i, strlen(string + i) + 1);
  }

  i = strlen(string) - 1;
  while (i > 0 && isspace(string[i]))
  {
    i--;
  }

  string[++i] = '\0';
}
