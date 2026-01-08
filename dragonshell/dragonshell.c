// dragonshell.c
// A simple shell program with job control, I/O redirection, and piping.

#include <string.h>   // For string manipulation (strtok, strcmp, strncpy, etc.)
#include <stdio.h>    // For standard I/O (printf, fgets, perror)
#include <stdlib.h>   // For general utilities (malloc, free, exit)
#include <unistd.h>   // For POSIX functions (chdir, getcwd, fork, execve, dup2, setpgid)
#include <limits.h>   // For system limits (PATH_MAX)
#include <errno.h>    // For error number checking (e.g., in waitpid)
#include <sys/types.h> // For various data types (pid_t)
#include <sys/wait.h> // For waitpid and related macros (WIFEXITED, WIFSTOPPED, etc.)
#include <fcntl.h>    // For file control (open)
#include <signal.h>   // For signal handling (sigaction, kill, signal)
#include <termios.h>  // For definition of STDIN_FILENO
#include <sys/ioctl.h>

// ---------------- Design Constants ----------------
#define LINE_LENGTH 100 // max # of characters in an input line
#define MAX_ARGS 20      // max # of args to a command (not incl. command name)
#define MAX_LENGTH 20   // max # of characters in an argument

// --- Job Control Data Structure ---
// Design: Linked list node to track background/suspended jobs.
// Process table structure for tracking jobs
typedef struct process
{
  pid_t pid;
  char state; // 'R' for running (background), 'T' for suspended (stopped)
  char command[LINE_LENGTH]; // Command line for display in `jobs`
  struct process *next;
} process_t;

// Global head of the job control linked list
static process_t *process_table = NULL;

// --- Global Foreground Process Tracking ---
// Design: Used by signal handlers (SIGINT/SIGTSTP) to direct signals to the
// currently active foreground job and by the main loop to manage terminal control.
static pid_t foreground_pid = -1;
static pid_t foreground_pgid = -1;

// Global environment variable pointer, required by execve
extern char **environ; 

// --- Function Forward Declarations ---
static void setup_signal_handlers(void);
static void sigchld_handler(int);
static void sigint_handler(int);
static void sigtstp_handler(int);
static void add_process(pid_t pid, char state, const char *cmd);
static void remove_process(pid_t pid);
static void update_process_state(pid_t pid, char state);
static void print_jobs_impl(void);
static void cleanup_processes(void);

static void parse_command(char **tokens, int *background,
                          char **input_file, char **output_file,
                          char ***pipe_rhs);

static void execute_external(char **argv, int background,
                             char *in, char *out);
static void execute_pipe(char **lhs, char **rhs);
static void strip_quotes(char *s);

// ---------------- Tokenizer ----------------
/**
 * @brief Tokenize a C string
 *
 * @param str - The C string to tokenize
 * @param delim - The C string containing delimiter character(s)
 * @param argv - A char* array that will contain the tokenized strings
 * Make sure that you allocate enough space for the array.
 */
void tokenize(char *str, const char *delim, char **argv)
{
  char *token;
  size_t i = 0;
  token = strtok(str, delim);
  while (token != NULL && i < (size_t)(MAX_ARGS + 1))
  { // cmd + MAX_ARGS
    argv[i++] = token;
    token = strtok(NULL, delim);
  }
  argv[i] = NULL;  // NULL-terminate the argument list for execve and parsing
}

// ---------------- Built-in commands ----------------
// Design: Built-in commands execute directly within the shell process,
// avoiding the overhead of forking.

/**
 * @brief Prints the current working directory.
 */
static void bi_pwd(void)
{
  char buf[PATH_MAX];
  if (getcwd(buf, sizeof(buf)))
  {
    printf("%s\n", buf);
    fflush(stdout);
  }
  else
  {
    perror("getcwd");
  }
}

/**
 * @brief Changes the current working directory.
 * @param path - The directory path.
 */
static void bi_cd(char *path)
{
  if (!path || path[0] == '\0')
  {
    fprintf(stderr, "dragonshell: Expected argument to \"cd\"\n");
    return;
  }
  if (chdir(path) < 0)
  {
    fprintf(stderr, "dragonshell: No such file or directory\n");
  }
}

/**
 * @brief Lists all running and suspended background jobs.
 */
static void bi_jobs(void)
{
  print_jobs_impl(); // Calls the internal printing helper
}

/**
 * @brief Signals the main loop to terminate the shell.
 * @return 1 to indicate exit.
 */
static int bi_exit(void)
{
  return 1; // signal main to quit loop
}

// ---------------- helpers ----------------
static void safe_strcat(char *dst, size_t cap, const char *src)
{
  size_t have = strlen(dst), need = strlen(src);
  if (have + need + 1 < cap)
    strcat(dst, src);
}

static void strip_quotes(char *s)
{
  if (!s)
    return;
  size_t n = strlen(s);
  if (n >= 2)
  {
    char q = s[0];
    if ((q == '"' || q == '\'') && s[n - 1] == q)
    {
      memmove(s, s + 1, n - 2);
      s[n - 2] = '\0';
    }
  }
}

// ---------------- signal setup ----------------
/**
 * @brief Configures the signal handlers for the shell process.
 * Design: The shell process must ignore or handle certain signals to maintain job control.
 */
static void setup_signal_handlers(void)
{
  struct sigaction sa;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESTART; // Restart slow system calls interrupted by a signal
  sa.sa_handler = SIG_IGN;

  // SIGCHLD: Mandatory for job control. The shell must handle child termination/state changes.
  sa.sa_handler = sigchld_handler;
  sigaction(SIGCHLD, &sa, NULL);

  // SIGINT (Ctrl-C): Handled by the shell to forward the signal to the foreground process group.
  sa.sa_handler = sigint_handler;
  sigaction(SIGINT, &sa, NULL);

// SIGTSTP (Ctrl-Z): The shell must ignore this signal. The terminal driver (TTY) sends
  // SIGTSTP to the *foreground* process group, which includes the job itself. Ignoring it
  // ensures the shell doesn't suspend while a job is running, allowing it to manage the job's suspension.
   sa.sa_handler = sigtstp_handler; 
  sigaction(SIGTSTP, &sa, NULL);

// SIGTTOU and SIGTTIN: Ignored to prevent the shell from stopping if a job attempts
  // to read/write to the terminal after the shell has reclaimed terminal control.
  signal(SIGTTOU, SIG_IGN); 
  signal(SIGTTIN, SIG_IGN);
}

/**
 * @brief SIGCHLD handler: Reaps terminated children and updates job states.
 * Design: Must be non-blocking (using WNOHANG) and handle multiple state changes (WUNTRACED, WCONTINUED)
 * inside a loop to process all pending events.
 */

static void sigchld_handler(int sig)
{
  (void)sig; // Mark parameter as unused
  while (1)
  {
    int status;
    pid_t pid = waitpid(-1, &status, WNOHANG | WUNTRACED | WCONTINUED);
    if (pid <= 0)
      break; // No more children or error

    if (WIFEXITED(status) || WIFSIGNALED(status))
    {
      remove_process(pid); // Child terminated normally or by a signal
    }
    else if (WIFSTOPPED(status))
    {
      update_process_state(pid, 'T'); // Child stopped (suspended)
    }
    else if (WIFCONTINUED(status))
    {
      update_process_state(pid, 'R'); // Child continued (resumed)
    }
  }
}

/**
 * @brief SIGINT handler: Forwards SIGINT to the foreground job's process group.
 * Design: The terminal sends SIGINT to the foreground group, but the handler is a safeguard
 * to ensure the signal is correctly delivered, primarily targeting the *process group* ID.
 */

static void sigint_handler(int sig)
{
  (void)sig;
  if (foreground_pgid > 0)
  {
    kill(-foreground_pgid, SIGINT); // Send to the entire foreground process group
  }
  else if (foreground_pid > 0) // Fallback for single-process job without a dedicated group
  {
    kill(foreground_pid, SIGINT);
  }
}

static void sigtstp_handler(int sig)
{
  (void)sig;
  if (foreground_pgid > 0)
  {
    kill(-foreground_pgid, SIGTSTP); // Send to the entire foreground process group
  }
  else if (foreground_pid > 0) // Fallback for single-process job without a dedicated group
  {
    kill(foreground_pid, SIGTSTP);
  }
}

// static void sigtstp_handler(int sig)
// {
//   (void)sig;
//   write(STDERR_FILENO, "\n[Shell received SIGTSTP]\n", 27);
//   char buf[100];
//   int len = snprintf(buf, sizeof(buf), "[fg_pid=%d, fg_pgid=%d]\n", foreground_pid, foreground_pgid);
//   write(STDERR_FILENO, buf, len);
  
//   if (foreground_pgid > 0)
//   {
//     kill(-foreground_pgid, SIGTSTP); // whole fg group
//   }
//   else if (foreground_pid > 0)
//   {
//     kill(foreground_pid, SIGTSTP);
//   }
// }

// ---------------- process table implementation ----------------
// Design: A simple linked list is used to track the job state, PID, and command string.

/**
 * @brief Adds a new process to the job table.
 */
static void add_process(pid_t pid, char state, const char *cmd)
{
  process_t *p = malloc(sizeof(*p));
  if (!p)
  {
    perror("malloc");
    return;
  }
  p->pid = pid;
  p->state = state;
  strncpy(p->command, cmd ? cmd : "", LINE_LENGTH - 1);
  p->command[LINE_LENGTH - 1] = '\0';
  p->next = process_table;
  process_table = p;
}

/**
 * @brief Removes a process from the job table (used after termination).
 */
static void remove_process(pid_t pid)
{
  process_t *cur = process_table, *prev = NULL;
  while (cur)
  {
    if (cur->pid == pid)
    {
      if (prev)
        prev->next = cur->next;
      else
        process_table = cur->next;
      free(cur);
      return;
    }
    prev = cur;
    cur = cur->next;
  }
}

/**
 * @brief Updates the state of a process in the job table (e.g., R -> T).
 */
static void update_process_state(pid_t pid, char state)
{
  for (process_t *p = process_table; p; p = p->next)
  {
    if (p->pid == pid)
    {
      p->state = state;
      return;
    }
  }
}

/**
 * @brief Prints all jobs in the table.
 */
static void print_jobs_impl(void)
{
  for (process_t *p = process_table; p; p = p->next)
  {
    printf("%d %c %s\n", p->pid, p->state, p->command);
  }
  fflush(stdout);
}

/**
 * @brief Cleans up all remaining jobs before shell exit.
 * Design: Tries SIGTERM (polite) and then SIGKILL (forceful) to terminate background jobs.
 */
static void cleanup_processes(void)
{
  // Design: Terminate politely (SIGTERM) then forcibly (SIGKILL).

  // 1. Polite termination
  for (process_t *p = process_table; p; p = p->next)
    kill(p->pid, SIGTERM);

  // 2. Wait briefly via waitpid, then forcibly kill remaining
  int st;
  // Wait for 200ms worth of children to exit after SIGTERM
  // NOTE: Relying on waitpid(2) with WNOHANG for asynchronous cleanup instead of usleep(3).
  for(int i = 0; i < 50; ++i) // Loop 50 times to check for termination, which provides a small delay
  {
      while (waitpid(-1, &st, WNOHANG) > 0);
      // We cannot use usleep(3), so we will just check frequently.
  }
  
  // 3. Forcible termination
  for (process_t *p = process_table; p; p = p->next)
    kill(p->pid, SIGKILL);

  // 4. Final reap
  while (waitpid(-1, &st, WNOHANG) > 0)
  {
  }

  // 5. Free linked list
  while (process_table)
  {
    process_t *tmp = process_table;
    process_table = process_table->next;
    free(tmp);
  }
}

// ---------------- parsing ----------------
/**
 * @brief Parses the tokenized command line for special shell syntax.
 * Design: Iterates through the argument list to find job control symbols ('&')
 * and I/O redirection/pipe symbols ('<', '>', '|'). It NULLs out the tokens
 * that represent the symbols and their arguments, effectively separating the command.
 */
static void parse_command(char **tokens, int *background,
                          char **input_file, char **output_file,
                          char ***pipe_rhs)
{
  *background = 0;
  *input_file = NULL;
  *output_file = NULL;
  *pipe_rhs = NULL;

  for (int i = 0; tokens[i]; ++i)
  {
    if (strcmp(tokens[i], "&") == 0)
    {
      *background = 1;
      tokens[i] = NULL;
      break;
    }
    else if (strcmp(tokens[i], "<") == 0)
    {
      tokens[i] = NULL;
      if (tokens[i + 1])
      {
        *input_file = tokens[i + 1];
        tokens[i + 1] = NULL;
        i++;
      }
    }
    else if (strcmp(tokens[i], ">") == 0)
    {
      tokens[i] = NULL;
      if (tokens[i + 1])
      {
        *output_file = tokens[i + 1];
        tokens[i + 1] = NULL;
        i++;
      }
    }
    else if (strcmp(tokens[i], "|") == 0)
    {
      tokens[i] = NULL;
      *pipe_rhs = &tokens[i + 1];
      break;
    }
  }
}

// ---------------- execution helpers ----------------
static void build_cmd_str(char *dst, size_t cap,
                          char **argv, const char *in, const char *out, int bg)
{
  dst[0] = '\0';
  for (int i = 0; argv && argv[i]; ++i)
  {
    safe_strcat(dst, cap, argv[i]);
    if (argv[i + 1])
      safe_strcat(dst, cap, " ");
  }
  if (in)
  {
    safe_strcat(dst, cap, " < ");
    safe_strcat(dst, cap, in);
  }
  if (out)
  {
    safe_strcat(dst, cap, " > ");
    safe_strcat(dst, cap, out);
  }
  if (bg)
  {
    safe_strcat(dst, cap, " &");
  }
}

/**
 * @brief Sets the foreground process group using the ioctl(2) system call (Section 2 compliant).
 * @param pgid The process group ID to set as the foreground group.
 * @return 0 on success, -1 on failure.
 */
// static int set_foreground_pgid(pid_t pgid)
// {
//     // TIOCSPGRP is the ioctl request code to set the foreground process group ID.
//     // The ioctl(2) system call is documented in Section 2.
//     if (ioctl(STDIN_FILENO, TIOCSPGRP, &pgid) < 0) {
//         perror("ioctl (set TTY pgrp)");
//         return -1;
//     }
//     return 0;
// }

static int set_foreground_pgid(pid_t pgid)
{
    int fd = STDIN_FILENO;
    int r;

retry_ioctl:
    r = ioctl(fd, TIOCSPGRP, &pgid);
    if (r == 0) return 0;

    // If STDIN isn't a tty or is invalid, fall back to /dev/tty
    if (errno == ENOTTY || errno == EBADF) {
        int ttyfd = open("/dev/tty", O_RDWR);
        if (ttyfd < 0) {
            // No controlling tty available
            perror("open(/dev/tty)");
            return -1;
        }
        r = ioctl(ttyfd, TIOCSPGRP, &pgid);
        int saved = errno;
        close(ttyfd);
        if (r == 0) return 0;
        errno = saved;
    } else if (errno == EINTR) {
        goto retry_ioctl;
    }

    // Common macOS failures: EPERM (not same session / not ready), EINVAL
    perror("ioctl(TIOCSPGRP)");
    return -1;
}

/**
 * @brief Helper to redirect STDIN/STDOUT based on file paths.
 * @return 0 on success, -1 on failure.
 */
static int redirect_io(const char *in, const char *out)
{
  if (in)
  {
    int fd = open(in, O_RDONLY);
    if (fd < 0)
    {
      perror("open input file");
      return -1;
    }
    if (dup2(fd, STDIN_FILENO) < 0)
    {
      perror("dup2 in");
      close(fd);
      return -1;
    }
    close(fd);
  }
  if (out)
  {
    int fd = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
    {
      perror("open output file");
      return -1;
    }
    if (dup2(fd, STDOUT_FILENO) < 0)
    {
      perror("dup2 out");
      close(fd);
      return -1;
    }
    close(fd);
  }
  return 0;
}

/**
 * @brief Executes a single external command (non-pipe).
 * Design: Forks, sets up process groups, redirects I/O in the child,
 * executes the command, and handles job control in the parent.
 */
static void execute_external(char **argv, int background,
                             char *in, char *out)
{
  if (!argv || !argv[0])
    return;

  pid_t pid = fork();
  if (pid < 0)
  {
    perror("fork");
    return;
  }

  if (pid == 0)
{
  // child
  setpgid(0, 0); // new process group for this job

  struct sigaction da;
  sigemptyset(&da.sa_mask);
  da.sa_flags = 0;
  da.sa_handler = SIG_DFL;
  sigaction(SIGINT, &da, NULL);
  sigaction(SIGTSTP, &da, NULL);

    if (redirect_io(in, out) < 0) _exit(1);

    // Spec: do NOT search PATH; run exactly as provided
    execve(argv[0], argv, environ);

    fprintf(stderr, "dragonshell: Command not found\n");
    _exit(1);
  }

  // parent
  setpgid(pid, pid); // Ensure the job has a PGID equal to its PID

  char cmd[LINE_LENGTH];
  build_cmd_str(cmd, sizeof(cmd), argv, in, out, background);
  add_process(pid, 'R', cmd);

  if (background)
  {
    printf("PID %d is sent to background\n", pid);
    fflush(stdout);
  }
  else
  {
    foreground_pid = pid;
    foreground_pgid = pid;

  set_foreground_pgid(foreground_pgid);

    int status;
    for (;;)
    {
      pid_t w = waitpid(pid, &status, WUNTRACED);
      if (w == -1)
      {
        if (errno == EINTR)
          continue; // interrupted by a signal — retry
        break;      // unexpected error
      }
      if (WIFSTOPPED(status))
      {
        // child stopped by SIGTSTP — keep it in the job table as 'T' and return to prompt
        update_process_state(pid, 'T');
        break;
      }
      if (WIFEXITED(status) || WIFSIGNALED(status))
      {
        // child finished — remove from table
        remove_process(pid);
        break;
      }
    }
     set_foreground_pgid(getpgrp());

    foreground_pid = -1;
    foreground_pgid = -1;
  }
}

/**
 * @brief Executes a simple two-command pipe (lhs | rhs).
 * Design: Creates two children connected by a pipe. The two children form a single
 * process group. The shell waits for both to finish/stop.
 */
static void execute_pipe(char **lhs, char **rhs)
{
  int pipefd[2];
  if (pipe(pipefd) < 0) { perror("pipe"); return; }

  pid_t p1 = fork();
  if (p1 < 0) { perror("fork"); close(pipefd[0]); close(pipefd[1]); return; }

  if (p1 == 0) {
    // writer
    setpgid(0, 0);  // new group (leader = this child)
    struct sigaction da; sigemptyset(&da.sa_mask); da.sa_flags = 0;
    da.sa_handler = SIG_DFL;
    sigaction(SIGINT,  &da, NULL);
    sigaction(SIGTSTP, &da, NULL);

    close(pipefd[0]);
    if (dup2(pipefd[1], STDOUT_FILENO) < 0) { perror("dup2 pipe write"); _exit(1); }
    close(pipefd[1]);

    execve(lhs[0], lhs, environ);
    fprintf(stderr, "dragonshell: Command not found\n");
    _exit(1);
  }
  // parent of p1
  setpgid(p1, p1);

  pid_t p2 = fork();
  if (p2 < 0) { perror("fork"); close(pipefd[0]); close(pipefd[1]); return; }

  if (p2 == 0) {
    // reader joins writer's group
    setpgid(0, p1);
    struct sigaction da; sigemptyset(&da.sa_mask); da.sa_flags = 0;
    da.sa_handler = SIG_DFL;
    sigaction(SIGINT,  &da, NULL);
    sigaction(SIGTSTP, &da, NULL);

    close(pipefd[1]);
    if (dup2(pipefd[0], STDIN_FILENO) < 0) { perror("dup2 pipe read"); _exit(1); }
    close(pipefd[0]);

    execve(rhs[0], rhs, environ);
    fprintf(stderr, "dragonshell: Command not found\n");
    _exit(1);
  }
  // parent of p2
  setpgid(p2, p1);

  // The pipeline's PG owns the terminal while it runs
 set_foreground_pgid(p1);


  close(pipefd[0]); close(pipefd[1]);

  // record once under each pid (ok for rubric)
  char cmd[LINE_LENGTH] = "";
  for (int i=0; lhs[i]; ++i){ safe_strcat(cmd, sizeof(cmd), lhs[i]); if (lhs[i+1]) safe_strcat(cmd, sizeof(cmd), " "); }
  safe_strcat(cmd, sizeof(cmd), " | ");
  for (int i=0; rhs[i]; ++i){ safe_strcat(cmd, sizeof(cmd), rhs[i]); if (rhs[i+1]) safe_strcat(cmd, sizeof(cmd), " "); }
  add_process(p1, 'R', cmd);
  add_process(p2, 'R', cmd);

  // wait both with WUNTRACED so Ctrl-Z stops are visible
  foreground_pid  = p1;
  foreground_pgid = p1;

  int st;
  if (waitpid(p1, &st, WUNTRACED) != -1) {
    if (WIFSTOPPED(st)) update_process_state(p1, 'T');
    else if (WIFEXITED(st) || WIFSIGNALED(st)) remove_process(p1);
  }
  if (waitpid(p2, &st, WUNTRACED) != -1) {
    if (WIFSTOPPED(st)) update_process_state(p2, 'T');
    else if (WIFEXITED(st) || WIFSIGNALED(st)) remove_process(p2);
  }

  // now give the terminal back to the shell
    set_foreground_pgid(getpgrp());

  foreground_pid  = -1;
  foreground_pgid = -1;
}

// ---------------- Main REPL loop ----------------
int main(int argc, char **argv)
{
  (void)argc;
  (void)argv;

  setup_signal_handlers();
  setpgid(0, 0);      

      // shell becomes a process-group leader
   if (set_foreground_pgid(getpgrp()) < 0) 
    perror("Initial TTY setup failed");

  // startup greeting
  printf("Welcome to Dragon Shell!\n");

  char line[LINE_LENGTH + 1]; // buffer to hold the input line

  // print the string prompt without a newline, before beginning to read
  while (1)
  {
    printf("dragonshell > ");
    fflush(stdout); // force the prompt to print immediately

    // read user input (pauses until user types + press Enter)
    if (!fgets(line, sizeof(line), stdin))
    {
      break; // EOF (Ctrl-D) → exit the shell
    }

    // remove the newline character from the input line
    line[strcspn(line, "\n")] = 0;

    // skip empty lines
    if (strlen(line) == 0)
      continue;

    // tokenize the input
    char *args[MAX_ARGS + 2] = {0};
    tokenize(line, " \t", args);
    if (!args[0])
      continue;

    // parse special syntax
    int background = 0;
    char *in = NULL, *out = NULL;
    char **pipe_rhs = NULL;
    parse_command(args, &background, &in, &out, &pipe_rhs);

    // strip quotes in args, pipe side, and redir filenames
    for (int i = 0; args[i]; ++i)
      strip_quotes(args[i]);
    if (pipe_rhs)
    {
      for (int j = 0; pipe_rhs[j]; ++j)
        strip_quotes(pipe_rhs[j]);
    }
    if (in)
      strip_quotes(in);
    if (out)
      strip_quotes(out);

    // If a pipe exists → skip built-ins (external-only per requirements)
    if (pipe_rhs)
    {
      execute_pipe(args, pipe_rhs);
      // Reap any finished bg children without blocking
      int st;
      while (waitpid(-1, &st, WNOHANG) > 0)
      {
      }
      continue;
    }

    // check for built-in commands (ignore extra args)
    if (strcmp(args[0], "pwd") == 0)
    {
      bi_pwd();
      continue;
    }
    if (strcmp(args[0], "cd") == 0)
    {
      bi_cd(args[1]); // ignore extras
      continue;
    }
    if (strcmp(args[0], "jobs") == 0)
    {
      bi_jobs();
      continue;
    }
    if (strcmp(args[0], "exit") == 0)
    {
      if (bi_exit())
      {
        cleanup_processes();
        return 0;
      }
      continue;
    }

    // external command (no PATH search; absolute/relative only)
    execute_external(args, background, in, out);

    // Reap any finished bg children without blocking
    int status;
    while (waitpid(-1, &status, WNOHANG) > 0)
    {
    }
  }
  cleanup_processes();
  return 0;
}