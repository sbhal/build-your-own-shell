/*
 * COMPREHENSIVE UNIX SHELL IMPLEMENTATION WITH DEEP TECHNICAL ANNOTATIONS
 * ========================================================================
 * 
 * KERNEL-LEVEL UNDERSTANDING: How Unix Process Model Works
 * 
 * 1. PROCESS LIFECYCLE - THE FORK/EXEC/WAIT TRINITY:
 * 
 *    fork() - syscall: clone() on Linux, creates new process
 *    -------
 *    Kernel operation:
 *      - Allocates new task_struct (process descriptor)
 *      - Copies parent's memory mappings (COW - copy-on-write)
 *      - Duplicates file descriptor table (struct files_struct)
 *      - Inherits signal handlers, process group, session
 *      - Child gets new PID, same PPID as parent
 *    Returns: 0 in child, child's PID in parent, -1 on error
 *    Cost: ~100μs on modern systems (mostly page table setup)
 * 
 *    exec() - syscall: execve(path, argv, envp)
 *    -------
 *    Kernel operation:
 *      - Discards current process image (text, data, stack)
 *      - Loads new ELF binary from filesystem
 *      - Sets up new stack with argc, argv, envp
 *      - Resets signal handlers to SIG_DFL (except SIG_IGN)
 *      - Preserves PID, PPID, file descriptors (unless FD_CLOEXEC)
 *      - Jumps to entry point (_start in libc)
 *    Critical: exec() never returns on success!
 * 
 *    wait() - syscall: wait4(pid, &status, options, rusage)
 *    -------
 *    Kernel operation:
 *      - Blocks until child changes state (exit/stop/continue)
 *      - Reaps zombie (removes from process table)
 *      - Returns resource usage (CPU time, memory, etc.)
 *    Options:
 *      WNOHANG: Return immediately if no child ready
 *      WUNTRACED: Report stopped children (for job control)
 *      WCONTINUED: Report continued children
 *    Status macros:
 *      WIFEXITED(s): True if normal exit
 *      WEXITSTATUS(s): Extract exit code (0-255)
 *      WIFSIGNALED(s): True if killed by signal
 *      WTERMSIG(s): Extract signal number
 *      WIFSTOPPED(s): True if stopped (^Z)
 *      WSTOPSIG(s): Extract stop signal
 * 
 * 2. FILE DESCRIPTORS - KERNEL'S I/O ABSTRACTION:
 * 
 *    FD Table Structure (per-process):
 *      Process → files_struct → fd_array[] → file* → inode → device
 *    
 *    Each FD points to 'struct file' containing:
 *      - f_pos: Current file offset (lseek modifies this)
 *      - f_flags: O_RDONLY, O_WRONLY, O_RDWR, O_APPEND, O_NONBLOCK
 *      - f_mode: Kernel-internal permissions
 *      - f_op: Function pointers (read, write, ioctl, etc.)
 *    
 *    dup2(oldfd, newfd) - syscall: dup2()
 *    -------------------
 *    Atomically:
 *      1. Close newfd if open (ignoring errors)
 *      2. Make newfd point to same 'struct file' as oldfd
 *      3. Increment file's reference count
 *    Result: Both FDs share offset, flags, but have independent close()
 *    Use case: Redirection (dup2(filefd, STDOUT_FILENO))
 * 
 *    FD Inheritance:
 *      - fork(): Child gets copy of FD table (same files, incremented refcount)
 *      - exec(): FDs preserved unless FD_CLOEXEC flag set
 *      - fcntl(fd, F_SETFD, FD_CLOEXEC): Mark FD to close on exec
 * 
 * 3. PIPES - KERNEL BUFFER FOR IPC:
 * 
 *    pipe(pipefd[2]) - syscall: pipe2(pipefd, flags)
 *    ----------------
 *    Kernel operation:
 *      - Allocates circular buffer in kernel (default 64KB on Linux)
 *      - Creates two file descriptors:
 *        pipefd[0]: Read end (O_RDONLY)
 *        pipefd[1]: Write end (O_WRONLY)
 *      - Both point to same pipe_inode_info structure
 *    
 *    Pipe semantics:
 *      - Write blocks if buffer full (unless O_NONBLOCK)
 *      - Read blocks if buffer empty (unless O_NONBLOCK)
 *      - Read returns 0 (EOF) when all write ends closed
 *      - Write gets SIGPIPE when all read ends closed
 *    
 *    CRITICAL GOTCHA: Pipe deadlock
 *      If process keeps write end open while reading:
 *        read() never returns EOF → hangs forever
 *      Solution: Close unused pipe ends immediately after fork()
 * 
 * 4. JOB CONTROL - PROCESS GROUPS & SESSIONS:
 * 
 *    MENTAL MODEL: Why Process Groups Exist
 *    ========================================
 * 
 *    Problem: Pipelines need coordinated signal delivery
 *      $ cat file | grep pattern | wc -l
 *      User presses ^C → Should kill ALL three processes, not just one
 * 
 *    Solution: Process Groups (PGID)
 *      - Group of related processes (typically a pipeline)
 *      - Signals sent to entire group atomically: kill(-pgid, sig)
 *      - Terminal sends signals to foreground GROUP, not individual process
 * 
 *    Why terminal attaches to GROUP not PROCESS:
 *      1. PIPELINES: "ls | grep foo" creates 2 processes
 *         - Both must receive ^C simultaneously
 *         - If terminal tracked single PID, which one gets signal?
 *         - Process group solves this: both in same group
 *      
 *      2. ATOMIC SIGNAL DELIVERY:
 *         kill(-pgid, SIGINT) → kernel sends to ALL processes in group
 *         No race: All processes get signal before any can exit
 *      
 *      3. BACKGROUND JOBS:
 *         Shell can move entire job (pipeline) to background
 *         Terminal ignores background groups (no ^C, no input)
 *      
 *      4. HIERARCHICAL CONTROL:
 *         Session → Multiple Process Groups → Multiple Processes
 *         Terminal controls which GROUP is foreground
 *         Only one group can be foreground at a time
 * 
 *    Process hierarchy:
 *      Session (SID)
 *        ├─ Process Group 1 (PGID) ← Foreground (has terminal)
 *        │    ├─ Process A (PID)
 *        │    └─ Process B (PID)
 *        ├─ Process Group 2 (PGID) ← Background
 *        │    └─ Process C (PID)
 *        └─ Shell Process Group (PGID) ← Session leader
 *             └─ Shell (PID)
 * 
 *    Why shell in its own process group:
 *      1. ISOLATION: Shell must not receive job control signals
 *         - If shell in same group as job, ^C kills shell!
 *         - Shell creates new group for each job
 *      
 *      2. TERMINAL CONTROL: Shell is session leader
 *         - Only session leader can call tcsetpgrp()
 *         - Shell gives terminal to job, then reclaims it
 *      
 *      3. SIGNAL FLOW:
 *         User presses ^C:
 *           Terminal → Foreground PGID → All processes in that group
 *           Shell is NOT in foreground group → Shell survives
 * 
 *    Interactive vs Non-interactive:
 *      Interactive shell (terminal attached):
 *        - Shell becomes session leader: setsid()
 *        - Shell puts itself in own group: setpgid(0, 0)
 *        - Each job gets new group: setpgid(child, child)
 *        - Shell controls terminal: tcsetpgrp(tty, job_pgid)
 *      
 *      Non-interactive shell (script, pipe):
 *        - No terminal control needed
 *        - All processes can share same group
 *        - No job control (no ^C, ^Z)
 *        - Simpler: just fork/exec/wait
 * 
 *    setpgid(pid, pgid) - syscall: setpgid()
 *    -------------------
 *    Sets process group ID:
 *      - pgid == 0: Use pid as pgid (create new group)
 *      - pgid == pid: Make process group leader
 *      - Must be called in both parent and child (race condition)
 *    Restrictions:
 *      - Can only modify own children before they exec()
 *      - Process and target must be in same session
 *    
 *    Terminal ownership:
 *      - Each terminal has controlling process group (foreground)
 *      - Only foreground group can read/write terminal
 *      - Background read → SIGTTIN (stops process)
 *      - Background write → SIGTTOU (if TOSTOP set)
 * 
 *    SIGNAL FLOW MENTAL MODEL:
 *    =========================
 * 
 *    Scenario: User runs "sleep 100 | cat" and presses ^C
 * 
 *    Setup:
 *      Shell (PID 1000, PGID 1000) ← Session leader, NOT foreground
 *      Job (PGID 2000) ← Foreground group
 *        ├─ sleep (PID 2000)
 *        └─ cat (PID 2001)
 * 
 *    Signal flow:
 *      1. User presses ^C
 *      2. Terminal driver (kernel) sees VINTR character
 *      3. Kernel checks: termios.c_lflag & ISIG? Yes
 *      4. Kernel reads: tty->pgrp = 2000 (foreground group)
 *      5. Kernel calls: kill(-2000, SIGINT)
 *         ↓
 *         Sends SIGINT to ALL processes where pgid == 2000
 *         ↓
 *         sleep gets SIGINT → dies
 *         cat gets SIGINT → dies
 *         Shell (pgid 1000) → NOT affected!
 *      6. Shell receives SIGCHLD (children died)
 *      7. Shell calls waitpid(), reaps zombies
 *      8. Shell reclaims terminal: tcsetpgrp(tty, 1000)
 * 
 *    Why shell ignores SIGINT:
 *      - Shell sets: signal(SIGINT, SIG_IGN)
 *      - Reason: Shell is NOT in foreground group
 *      - But what if shell accidentally gets SIGINT?
 *        (Bug, race condition, manual kill)
 *      - SIG_IGN ensures shell survives
 *      - Children inherit SIG_IGN, must reset to SIG_DFL
 * 
 *    Confusion clarified:
 *      Q: "^C signal sent to child, not shell, why ignore?"
 *      A: Defense in depth!
 *         - Normal case: Signal goes to foreground group (child)
 *         - Shell not in foreground, shouldn't receive signal
 *         - BUT: Ignore anyway for safety (bugs, races, manual kill)
 *         - If shell didn't ignore and got signal → shell dies!
 * 
 *    Process group enables:
 *      1. Atomic signal delivery to pipelines
 *      2. Background job management (move group to background)
 *      3. Terminal arbitration (one group at a time)
 *      4. Shell isolation (shell in separate group)
 *      5. Job control (stop/continue entire pipeline)
 * 
 * 5. SIGNALS - ASYNCHRONOUS PROCESS CONTROL:
 * 
 *    Signal delivery mechanism:
 *      1. Kernel marks signal pending in task_struct
 *      2. On return from syscall/interrupt, kernel checks pending signals
 *      3. If handler installed, kernel sets up signal frame on user stack
 *      4. Process returns to user mode, executes handler
 *      5. Handler returns via sigreturn() syscall
 *    
 *    sigaction(sig, &act, &oldact) - syscall: rt_sigaction()
 *    ------------------------------
 *    struct sigaction:
 *      sa_handler: SIG_DFL (default), SIG_IGN (ignore), or function pointer
 *      sa_mask: Signals blocked during handler execution
 *      sa_flags:
 *        SA_RESTART: Restart interrupted syscalls automatically
 *        SA_NOCLDSTOP: Don't receive SIGCHLD when children stop
 *        SA_NODEFER: Don't block signal during its own handler
 *        SA_RESETHAND: Reset to SIG_DFL after one delivery
 *    
 *    Key signals for shells:
 *      SIGINT (2): ^C - Interrupt (terminate)
 *      SIGQUIT (3): ^\ - Quit with core dump
 *      SIGTSTP (20): ^Z - Stop (suspend)
 *      SIGCONT (18): Resume stopped process
 *      SIGCHLD (17): Child status changed
 *      SIGTTIN (21): Background read from terminal
 *      SIGTTOU (22): Background write to terminal
 *      SIGPIPE (13): Write to pipe with no readers
 * 
 * 6. TERMINAL CONTROL - TTY SUBSYSTEM:
 * 
 *    tcgetpgrp(fd) - syscall: ioctl(fd, TIOCGPGRP, &pgid)
 *    --------------
 *    Returns foreground process group of terminal
 *    
 *    tcsetpgrp(fd, pgid) - syscall: ioctl(fd, TIOCSPGRP, &pgid)
 *    --------------------
 *    Sets foreground process group:
 *      - Only session leader can call this
 *      - pgid must be in same session
 *      - Terminal sends signals (INT, QUIT, TSTP) to this group
 *    
 *    tcgetattr/tcsetattr - syscall: ioctl(fd, TCGETS/TCSETS, &termios)
 *    --------------------
 *    struct termios controls:
 *      c_iflag: Input modes (ICRNL: CR→NL, IXON: XON/XOFF flow control)
 *      c_oflag: Output modes (OPOST: post-process output)
 *      c_cflag: Control modes (CSIZE: char size, PARENB: parity)
 *      c_lflag: Local modes:
 *        ICANON: Canonical mode (line buffering, editing)
 *        ECHO: Echo input characters
 *        ISIG: Generate signals for ^C, ^Z, ^\
 *        IEXTEN: Extended processing
 *      c_cc[]: Control characters (VINTR=^C, VSUSP=^Z, VEOF=^D)
 *    
 *    Canonical vs Raw mode:
 *      Canonical: Line buffering, editing (backspace, ^U, ^W)
 *      Raw: Character-at-a-time, no processing
 * 
 * 7. VARIABLE EXPANSION - SHELL WORD PROCESSING:
 * 
 *    Expansion order (POSIX):
 *      1. Tilde expansion (~user → /home/user)
 *      2. Parameter expansion ($VAR, ${VAR:-default})
 *      3. Command substitution ($(cmd), `cmd`)
 *      4. Arithmetic expansion ($((expr)))
 *      5. Field splitting (IFS-based word splitting)
 *      6. Pathname expansion (globbing: *, ?, [...])
 *      7. Quote removal
 *    
 *    glob() - library function using getdents() syscall
 *    -------
 *    Pattern matching:
 *      *: Matches any string (including empty)
 *      ?: Matches any single character
 *      [...]: Matches any character in set
 *      [!...]: Matches any character not in set
 *    Implementation:
 *      - Reads directory entries via getdents64()
 *      - Matches each entry against pattern
 *      - Sorts results lexicographically
 * 
 * 8. QUOTING - CONTROLLING EXPANSION:
 * 
 *    'single': Preserves literal value of all characters
 *    "double": Preserves literal except $, `, \, "
 *    \: Escapes next character (even in double quotes)
 *    
 *    Implementation: State machine tracking quote context
 */

#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
#include <termios.h>
#include <signal.h>
#include <glob.h>
#include <pwd.h>

#define MAX_LINE 4096
#define MAX_ARGS 128
#define MAX_CMDS 64
#define MAX_REDIRECTS 16
#define MAX_JOBS 64
#define MAX_VARS 256

/* Job states */
typedef enum { JOB_RUNNING, JOB_STOPPED, JOB_DONE } job_state_t;

/* Job structure for job control */
typedef struct {
    int id;
    pid_t pgid;
    job_state_t state;
    char *command;
} job_t;

/* Variable storage */
typedef struct {
    char *name;
    char *value;
    int exported;
} var_t;

/* Redirection */
typedef struct {
    int fd;
    char *file;
    int flags;
    mode_t mode;
} redirect_t;

/* Command in pipeline */
typedef struct {
    char *args[MAX_ARGS];
    int argc;
    redirect_t redirects[MAX_REDIRECTS];
    int nredirects;
} command_t;

/* Pipeline (job) */
typedef struct {
    command_t cmds[MAX_CMDS];
    int ncmds;
    int negate;
    int background;
} pipeline_t;

/* Global state */
static int last_status = 0;
static pid_t shell_pgid;
static int shell_terminal;
static int interactive = 0;
static struct termios shell_tmodes;
static job_t jobs[MAX_JOBS];
static int njobs = 0;
static var_t vars[MAX_VARS];
static int nvars = 0;
static pid_t last_bg_pid = 0;

/* Forward declarations */
static void init_shell(void);
static int execute_pipeline(pipeline_t *pl);
static char *expand_word(const char *word);

/* Error handling */
static void die(const char *msg) {
    perror(msg);
    exit(1);
}

/*
 * VARIABLE MANAGEMENT - ENVIRONMENT PASSING MECHANISM
 * 
 * Shell variables vs environment variables:
 * - Shell variables: Stored in shell's memory only
 * - Environment variables: Passed to children via execve()
 * 
 * execve() environment passing:
 *   execve(path, argv, envp)
 *          ↓
 *   Kernel copies envp strings to new process stack
 *          ↓
 *   Child's main() receives: int main(int argc, char *argv[], char *envp[])
 *          ↓
 *   libc sets global 'environ' pointer to envp
 *          ↓
 *   getenv()/setenv() search this array
 * 
 * setenv(name, value, overwrite) - library function
 * -------------------------------
 * Implementation:
 *   1. Search environ[] for "name="
 *   2. If found and overwrite: Replace value
 *   3. If not found: Realloc environ[], append "name=value"
 * Note: Not a syscall! Modifies process memory only.
 * 
 * Key insight: export marks variable for inclusion in envp when exec'ing.
 * The shell maintains two lists:
 *   1. Shell variables (local)
 *   2. Exported variables (copied to envp on exec)
 */

static var_t *find_var(const char *name) {
    for (int i = 0; i < nvars; i++) {
        if (strcmp(vars[i].name, name) == 0) {
            return &vars[i];
        }
    }
    return NULL;
}

static void set_var(const char *name, const char *value, int exported) {
    var_t *v = find_var(name);
    if (v) {
        free(v->value);
        v->value = strdup(value);
        if (exported) v->exported = 1;
    } else if (nvars < MAX_VARS) {
        vars[nvars].name = strdup(name);
        vars[nvars].value = strdup(value);
        vars[nvars].exported = exported;
        nvars++;
    }
    
    if (exported) {
        setenv(name, value, 1);
    }
}

static const char *get_var(const char *name) {
    /* Special parameters */
    static char buf[32];
    if (strcmp(name, "?") == 0) {
        snprintf(buf, sizeof(buf), "%d", last_status);
        return buf;
    }
    if (strcmp(name, "$") == 0) {
        snprintf(buf, sizeof(buf), "%d", getpid());
        return buf;
    }
    if (strcmp(name, "!") == 0) {
        snprintf(buf, sizeof(buf), "%d", last_bg_pid);
        return buf;
    }
    
    var_t *v = find_var(name);
    if (v) return v->value;
    return getenv(name);
}

/*
 * JOB CONTROL - TERMINAL MULTIPLEXING VIA PROCESS GROUPS
 * 
 * Kernel data structures:
 *   struct tty_struct {
 *     pid_t pgrp;           // Foreground process group (NOT single PID!)
 *     struct session *session;
 *     struct termios termios;
 *     ...
 *   };
 * 
 * Why tty->pgrp is PGID not PID:
 *   - Pipelines have multiple processes
 *   - All need coordinated signal delivery
 *   - kill(-pgid, sig) sends to entire group
 *   - Terminal can't track multiple PIDs, uses one PGID
 * 
 * Job control protocol:
 * 
 * 1. LAUNCHING FOREGROUND JOB (PIPELINE):
 *    Example: "ls | grep foo"
 *    
 *    Shell (PGID 1000):
 *      fork() → child1 (PID 2000)
 *      fork() → child2 (PID 2001)
 *      
 *      setpgid(2000, 2000)  // Make child1 group leader
 *      setpgid(2001, 2000)  // Put child2 in same group
 *      
 *      tcsetpgrp(tty, 2000) → ioctl(tty, TIOCSPGRP, &pgid)
 *                                      ↓
 *                            Kernel: tty->pgrp = 2000
 *      
 *      Now terminal "belongs" to PGID 2000 (both processes)
 *    
 *    Child1 (ls):
 *      setpgid(0, 0)  // Ensure in correct group (race with parent)
 *      exec("ls")
 *    
 *    Child2 (grep):
 *      setpgid(0, getpid_of_child1)  // Join child1's group
 *      exec("grep")
 *    
 *    Result: Both processes in PGID 2000, terminal attached to 2000
 * 
 * 2. USER PRESSES ^C:
 *    Terminal driver (kernel tty_io.c):
 *      n_tty_receive_char(tty, '^C'):
 *        if (c == termios.c_cc[VINTR] && L_ISIG(tty)) {
 *          isig(SIGINT, tty);  // Send to foreground group
 *        }
 *      
 *      isig(SIGINT, tty):
 *        pgid = tty->pgrp;  // Get foreground group (2000)
 *        kill_pgrp(pgid, SIGINT, 1);  // Send to ALL in group
 *      
 *      kill_pgrp(2000, SIGINT, 1):
 *        for each process p where p->pgid == 2000:
 *          send_signal(SIGINT, p)  // ls gets it, grep gets it
 *    
 *    Result: Both ls and grep receive SIGINT simultaneously
 *            Shell (PGID 1000) does NOT receive signal
 *    
 *    Shell receives SIGCHLD:
 *      waitpid(-1, &status, WUNTRACED)
 *      WIFSIGNALED(status) → true, WTERMSIG(status) → SIGINT
 *      Shell reclaims terminal: tcsetpgrp(tty, 1000)
 * 
 * 3. USER PRESSES ^Z:
 *    Terminal driver (kernel):
 *      - Sees VSUSP character (^Z)
 *      - Checks termios.c_lflag & ISIG
 *      - Calls kill(-tty->pgrp, SIGTSTP)
 *                    ↓
 *          Sends SIGTSTP to all processes in foreground group
 *                    ↓
 *          Default action: Stop process (TASK_STOPPED)
 *    
 *    Shell receives SIGCHLD:
 *      waitpid(-1, &status, WUNTRACED)
 *      WIFSTOPPED(status) → true
 *      Shell reclaims terminal: tcsetpgrp(tty, shell_pgid)
 *      Shell prints: [1]+ Stopped    ls | grep foo
 * 
 * 4. BACKGROUND JOB TRIES TO READ:
 *    Kernel (tty read path in n_tty_read()):
 *      if (current->pgrp != tty->pgrp) {  // Not foreground?
 *        if (is_ignored(SIGTTIN) || is_orphaned_pgrp(current->pgrp)) {
 *          return -EIO;  // Error
 *        }
 *        kill(-current->pgrp, SIGTTIN);  // Stop entire group
 *        return -ERESTARTSYS;  // Restart syscall after signal
 *      }
 *    Result: Entire background group stops, shell gets SIGCHLD
 * 
 * 5. CONTINUING STOPPED JOB:
 *    Shell: kill(-pgid, SIGCONT)  // Resume entire group
 *           tcsetpgrp(tty, pgid)  // If bringing to foreground
 *    Kernel: Changes state TASK_STOPPED → TASK_RUNNING
 *            Sends SIGCHLD to parent
 * 
 * SIGNIFICANCE OF PROCESS GROUPS:
 * ================================
 * 
 * 1. COORDINATED SIGNAL DELIVERY:
 *    Without groups: kill(pid, sig) → one process
 *    With groups: kill(-pgid, sig) → all processes atomically
 * 
 * 2. PIPELINE MANAGEMENT:
 *    "cmd1 | cmd2 | cmd3" → All in same group
 *    ^C kills all three simultaneously
 *    ^Z stops all three simultaneously
 * 
 * 3. TERMINAL ARBITRATION:
 *    Only one group can be foreground
 *    Background groups can't read terminal (SIGTTIN)
 *    Prevents multiple jobs fighting for input
 * 
 * 4. JOB ABSTRACTION:
 *    User thinks: "I have 3 jobs running"
 *    Kernel thinks: "I have 3 process groups"
 *    Job = Process Group (may contain multiple processes)
 * 
 * 5. SHELL ISOLATION:
 *    Shell in separate group → immune to job signals
 *    Shell can manage jobs without being affected
 * 
 * Why only interactive shells use process groups:
 *   - Non-interactive: No terminal, no ^C/^Z, no job control
 *   - All processes can share same group (simpler)
 *   - Interactive: Terminal control essential
 *   - Must isolate shell, manage foreground/background
 */

static void add_job(pid_t pgid, const char *cmd, int background) {
    if (njobs >= MAX_JOBS) return;
    jobs[njobs].id = njobs + 1;
    jobs[njobs].pgid = pgid;
    jobs[njobs].state = JOB_RUNNING;
    jobs[njobs].command = strdup(cmd);
    njobs++;
    
    if (background) {
        printf("[%d] %d\n", njobs, pgid);
    }
}

static job_t *find_job(pid_t pgid) {
    for (int i = 0; i < njobs; i++) {
        if (jobs[i].pgid == pgid) return &jobs[i];
    }
    return NULL;
}

static void remove_job(pid_t pgid) {
    for (int i = 0; i < njobs; i++) {
        if (jobs[i].pgid == pgid) {
            free(jobs[i].command);
            memmove(&jobs[i], &jobs[i+1], (njobs - i - 1) * sizeof(job_t));
            njobs--;
            return;
        }
    }
}

/*
 * SIGNAL HANDLING - ASYNCHRONOUS EVENT NOTIFICATION
 * 
 * Signal disposition inheritance:
 *   fork(): Child inherits parent's signal handlers
 *   exec(): Resets handlers to SIG_DFL (except SIG_IGN preserved)
 * 
 * WHY SHELL IGNORES JOB CONTROL SIGNALS - DEFENSE IN DEPTH:
 * ==========================================================
 * 
 * Confusion: "^C goes to child, not shell, so why ignore?"
 * 
 * Answer: Multiple layers of protection!
 * 
 * Layer 1: Process Group Isolation (primary defense)
 *   - Shell in PGID 1000
 *   - Job in PGID 2000
 *   - Terminal attached to PGID 2000
 *   - ^C sends signal to PGID 2000 only
 *   - Shell (PGID 1000) not targeted
 * 
 * Layer 2: Signal Ignore (defense in depth)
 *   - Shell sets: signal(SIGINT, SIG_IGN)
 *   - Why? What if shell accidentally gets signal?
 *     • Bug in terminal driver
 *     • Race condition during tcsetpgrp()
 *     • User manually: kill -INT <shell_pid>
 *     • Child hasn't called setpgid() yet
 *   - If shell didn't ignore → shell dies → user loses session!
 * 
 * Real-world scenario where shell gets signal:
 *   1. Shell forks child
 *   2. Shell calls tcsetpgrp(tty, child_pgid)
 *   3. User presses ^C (very fast!)
 *   4. Child hasn't called setpgid() yet
 *   5. Child still in shell's group!
 *   6. Signal goes to shell's group → shell gets SIGINT
 *   7. If shell didn't ignore → shell dies!
 * 
 * Why children must reset to SIG_DFL:
 *   - Children inherit SIG_IGN from shell
 *   - exec() preserves SIG_IGN (special case!)
 *   - If child doesn't reset: ^C won't kill child!
 *   - Child must: signal(SIGINT, SIG_DFL) before exec()
 * 
 * Signal flow comparison:
 * 
 *   Normal case (shell in separate group):
 *     User presses ^C
 *       ↓
 *     Terminal: kill(-foreground_pgid, SIGINT)
 *       ↓
 *     Child (in foreground group) gets SIGINT → dies
 *     Shell (in different group) → not targeted
 *   
 *   Race condition (child not yet in new group):
 *     User presses ^C
 *       ↓
 *     Terminal: kill(-foreground_pgid, SIGINT)
 *       ↓
 *     Child (still in shell's group!) gets SIGINT → dies
 *     Shell (same group) gets SIGINT → IGNORED → survives!
 * 
 * Signal delivery race condition:
 *   Problem: SIGCHLD can arrive before parent calls waitpid()
 *   Solution: SA_RESTART flag + WNOHANG in handler
 * 
 * sigaction() vs signal():
 *   signal(): Old API, unreliable (handler resets to SIG_DFL)
 *   sigaction(): Modern API, reliable, more control
 * 
 * SIGCHLD handler implementation:
 *   - Must use waitpid(-1, ..., WNOHANG) to avoid blocking
 *   - Must loop until waitpid returns 0 (no more children)
 *   - Must handle EINTR (interrupted by another signal)
 *   - Must be async-signal-safe (no malloc, printf, etc.)
 * 
 * Async-signal-safe functions (POSIX.1-2008):
 *   Safe: write(), _exit(), waitpid(), kill(), sigaction()
 *   Unsafe: printf(), malloc(), free(), most library functions
 *   Reason: Non-reentrant (use global state, locks)
 * 
 * Signal mask (blocked signals):
 *   sigprocmask(SIG_BLOCK, &set, &oldset) - syscall: rt_sigprocmask()
 *   Kernel maintains per-thread blocked signal mask
 *   Blocked signals remain pending until unblocked
 *   Used to create critical sections
 */

/*
 * SIGCHLD HANDLER - REAPING ZOMBIE PROCESSES
 * 
 * Zombie process: Terminated but not yet reaped (still in process table)
 * - Holds PID, exit status, resource usage
 * - Removed only when parent calls wait()
 * - If parent dies, init (PID 1) adopts and reaps
 * 
 * waitpid() flags:
 *   WNOHANG: Return immediately if no child ready (non-blocking)
 *   WUNTRACED: Report stopped children (for ^Z handling)
 *   WCONTINUED: Report continued children (for fg/bg)
 * 
 * Why loop with WNOHANG:
 *   - Multiple children may have changed state
 *   - Signals are not queued (multiple SIGCHLDs may coalesce)
 *   - Must reap all ready children to avoid zombies
 * 
 * Race condition:
 *   Parent: fork() → ... → waitpid()
 *   Child:  ... → exit()
 *   SIGCHLD may arrive before parent reaches waitpid()
 *   Solution: Handler reaps, main code checks errno == ECHILD
 */
static void sigchld_handler(int sig) {
    (void)sig;
    int status;
    pid_t pid;
    int saved_errno = errno;  /* Preserve errno across handler */
    
    /* Reap all dead children without blocking
     * waitpid(-1, ...) waits for any child
     * Returns: PID on success, 0 if WNOHANG and no child ready, -1 on error
     */
    while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED | WCONTINUED)) > 0) {
        job_t *job = find_job(pid);
        if (!job) continue;
        
        /* Status decoding:
         * WIFEXITED(s): (s & 0x7f) == 0
         * WEXITSTATUS(s): (s >> 8) & 0xff
         * WIFSIGNALED(s): ((s & 0x7f) + 1) >> 1 > 0
         * WTERMSIG(s): s & 0x7f
         * WIFSTOPPED(s): (s & 0xff) == 0x7f
         * WSTOPSIG(s): (s >> 8) & 0xff
         */
        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            job->state = JOB_DONE;
            /* Note: printf() is NOT async-signal-safe!
             * In production, use write() with pre-formatted buffer */
            printf("[%d] Done    %s\n", job->id, job->command);
            remove_job(pid);
        } else if (WIFSTOPPED(status)) {
            job->state = JOB_STOPPED;
            printf("[%d] Stopped %s\n", job->id, job->command);
        } else if (WIFCONTINUED(status)) {
            job->state = JOB_RUNNING;
        }
    }
    errno = saved_errno;  /* Restore errno */
}

/*
 * SIGNAL INITIALIZATION - SETTING UP SHELL'S SIGNAL ENVIRONMENT
 * 
 * Why shell ignores job control signals (DEFENSE IN DEPTH):
 *   Primary: Shell in separate process group (not targeted by ^C)
 *   Backup: Ignore anyway (protects against races, bugs, manual kill)
 * 
 * The race that justifies ignoring:
 *   Time 0: Shell forks child (child in shell's group)
 *   Time 1: Shell calls tcsetpgrp(tty, child_pgid)
 *   Time 2: User presses ^C (FAST!)
 *   Time 3: Child calls setpgid(0, 0)
 *   
 *   If ^C at Time 2: Child still in shell's group!
 *   Signal goes to shell's group → shell gets SIGINT
 *   If shell didn't ignore → shell dies!
 * 
 * Signal inheritance:
 *   - Child inherits SIG_IGN (preserved across exec)
 *   - Child must reset to SIG_DFL before exec
 *   - Otherwise ^C won't work on child!
 */
static void init_signals(void) {
    struct sigaction sa;
    
    /* Ignore interactive signals in shell
     * 
     * Why ignore if shell not in foreground group?
     * DEFENSE IN DEPTH:
     *   - Normal: Signal goes to foreground group (child)
     *   - Race: Child not yet in new group, signal hits shell
     *   - Bug: Terminal driver error, manual kill
     *   - Ignore ensures shell survives all cases
     * 
     * SIG_IGN is preserved across exec(), so children inherit
     * Children must explicitly reset to SIG_DFL before exec
     * Otherwise ^C won't kill child!
     */
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);  /* Don't block additional signals */
    sa.sa_flags = 0;
    
    /* SIGINT: ^C (interrupt) */
    sigaction(SIGINT, &sa, NULL);
    /* SIGQUIT: ^\ (quit with core dump) */
    sigaction(SIGQUIT, &sa, NULL);
    /* SIGTSTP: ^Z (suspend) */
    sigaction(SIGTSTP, &sa, NULL);
    /* SIGTTIN: Background read from terminal */
    sigaction(SIGTTIN, &sa, NULL);
    /* SIGTTOU: Background write to terminal */
    sigaction(SIGTTOU, &sa, NULL);
    
    /* Handle SIGCHLD to reap background jobs
     * SA_RESTART: Restart interrupted syscalls (read, write, etc.)
     * SA_NOCLDSTOP: Don't receive SIGCHLD when children stop (only exit)
     *               We use WUNTRACED in waitpid() instead for explicit control
     */
    sa.sa_handler = sigchld_handler;
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);
}

/*
 * PATH SEARCH - LOCATING EXECUTABLES
 * 
 * PATH environment variable format: "/usr/bin:/bin:/usr/local/bin"
 * 
 * access(path, X_OK) - syscall: faccessat()
 * -------------------
 * Checks if file exists and is executable:
 *   - Follows symlinks
 *   - Checks real UID/GID (not effective)
 *   - Returns 0 if accessible, -1 if not
 * 
 * Alternative: stat() + S_IXUSR check
 *   struct stat st;
 *   stat(path, &st);
 *   if (st.st_mode & S_IXUSR) { // executable // }
 * 
 * Security note: TOCTOU race (Time-Of-Check-Time-Of-Use)
 *   access() → ... → exec()
 *   File could change between check and use!
 *   Better: Just try exec() and handle ENOENT/EACCES
 */
static char *find_in_path(const char *cmd) {
    static char buf[MAX_LINE];
    
    if (strchr(cmd, '/')) return (char *)cmd;
    
    char *path = getenv("PATH");
    if (!path) path = "/usr/bin:/bin";
    
    char *path_copy = strdup(path);
    char *dir = strtok(path_copy, ":");
    
    while (dir) {
        snprintf(buf, sizeof(buf), "%s/%s", dir, cmd);
        if (access(buf, X_OK) == 0) {
            free(path_copy);
            return buf;
        }
        dir = strtok(NULL, ":");
    }
    
    free(path_copy);
    return NULL;
}

/*
 * BUILTINS
 */

static int builtin_cd(command_t *cmd) {
    char *dir = cmd->args[1] ? cmd->args[1] : getenv("HOME");
    if (!dir) {
        fprintf(stderr, "cd: HOME not set\n");
        return 1;
    }
    if (chdir(dir) < 0) {
        perror("cd");
        return 1;
    }
    return 0;
}

/*
 * BUILTIN: export - MARKING VARIABLES FOR ENVIRONMENT EXPORT
 * 
 * export VAR=value: Set and export
 * export VAR: Export existing variable
 * 
 * Implementation:
 *   1. Parse VAR=value or VAR
 *   2. Update shell's variable table
 *   3. Call setenv() to update environ[]
 *   4. Mark variable as exported
 * 
 * setenv() implementation (glibc):
 *   - Searches environ[] for "name="
 *   - If found: Replaces value (may realloc)
 *   - If not found: Expands environ[], appends "name=value"
 *   - environ[] is NULL-terminated array of "name=value" strings
 */
static int builtin_export(command_t *cmd) {
    for (int i = 1; i < cmd->argc; i++) {
        char *eq = strchr(cmd->args[i], '=');
        if (eq) {
            *eq = '\0';
            set_var(cmd->args[i], eq + 1, 1);
            *eq = '=';
        } else {
            var_t *v = find_var(cmd->args[i]);
            if (v) {
                v->exported = 1;
                setenv(v->name, v->value, 1);
            }
        }
    }
    return 0;
}

static int builtin_fg(command_t *cmd) {
    (void)cmd;
    if (njobs == 0) {
        fprintf(stderr, "fg: no jobs\n");
        return 1;
    }
    
    job_t *job = &jobs[njobs - 1];
    tcsetpgrp(shell_terminal, job->pgid);
    killpg(job->pgid, SIGCONT);
    
    int status;
    waitpid(-job->pgid, &status, WUNTRACED);
    tcsetpgrp(shell_terminal, shell_pgid);
    
    if (WIFEXITED(status)) {
        last_status = WEXITSTATUS(status);
        remove_job(job->pgid);
    } else if (WIFSTOPPED(status)) {
        job->state = JOB_STOPPED;
    }
    
    return last_status;
}

static int builtin_bg(command_t *cmd) {
    (void)cmd;
    if (njobs == 0) {
        fprintf(stderr, "bg: no jobs\n");
        return 1;
    }
    
    job_t *job = &jobs[njobs - 1];
    if (job->state == JOB_STOPPED) {
        killpg(job->pgid, SIGCONT);
        job->state = JOB_RUNNING;
    }
    return 0;
}

static int builtin_jobs(command_t *cmd) {
    (void)cmd;
    for (int i = 0; i < njobs; i++) {
        const char *state = jobs[i].state == JOB_RUNNING ? "Running" : "Stopped";
        printf("[%d] %s    %s\n", jobs[i].id, state, jobs[i].command);
    }
    return 0;
}

static int is_builtin(const char *cmd) {
    return strcmp(cmd, "cd") == 0 ||
           strcmp(cmd, "export") == 0 ||
           strcmp(cmd, "fg") == 0 ||
           strcmp(cmd, "bg") == 0 ||
           strcmp(cmd, "jobs") == 0;
}

static int run_builtin(command_t *cmd) {
    if (strcmp(cmd->args[0], "cd") == 0) return builtin_cd(cmd);
    if (strcmp(cmd->args[0], "export") == 0) return builtin_export(cmd);
    if (strcmp(cmd->args[0], "fg") == 0) return builtin_fg(cmd);
    if (strcmp(cmd->args[0], "bg") == 0) return builtin_bg(cmd);
    if (strcmp(cmd->args[0], "jobs") == 0) return builtin_jobs(cmd);
    return 1;
}

/*
 * REDIRECTION SETUP
 * 
 * Called in child after fork, before exec.
 * Opens files and uses dup2 to redirect FDs.
 */
static void setup_redirects(command_t *cmd) {
    for (int i = 0; i < cmd->nredirects; i++) {
        int fd = open(cmd->redirects[i].file, cmd->redirects[i].flags,
                     cmd->redirects[i].mode);
        if (fd < 0) {
            perror(cmd->redirects[i].file);
            exit(1);
        }
        
        if (dup2(fd, cmd->redirects[i].fd) < 0) {
            perror("dup2");
            exit(1);
        }
        close(fd);
    }
}

/*
 * PIPELINE EXECUTION
 * 
 * Core algorithm:
 * 1. Create all pipes upfront
 * 2. Fork each command, setting up pipe FDs
 * 3. First child creates new PGRP, others join it
 * 4. Give terminal to PGRP if foreground
 * 5. Close all pipe FDs in parent
 * 6. Wait for completion (foreground) or return (background)
 * 
 * Race condition handling: Both parent and child call setpgid() to
 * avoid race where parent tries to tcsetpgrp() before child setpgid().
 */
static int execute_pipeline(pipeline_t *pl) {
    if (pl->ncmds == 0) return 0;
    
    /* Single builtin without pipes */
    if (pl->ncmds == 1 && is_builtin(pl->cmds[0].args[0]) && !pl->background) {
        int status = run_builtin(&pl->cmds[0]);
        return pl->negate ? !status : status;
    }
    
    int pipes[MAX_CMDS][2];
    pid_t pids[MAX_CMDS];
    pid_t pgid = 0;
    
    /* Create pipes */
    for (int i = 0; i < pl->ncmds - 1; i++) {
        if (pipe(pipes[i]) < 0) die("pipe");
    }
    
    /* Fork and execute commands */
    for (int i = 0; i < pl->ncmds; i++) {
        pid_t pid = fork();
        if (pid < 0) die("fork");
        
        if (pid == 0) {  /* Child */
            /* Reset signal handlers to default */
            signal(SIGINT, SIG_DFL);
            signal(SIGQUIT, SIG_DFL);
            signal(SIGTSTP, SIG_DFL);
            signal(SIGTTIN, SIG_DFL);
            signal(SIGTTOU, SIG_DFL);
            signal(SIGCHLD, SIG_DFL);
            
            /* Set process group */
            if (i == 0) {
                pgid = getpid();
                setpgid(0, pgid);
                if (interactive && !pl->background) {
                    tcsetpgrp(shell_terminal, pgid);
                }
            } else {
                setpgid(0, pgid);
            }
            
            /* Setup pipes */
            if (i > 0) {
                dup2(pipes[i-1][0], 0);
            }
            if (i < pl->ncmds - 1) {
                dup2(pipes[i][1], 1);
            }
            
            /* Close all pipe FDs */
            for (int j = 0; j < pl->ncmds - 1; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }
            
            /* Setup redirections */
            setup_redirects(&pl->cmds[i]);
            
            /* Execute builtin or external command */
            if (is_builtin(pl->cmds[i].args[0])) {
                exit(run_builtin(&pl->cmds[i]));
            }
            
            char *path = find_in_path(pl->cmds[i].args[0]);
            if (!path) {
                fprintf(stderr, "%s: command not found\n", pl->cmds[i].args[0]);
                exit(127);
            }
            
            execv(path, pl->cmds[i].args);
            perror("execv");
            exit(1);
        }
        
        /* Parent */
        pids[i] = pid;
        if (i == 0) {
            pgid = pid;
            setpgid(pid, pgid);
            if (interactive && !pl->background) {
                tcsetpgrp(shell_terminal, pgid);
            }
        } else {
            setpgid(pid, pgid);
        }
    }
    
    /* Close all pipes in parent */
    for (int i = 0; i < pl->ncmds - 1; i++) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }
    
    last_bg_pid = pgid;
    
    if (pl->background) {
        add_job(pgid, "background job", 1);
        return 0;
    }
    
    /* Wait for foreground job */
    int status = 0;
    for (int i = 0; i < pl->ncmds; i++) {
        int wstatus;
        waitpid(pids[i], &wstatus, WUNTRACED);
        
        if (WIFSTOPPED(wstatus)) {
            add_job(pgid, "stopped job", 0);
            printf("[%d] Stopped\n", njobs);
            if (interactive) {
                tcsetpgrp(shell_terminal, shell_pgid);
            }
            return 0;
        }
        
        if (i == pl->ncmds - 1) {
            if (WIFEXITED(wstatus)) {
                status = WEXITSTATUS(wstatus);
            } else if (WIFSIGNALED(wstatus)) {
                status = 128 + WTERMSIG(wstatus);
            }
        }
    }
    
    if (interactive) {
        tcsetpgrp(shell_terminal, shell_pgid);
    }
    
    return pl->negate ? !status : status;
}

/*
 * EXPANSION
 * 
 * Handles: $VAR, ${VAR}, ~, ~user
 * Returns newly allocated string.
 */
static char *expand_word(const char *word) {
    static char buf[MAX_LINE];
    char *out = buf;
    const char *p = word;
    
    while (*p && out < buf + MAX_LINE - 1) {
        if (*p == '$') {
            p++;
            char varname[256];
            int i = 0;
            
            if (*p == '{') {
                p++;
                while (*p && *p != '}' && i < 255) {
                    varname[i++] = *p++;
                }
                if (*p == '}') p++;
            } else {
                while (*p && (isalnum(*p) || *p == '_' || *p == '?' || *p == '$' || *p == '!') && i < 255) {
                    varname[i++] = *p++;
                }
            }
            
            varname[i] = '\0';
            const char *val = get_var(varname);
            if (val) {
                while (*val && out < buf + MAX_LINE - 1) {
                    *out++ = *val++;
                }
            }
        } else if (*p == '~' && (p == word || *(p-1) == ':')) {
            p++;
            if (*p == '/' || *p == '\0') {
                const char *home = getenv("HOME");
                if (home) {
                    while (*home && out < buf + MAX_LINE - 1) {
                        *out++ = *home++;
                    }
                }
            } else {
                char username[256];
                int i = 0;
                while (*p && *p != '/' && i < 255) {
                    username[i++] = *p++;
                }
                username[i] = '\0';
                struct passwd *pw = getpwnam(username);
                if (pw) {
                    const char *home = pw->pw_dir;
                    while (*home && out < buf + MAX_LINE - 1) {
                        *out++ = *home++;
                    }
                }
            }
        } else {
            *out++ = *p++;
        }
    }
    
    *out = '\0';
    return strdup(buf);
}

/*
 * TOKENIZER
 * 
 * Splits input on whitespace, handles quotes and escapes.
 * Returns NULL-terminated array.
 */
static char **tokenize(char *line, int *ntokens) {
    static char *tokens[MAX_ARGS];
    *ntokens = 0;
    
    char *p = line;
    while (*p && *ntokens < MAX_ARGS - 1) {
        while (*p && isspace(*p)) p++;
        if (!*p) break;
        
        tokens[(*ntokens)++] = p;
        
        int in_quote = 0;
        while (*p && (in_quote || !isspace(*p))) {
            if (*p == '"' || *p == '\'') {
                in_quote = !in_quote;
            }
            p++;
        }
        
        if (*p) *p++ = '\0';
    }
    
    tokens[*ntokens] = NULL;
    return tokens;
}

/*
 * PARSER - SYNTAX ANALYSIS (TOKENS → PIPELINE STRUCTURE)
 * ========================================================
 * 
 * MENTAL MODEL: Building the Execution Plan
 * 
 * Input: Array of tokens (strings)
 *   ["ls", "-la", "|", "grep", "foo", ">", "out.txt", "&"]
 * 
 * Output: Pipeline structure
 *   pipeline_t {
 *     ncmds = 2
 *     cmds[0] = {args: ["ls", "-la"], argc: 2}
 *     cmds[1] = {args: ["grep", "foo"], argc: 2, redirects: [{fd:1, file:"out.txt"}]}
 *     background = 1
 *   }
 * 
 * Parser's job:
 *   1. Recognize special tokens (|, <, >, >>, &, !)
 *   2. Split pipeline into commands
 *   3. Collect arguments for each command
 *   4. Parse redirections
 *   5. Handle variable assignments
 *   6. Expand variables and globs
 * 
 * Grammar (simplified POSIX shell):
 *   pipeline    := [!] command [| command]* [&]
 *   command     := [assignment]* word [word | redirect]*
 *   redirect    := < file | > file | >> file
 *   assignment  := VAR=value
 * 
 * Why parse?
 *   - Tokens are just strings
 *   - Need structure for execution
 *   - Identify pipes, redirections, background
 *   - Separate commands in pipeline
 * 
 * Parsing is a state machine:
 *   State 1: Looking for ! (negation)
 *   State 2: Looking for & at end (background)
 *   State 3: Parsing commands
 *     Sub-state A: Parsing variable assignments
 *     Sub-state B: Parsing arguments
 *     Sub-state C: Parsing redirections
 *   State 4: Validation
 */
static int parse_pipeline(char **tokens, int ntokens, pipeline_t *pl) {
    /* Initialize pipeline structure
     * All fields start at 0/NULL
     */
    pl->ncmds = 0;        /* Number of commands in pipeline */
    pl->negate = 0;       /* ! prefix (invert exit status) */
    pl->background = 0;   /* & suffix (run in background) */
    
    int i = 0;  /* Token index */
    
    /* STEP 1: Check for negation (!)
     * 
     * Example: ! grep foo file
     * Effect: Inverts exit status (0→1, 1→0)
     * 
     * Use case:
     *   if ! grep pattern file; then
     *     echo "pattern not found"
     *   fi
     */
    if (ntokens > 0 && strcmp(tokens[0], "!") == 0) {
        pl->negate = 1;
        i++;  /* Skip ! token */
    }
    
    /* STEP 2: Check for background (&)
     * 
     * Example: sleep 100 &
     * Effect: Shell doesn't wait for completion
     * 
     * Implementation:
     *   - & must be last token
     *   - Remove it from token list
     *   - Set background flag
     *   - execute_pipeline() will skip waitpid()
     */
    if (ntokens > 0 && strcmp(tokens[ntokens - 1], "&") == 0) {
        pl->background = 1;
        ntokens--;  /* Remove & from consideration */
    }
    
    /* STEP 3: Initialize first command
     * 
     * Pipeline can have multiple commands (separated by |)
     * Start with first command
     */
    command_t *cmd = &pl->cmds[pl->ncmds++];
    cmd->argc = 0;         /* Argument count */
    cmd->nredirects = 0;   /* Redirection count */
    
    /* STEP 4: Parse tokens into commands
     * 
     * State machine:
     *   in_assignments: Parsing VAR=value at start of command
     *   After first non-assignment: Switch to parsing arguments
     * 
     * Why track assignments separately?
     *   - VAR=value at start: Variable assignment
     *   - VAR=value after command: Regular argument
     *   Example:
     *     FOO=bar echo $FOO     # FOO set for echo only
     *     echo FOO=bar          # FOO=bar is argument to echo
     */
    int in_assignments = 1;
    
    /* Main parsing loop: Process each token */
    for (; i < ntokens; i++) {
        /* PIPE: Start new command
         * 
         * Example: ls | grep foo
         *          ^   ^
         *          cmd0 cmd1
         * 
         * Effect:
         *   - Finalize current command (NULL-terminate args)
         *   - Start new command
         *   - Reset to assignment parsing mode
         */
        if (strcmp(tokens[i], "|") == 0) {
            cmd->args[cmd->argc] = NULL;  /* NULL-terminate argv */
            cmd = &pl->cmds[pl->ncmds++]; /* Next command */
            cmd->argc = 0;
            cmd->nredirects = 0;
            in_assignments = 1;  /* New command can have assignments */
        /* INPUT REDIRECTION: < file
         * 
         * Example: grep foo < input.txt
         * Effect: stdin (FD 0) reads from input.txt
         * 
         * Implementation:
         *   - Open file with O_RDONLY
         *   - dup2(filefd, 0) in child before exec
         *   - File replaces stdin
         * 
         * Kernel operation:
         *   open("input.txt", O_RDONLY) → fd 3
         *   dup2(3, 0) → fd 0 now points to input.txt
         *   close(3)
         *   exec("grep") → grep reads from input.txt via stdin
         */
        } else if (strcmp(tokens[i], "<") == 0 && i + 1 < ntokens) {
            cmd->redirects[cmd->nredirects].fd = 0;  /* stdin */
            cmd->redirects[cmd->nredirects].file = expand_word(tokens[++i]);
            cmd->redirects[cmd->nredirects].flags = O_RDONLY;
            cmd->nredirects++;
        /* OUTPUT REDIRECTION: > file
         * 
         * Example: echo hello > output.txt
         * Effect: stdout (FD 1) writes to output.txt
         * 
         * Flags:
         *   O_WRONLY: Write-only access
         *   O_CREAT: Create file if doesn't exist
         *   O_TRUNC: Truncate file to 0 bytes (overwrite)
         * 
         * Mode: 0644 (rw-r--r--)
         *   Owner: read+write
         *   Group: read
         *   Other: read
         * 
         * Kernel operation:
         *   open("output.txt", O_WRONLY|O_CREAT|O_TRUNC, 0644) → fd 3
         *   dup2(3, 1) → fd 1 now points to output.txt
         *   close(3)
         *   exec("echo") → echo writes to output.txt via stdout
         */
        } else if (strcmp(tokens[i], ">") == 0 && i + 1 < ntokens) {
            cmd->redirects[cmd->nredirects].fd = 1;  /* stdout */
            cmd->redirects[cmd->nredirects].file = expand_word(tokens[++i]);
            cmd->redirects[cmd->nredirects].flags = O_WRONLY | O_CREAT | O_TRUNC;
            cmd->redirects[cmd->nredirects].mode = 0644;
            cmd->nredirects++;
        /* APPEND REDIRECTION: >> file
         * 
         * Example: echo hello >> output.txt
         * Effect: stdout appends to output.txt (doesn't overwrite)
         * 
         * Difference from >:
         *   >  : O_TRUNC (truncate to 0, overwrite)
         *   >> : O_APPEND (seek to end, append)
         * 
         * O_APPEND is atomic:
         *   - Kernel seeks to end before each write()
         *   - Multiple processes can append safely
         *   - No race condition (kernel handles locking)
         * 
         * Use case:
         *   while true; do
         *     echo "$(date)" >> log.txt  # Safe concurrent logging
         *   done &
         */
        } else if (strcmp(tokens[i], ">>") == 0 && i + 1 < ntokens) {
            cmd->redirects[cmd->nredirects].fd = 1;  /* stdout */
            cmd->redirects[cmd->nredirects].file = expand_word(tokens[++i]);
            cmd->redirects[cmd->nredirects].flags = O_WRONLY | O_CREAT | O_APPEND;
            cmd->redirects[cmd->nredirects].mode = 0644;
            cmd->nredirects++;
        } else {
            /* REGULAR TOKEN: Variable assignment or argument
             * 
             * Two cases:
             *   1. VAR=value at start of command → Variable assignment
             *   2. Everything else → Command argument
             */
            
            /* VARIABLE ASSIGNMENT: VAR=value
             * 
             * Example: FOO=bar echo $FOO
             * Effect: Sets FOO for this command only
             * 
             * in_assignments flag:
             *   - True at start of command
             *   - False after first non-assignment
             * 
             * Why?
             *   FOO=bar BAZ=qux echo $FOO  # FOO and BAZ are assignments
             *   echo FOO=bar               # FOO=bar is argument
             */
            if (in_assignments && strchr(tokens[i], '=')) {
                char *eq = strchr(tokens[i], '=');
                *eq = '\0';  /* Split at = */
                set_var(tokens[i], eq + 1, 0);  /* exported=0 (local) */
                *eq = '=';   /* Restore original token */
            } else {
                /* COMMAND ARGUMENT
                 * 
                 * Once we see non-assignment, all remaining tokens are arguments
                 */
                in_assignments = 0;
                
                /* EXPANSION: $VAR, ~, globs
                 * 
                 * expand_word() handles:
                 *   - $VAR → value of VAR
                 *   - ${VAR} → value of VAR
                 *   - ~ → $HOME
                 *   - ~user → /home/user
                 * 
                 * Example:
                 *   Input:  "$HOME/file.txt"
                 *   Output: "/home/user/file.txt"
                 */
                char *expanded = expand_word(tokens[i]);
                
                /* GLOB EXPANSION: *, ?, [...]
                 * 
                 * glob() - library function using getdents64() syscall
                 * 
                 * Example:
                 *   Input:  "*.txt"
                 *   Output: ["a.txt", "b.txt", "c.txt"]
                 * 
                 * Flags:
                 *   GLOB_NOCHECK: If no match, return pattern itself
                 *   (Without this, "*.txt" with no matches → error)
                 * 
                 * Implementation:
                 *   1. glob() reads directory with getdents64()
                 *   2. Matches each entry against pattern
                 *   3. Sorts results lexicographically
                 *   4. Returns array of matched paths
                 * 
                 * Why glob in shell, not in program?
                 *   - Shell expands before exec
                 *   - Program sees expanded arguments
                 *   - Example: ls *.txt
                 *     Shell: exec("ls", ["ls", "a.txt", "b.txt"])
                 *     ls sees: argv = ["ls", "a.txt", "b.txt"]
                 *     ls doesn't know about glob!
                 */
                if (strchr(expanded, '*') || strchr(expanded, '?')) {
                    glob_t globbuf;
                    if (glob(expanded, GLOB_NOCHECK, NULL, &globbuf) == 0) {
                        /* Add all matched files as separate arguments */
                        for (size_t j = 0; j < globbuf.gl_pathc && cmd->argc < MAX_ARGS - 1; j++) {
                            cmd->args[cmd->argc++] = strdup(globbuf.gl_pathv[j]);
                        }
                        globfree(&globbuf);  /* Free glob results */
                    }
                    free(expanded);
                } else {
                    /* No glob characters, use as-is */
                    cmd->args[cmd->argc++] = expanded;
                }
            }
        }
    }
    
    /* STEP 5: Finalize last command
     * 
     * NULL-terminate argv array
     * execv() expects NULL-terminated array:
     *   argv = ["ls", "-la", NULL]
     * 
     * Why NULL?
     *   - exec() doesn't take argc
     *   - Needs to know where array ends
     *   - Scans until NULL
     */
    cmd->args[cmd->argc] = NULL;
    
    /* STEP 6: Validate pipeline
     * 
     * Valid if:
     *   - At least one command
     *   - First command has arguments OR redirections
     * 
     * Examples:
     *   Valid:   "ls -la"           (has args)
     *   Valid:   "< input.txt"      (has redirect)
     *   Invalid: ""                 (empty)
     *   Invalid: "| grep foo"       (starts with pipe)
     * 
     * Returns: 1 if valid, 0 if invalid
     */
    return pl->ncmds > 0 && (pl->cmds[0].argc > 0 || pl->cmds[0].nredirects > 0);
}

/*
 * PARSING EXAMPLES - MENTAL MODELS
 * =================================
 * 
 * Example 1: Simple command
 *   Input:  "ls -la"
 *   Tokens: ["ls", "-la"]
 *   Result:
 *     pipeline_t {
 *       ncmds = 1
 *       cmds[0] = {args: ["ls", "-la", NULL], argc: 2}
 *       background = 0
 *     }
 * 
 * Example 2: Pipeline
 *   Input:  "ls | grep foo"
 *   Tokens: ["ls", "|", "grep", "foo"]
 *   Result:
 *     pipeline_t {
 *       ncmds = 2
 *       cmds[0] = {args: ["ls", NULL], argc: 1}
 *       cmds[1] = {args: ["grep", "foo", NULL], argc: 2}
 *     }
 * 
 * Example 3: Redirection
 *   Input:  "grep foo < in.txt > out.txt"
 *   Tokens: ["grep", "foo", "<", "in.txt", ">", "out.txt"]
 *   Result:
 *     pipeline_t {
 *       ncmds = 1
 *       cmds[0] = {
 *         args: ["grep", "foo", NULL]
 *         argc: 2
 *         redirects: [
 *           {fd: 0, file: "in.txt", flags: O_RDONLY},
 *           {fd: 1, file: "out.txt", flags: O_WRONLY|O_CREAT|O_TRUNC}
 *         ]
 *         nredirects: 2
 *       }
 *     }
 * 
 * Example 4: Background with variable
 *   Input:  "FOO=bar echo $FOO &"
 *   Tokens: ["FOO=bar", "echo", "$FOO", "&"]
 *   Result:
 *     - set_var("FOO", "bar", 0) called
 *     - $FOO expanded to "bar"
 *     pipeline_t {
 *       ncmds = 1
 *       cmds[0] = {args: ["echo", "bar", NULL], argc: 2}
 *       background = 1
 *     }
 * 
 * Example 5: Glob expansion
 *   Input:  "ls *.txt"
 *   Tokens: ["ls", "*.txt"]
 *   Filesystem: [a.txt, b.txt, c.txt]
 *   Result:
 *     pipeline_t {
 *       ncmds = 1
 *       cmds[0] = {args: ["ls", "a.txt", "b.txt", "c.txt", NULL], argc: 4}
 *     }
 * 
 * Example 6: Complex pipeline
 *   Input:  "! cat file | grep -v foo | wc -l > count.txt &"
 *   Result:
 *     pipeline_t {
 *       negate = 1
 *       ncmds = 3
 *       cmds[0] = {args: ["cat", "file", NULL]}
 *       cmds[1] = {args: ["grep", "-v", "foo", NULL]}
 *       cmds[2] = {
 *         args: ["wc", "-l", NULL]
 *         redirects: [{fd: 1, file: "count.txt", ...}]
 *       }
 *       background = 1
 *     }
 */

/*
 * SHELL INITIALIZATION - SETTING UP JOB CONTROL ENVIRONMENT
 * 
 * This function determines if shell is interactive and sets up job control.
 * 
 * Interactive shell: Has controlling terminal (stdin is TTY)
 *   - User typing commands at prompt
 *   - Needs job control (^C, ^Z, fg, bg)
 *   - Must manage process groups
 * 
 * Non-interactive shell: No terminal (script, pipe)
 *   - Reading from file or pipe
 *   - No job control needed
 *   - Simpler execution model
 */
static void init_shell(void) {
    /* shell_terminal: FD for controlling terminal
     * STDIN_FILENO = 0 (standard input)
     * We assume stdin is the controlling terminal
     */
    shell_terminal = STDIN_FILENO;
    
    /* isatty(fd) - library function wrapping ioctl()
     * ----------------
     * Implementation:
     *   isatty(fd) → tcgetattr(fd, &termios) → ioctl(fd, TCGETS, &termios)
     * 
     * Kernel operation:
     *   1. Lookup fd in process's fd table
     *   2. Check if file->f_op points to tty_fops (terminal operations)
     *   3. If TTY: Return terminal attributes (success)
     *   4. If not TTY: Return ENOTTY error
     * 
     * Returns: 1 if fd is terminal, 0 otherwise
     * 
     * Why check?
     *   - Interactive: stdin is terminal → enable job control
     *   - Script: stdin is file → disable job control
     *   - Pipe: stdin is pipe → disable job control
     */
    interactive = isatty(shell_terminal);
    
    if (interactive) {
        /* STEP 1: Put shell in its own process group
         * 
         * Why?
         *   - Isolate shell from job signals (^C, ^Z)
         *   - Shell must survive when jobs die
         *   - Shell becomes process group leader
         */
        shell_pgid = getpid();  /* Use shell's PID as PGID */
        
        /* setpgid(pid, pgid) - syscall: setpgid()
         * When pid == pgid: Makes process a group leader
         * 
         * Kernel operation:
         *   task->signal->__pgrp = pgid;
         *   Updates process group membership
         */
        if (setpgid(shell_pgid, shell_pgid) < 0) {
            perror("setpgid");
            exit(1);
        }
        
        /* STEP 2: Take control of terminal
         * 
         * tcsetpgrp(fd, pgid) - syscall: ioctl(fd, TIOCSPGRP, &pgid)
         * 
         * Kernel operation:
         *   tty->pgrp = pgid;
         *   Sets foreground process group
         * 
         * Effect:
         *   - Terminal input goes to this group
         *   - Terminal signals (^C, ^Z) go to this group
         *   - Shell is now foreground (will change when launching jobs)
         */
        tcsetpgrp(shell_terminal, shell_pgid);
        
        /* STEP 3: Save terminal attributes
         * 
         * tcgetattr(fd, &termios) - syscall: ioctl(fd, TCGETS, &termios)
         * 
         * Reads current terminal settings:
         *   - Input modes (ICRNL, IXON, etc.)
         *   - Output modes (OPOST, etc.)
         *   - Control modes (CSIZE, PARENB, etc.)
         *   - Local modes (ICANON, ECHO, ISIG, etc.)
         *   - Control characters (^C, ^Z, ^D, etc.)
         * 
         * Why save?
         *   - Child processes may change terminal settings
         *   - Shell must restore settings after job stops/exits
         *   - Ensures consistent terminal behavior
         */
        tcgetattr(shell_terminal, &shell_tmodes);
        
        /* STEP 4: Setup signal handlers
         * 
         * Ignore job control signals (SIGINT, SIGTSTP, etc.)
         * Handle SIGCHLD to reap background jobs
         */
        init_signals();
    }
}

/*
 * MAIN REPL (Read-Eval-Print Loop)
 * =================================
 * 
 * MENTAL MODEL: The Shell as a Process Manager
 * 
 * Shell's job:
 *   1. Read command from user
 *   2. Fork child process(es)
 *   3. Child execs the command
 *   4. Parent waits for child
 *   5. Repeat
 * 
 * Key insight: Shell is just a loop that creates other processes!
 * 
 * Process lifecycle in shell:
 *   User types: "ls -la"
 *     ↓
 *   Shell reads: "ls -la"
 *     ↓
 *   Shell forks: Creates child process
 *     ↓
 *   Child execs: Becomes ls program
 *     ↓
 *   Parent waits: Blocks until ls finishes
 *     ↓
 *   Child exits: Returns status to parent
 *     ↓
 *   Parent reaps: Collects exit status
 *     ↓
 *   Shell loops: Shows prompt again
 * 
 * Why fork/exec pattern?
 *   - Fork: Creates copy of shell
 *   - Exec: Replaces copy with new program
 *   - Parent: Original shell survives
 *   - Child: Becomes the command
 * 
 * Alternative (doesn't work):
 *   - Shell calls exec("ls") directly
 *   - Shell process becomes ls
 *   - ls exits
 *   - Shell is gone!
 *   - User loses terminal session
 * 
 * The REPL loop structure:
 *   while (1) {              // Loop
 *     print_prompt();        // Print
 *     read_input();          // Read
 *     parse_input();         // Eval (part 1)
 *     execute_command();     // Eval (part 2)
 *   }
 */
int main(void) {
    /* Line buffer for user input
     * Stack allocation: 4KB buffer
     * Could overflow with very long lines (use getline() for production)
     */
    char line[MAX_LINE];
    
    /* Initialize shell: Set up job control if interactive
     * - Checks if stdin is TTY
     * - Creates process group for shell
     * - Takes control of terminal
     * - Sets up signal handlers
     */
    init_shell();
    
    /* REPL: Infinite loop until EOF or exit command
     * 
     * Loop invariant:
     *   - Shell is in foreground (has terminal)
     *   - All jobs are either background or completed
     *   - Shell's signal handlers are installed
     */
    while (1) {
        /* STEP 1: PRINT PROMPT (if interactive)
         * 
         * Interactive: Show prompt to user
         * Non-interactive: No prompt (reading from file/pipe)
         */
        if (interactive) {
            printf("$ ");  /* Simple prompt (could be customized with PS1) */
            
            /* fflush(stdout) - library function
             * 
             * Why needed?
             *   - stdout is line-buffered by default
             *   - Prompt has no newline, so it stays in buffer
             *   - fflush() forces immediate write to terminal
             *   - Without it, prompt appears after user types!
             * 
             * Implementation:
             *   - Calls write() syscall to flush buffer
             *   - write(STDOUT_FILENO, buffer, count)
             */
            fflush(stdout);
        }
        
        /* STEP 2: READ INPUT LINE
         * 
         * fgets(buf, size, stream) - library function
         * 
         * Behavior:
         *   - Reads up to size-1 characters from stream
         *   - Stops at newline or EOF
         *   - Null-terminates the string
         *   - Includes newline in buffer (if present)
         * 
         * Returns:
         *   - buf on success
         *   - NULL on EOF or error
         * 
         * Underlying syscall:
         *   read(STDIN_FILENO, buffer, count)
         * 
         * Terminal canonical mode:
         *   - Kernel buffers input until newline
         *   - User can edit with backspace, ^U, ^W
         *   - read() blocks until user presses Enter
         *   - Kernel returns entire line at once
         */
        if (!fgets(line, sizeof(line), stdin)) {
            /* EOF reached (^D pressed or input closed)
             * 
             * Interactive: User pressed ^D (VEOF character)
             *   - Terminal driver returns 0 bytes to read()
             *   - fgets() returns NULL
             *   - Shell should exit gracefully
             * 
             * Non-interactive: End of file/pipe
             *   - No more input to read
             *   - Shell exits
             */
            if (interactive) printf("\n");  /* Clean exit with newline */
            break;  /* Exit REPL loop */
        }
        printf("TTT");
        
        /* STEP 3: STRIP NEWLINE
         * 
         * strcspn(str, reject) - library function
         * Returns: Index of first character in str that matches any in reject
         * 
         * line[strcspn(line, "\n")] = '\0';
         * Effect: Replaces newline with null terminator
         * 
         * Why?
         *   - fgets() includes newline in buffer
         *   - We don't want newline in command arguments
         *   - Replace it with null terminator
         */
        line[strcspn(line, "\n")] = '\0';
        
        /* Skip empty lines
         * User just pressed Enter without typing anything
         */
        if (line[0] == '\0') continue;
        
        /* STEP 4: TOKENIZE (Lexical Analysis)
         * 
         * Splits input into words (tokens)
         * Handles:
         *   - Whitespace separation
         *   - Quote handling ('single', "double")
         *   - Escape sequences (\)
         * 
         * Example:
         *   Input:  "ls -la | grep foo"
         *   Tokens: ["ls", "-la", "|", "grep", "foo"]
         */
        int ntokens;
        char **tokens = tokenize(line, &ntokens);
        
        /* No tokens (only whitespace) */
        if (ntokens == 0) continue;
        
        /* STEP 5: PARSE (Syntax Analysis)
         * 
         * Converts tokens into pipeline structure
         * Handles:
         *   - Pipes (|)
         *   - Redirections (<, >, >>)
         *   - Background (&)
         *   - Negation (!)
         *   - Variable assignments (VAR=value)
         * 
         * Example:
         *   Tokens: ["ls", "-la", "|", "grep", "foo", "&"]
         *   Pipeline:
         *     ncmds = 2
         *     cmds[0] = {args: ["ls", "-la"], argc: 2}
         *     cmds[1] = {args: ["grep", "foo"], argc: 2}
         *     background = 1
         */
        pipeline_t pl;
        if (parse_pipeline(tokens, ntokens, &pl)) {
            /* STEP 6: EXECUTE (Evaluation)
             * 
             * Core shell operation:
             *   1. Fork child processes (one per command in pipeline)
             *   2. Set up pipes between commands
             *   3. Set up redirections
             *   4. Create process group for job
             *   5. Give terminal to job (if foreground)
             *   6. exec() each command
             *   7. Wait for completion (if foreground)
             *   8. Reclaim terminal
             *   9. Return exit status
             * 
             * Returns: Exit status of last command in pipeline
             *   0 = success
             *   1-255 = failure
             *   128+N = killed by signal N
             */
            last_status = execute_pipeline(&pl);
            
            /* last_status saved for $? expansion
             * User can check: echo $?
             * Scripts use for error handling: if cmd; then ...; fi
             */
        }
        
        /* LOOP BACK TO TOP
         * 
         * At this point:
         *   - Command has executed
         *   - Foreground jobs have completed
         *   - Background jobs are running (tracked in jobs[])
         *   - Shell has reclaimed terminal
         *   - Ready for next command
         * 
         * State of the system:
         *   Shell process:
         *     - PID: 1000 (example)
         *     - PGID: 1000 (own group)
         *     - State: RUNNING
         *     - Has terminal: YES
         *   
         *   Foreground job:
         *     - Completed and reaped
         *     - No longer in process table
         *   
         *   Background jobs:
         *     - Still running (or stopped)
         *     - Tracked in jobs[] array
         *     - Will send SIGCHLD when they change state
         * 
         * Async events that may occur:
         *   - SIGCHLD arrives (background job finished)
         *     → Handler reaps zombie
         *     → Updates job table
         *     → Prints "[1] Done    command"
         *   
         *   - User presses ^C
         *     → Terminal sends SIGINT to foreground group
         *     → Shell is foreground, but ignores SIGINT
         *     → Nothing happens (shell survives)
         *   
         *   - User presses ^Z
         *     → Terminal sends SIGTSTP to foreground group
         *     → Shell ignores SIGTSTP
         *     → Nothing happens
         * 
         * Why shell survives signals:
         *   - Shell is in foreground group (has terminal)
         *   - But shell ignores job control signals
         *   - When job runs, shell gives terminal to job's group
         *   - Signals go to job, not shell
         *   - When job stops, shell reclaims terminal
         */
    }
    
    /* EXIT SHELL
     * 
     * Reached when:
     *   - EOF on stdin (^D or file end)
     *   - exit builtin called
     * 
     * Returns: Last command's exit status
     * Parent process (terminal) sees this as shell's exit code
     */
    return last_status;
}
