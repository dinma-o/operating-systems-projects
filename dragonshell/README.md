# CMPUT 379 Assignment 1 — dragonshell

**Name:** Chidinma Obi-Okoye
**SID:** 1756548
**CCID:** obiokoye
---

## 🔧 Features Implemented

### 1. Built-in Commands
- **pwd**  
  - Uses `getcwd()` to print the current working directory.  
  - On error, calls `perror("getcwd")`.  
- **cd [path]**  
  - Uses `chdir()` to change the working directory.  
  - Error messages:  
    - If no argument: `dragonshell: Expected argument to "cd"`  
    - If path doesn’t exist: `dragonshell: No such file or directory`  
  - Ignores extra arguments as per spec.  
- **jobs**  
  - Maintains an internal linked list of background and suspended jobs.  
  - Each entry stores: `pid`, `state` (`R` = running, `T` = suspended), and the command string.  
  - Output format: `PID STATE COMMAND`  
- **exit**  
  - Iterates through process table.  
  - Sends `SIGTERM` to background and suspended jobs, waits briefly, then escalates with `SIGKILL` if needed.  
  - Reaps child processes with `waitpid()`.  

**Syscalls used:** `getcwd`, `chdir`, `kill`, `waitpid`.

---

### 2. Launching External Programs
- Handles both **absolute/relative paths** and executables found in the **current working directory**.  
- Uses `fork()` to create a child, `execve()` to run the program, and `setpgid()` to assign a process group.  
- If the program is not found: prints `dragonshell: Command not found`.  

**Syscalls used:** `fork`, `execve`, `setpgid`, `access`.

---

### 3. Background Execution (`&`)
- If last token is `&`, child process runs in background.  
- Shell prints: `PID <n> is sent to background` immediately and returns to prompt.  
- Background jobs are added to the process table.  

**Syscalls used:** `fork`, `execve`, `setpgid`.

---

### 4. I/O Redirection (`<`, `>`)
- Handles input (`<`) and output (`>`) redirection.  
- Supports chaining: `cmd < in.txt > out.txt`.  
- Implementation: open file with `open()`, then `dup2()` to redirect stdin/stdout.  

**Syscalls used:** `open`, `dup2`, `close`.

---

### 5. Single Pipe (`|`)
- Supports exactly one pipe: `cmd1 | cmd2`.  
- Creates a pipe with `pipe()`.  
- Forks twice: left child writes into pipe, right child reads from it.  
- Parent closes both ends and waits for children.  

**Syscalls used:** `pipe`, `fork`, `dup2`, `close`, `execve`.

---

### 6. Signal Handling
- Shell ignores `SIGINT` and `SIGTSTP` while waiting at prompt.  
- Foreground jobs inherit default handlers so they respond to `Ctrl-C` and `Ctrl-Z`.  
- Uses `tcsetpgrp()` to give terminal control to foreground jobs, then restores it to shell after job exits/suspends.  
- Handles `SIGCHLD` with `waitpid(WNOHANG | WUNTRACED | WCONTINUED)` to update job table (mark as `T`, `R`, or remove).  

**Syscalls used:** `sigaction`, `tcsetpgrp`, `waitpid`.

---

### 7. Graceful Exit
- On `exit`:  
  - Terminates all background and suspended processes.  
  - Removes them from the job table.  
  - Reclaims the terminal before terminating.  

---

## 📜 System Calls Used (Summary)

- **Process management:** `fork`, `execve`, `setpgid`, `waitpid`, `kill`.  
- **I/O and file ops:** `open`, `dup2`, `close`.  
- **Directory ops:** `chdir`, `getcwd`.  
- **Pipes and IPC:** `pipe`.  
- **Signals and terminal control:** `sigaction`, `tcsetpgrp`, `signal`, `perror`.

---

## ✅ Testing Methodology

I tested each rubric element with specific cases:

1. **Built-ins:**  
   - `pwd` (prints current dir).  
   - `cd ..` (navigates up).  
   - `cd` (no args → error).  
   - `cd nosuchdir` (error).  
   - `jobs` after background/suspended jobs.  
   - `exit` with background jobs running.

2. **External programs:**  
   - `./rand` (works).  
   - `rand` (bare in CWD).  
   - `../rand` (prints “Command not found”).  

3. **Background jobs:**  
   - `/bin/sleep 5 &` (prints PID + returns prompt).  
   - Run `jobs` immediately after.  

4. **I/O redirection:**  
   - `/bin/cat < input.txt` (reads from file).  
   - `/bin/echo hello > out.txt` + `cat out.txt`.  
   - `/bin/cat < input.txt > out.txt`.  

5. **Pipes:**  
   - `/bin/echo test | /usr/bin/tr a-z A-Z` → prints `TEST`.  

6. **Signal handling:**  
   - Run `/bin/sleep 30`, press `Ctrl-C` → job killed, not in jobs list.  
   - Run `./long`, press `Ctrl-Z` → job suspended, listed as `T`.  

7. **Graceful exit:**  
   - Run background jobs, then `exit`. Jobs terminated, shell exits cleanly.

---

## 📚 References

- Assignment spec & rubric (CMPUT 379 Fall 2025).  
- Linux man pages: `man 2 fork`, `man 2 execve`, `man 2 pipe`, `man 2 sigaction`, `man 3 tcsetpgrp`.  
- Course slides on signals and process groups.  

No external code copied; shell design is based on lecture material and system call documentation.
