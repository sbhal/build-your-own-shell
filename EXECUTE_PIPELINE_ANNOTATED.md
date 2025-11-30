# EXECUTE_PIPELINE - DETAILED LINE-BY-LINE ANNOTATION

## MENTAL MODEL: Pipeline Execution as Orchestration

Think of execute_pipeline as a conductor orchestrating multiple musicians (processes):
1. **Setup**: Create communication channels (pipes)
2. **Spawn**: Fork musicians (child processes)
3. **Connect**: Wire up instruments (pipe FDs)
4. **Perform**: Each musician plays (exec)
5. **Coordinate**: Conductor waits (waitpid)
6. **Cleanup**: Collect results (exit status)

## THE FUNCTION (Annotated Line-by-Line)

```c
static int execute_pipeline(pipeline_t *pl) {
    /* ENTRY POINT: Execute a parsed pipeline
     * 
     * Input: pipeline_t structure from parser
     * Output: Exit status of last command (0-255, or 128+signal)
     * 
     * This function is the HEART of the shell - where fork/exec/wait happens
     */
    
    /* EDGE CASE 1: Empty pipeline */
    if (pl->ncmds == 0) return 0;
    
    /* EDGE CASE 2: Single builtin in foreground
     * 
     * Why special case?
     *   - Builtins modify shell state (cd changes cwd, export sets vars)
     *   - If we fork, changes happen in child, not shell!
     *   - Must run in shell process itself
     * 
     * Example: cd /tmp
     *   - If forked: Child changes directory, exits
     *   - Shell still in original directory!
     *   - Solution: Run builtin directly in shell
     */
    if (pl->ncmds == 1 && is_builtin(pl->cmds[0].args[0]) && !pl->background) {
        int status = run_builtin(&pl->cmds[0]);
        return pl->negate ? !status : status;
    }
    
    /* SETUP PHASE: Allocate resources
     * 
     * pipes[i][0] = read end of pipe i
     * pipes[i][1] = write end of pipe i
     * 
     * For N commands, need N-1 pipes:
     *   cmd0 | cmd1 | cmd2
     *   pipe0   pipe1
     */
    int pipes[MAX_CMDS][2];
    pid_t pids[MAX_CMDS];
    pid_t pgid = 0;  /* Process group ID for this job */
    
    /* CREATE ALL PIPES UPFRONT
     * 
     * Why before forking?
     *   - All children need to see all pipes
     *   - Each child closes unused ends
     *   - Parent needs all FDs to close them
     * 
     * pipe(pipefd) - syscall: pipe2(pipefd, 0)
     * Kernel operation:
     *   1. Allocate pipe_inode_info (64KB circular buffer)
     *   2. Create two file descriptors
     *   3. pipefd[0] = read end (O_RDONLY)
     *   4. pipefd[1] = write end (O_WRONLY)
     * 
     * Example: ls | grep foo | wc
     *   pipes[0]: ls writes, grep reads
     *   pipes[1]: grep writes, wc reads
     */
    for (int i = 0; i < pl->ncmds - 1; i++) {
        if (pipe(pipes[i]) < 0) die("pipe");
        /* After this loop:
         * pipes[0][0], pipes[0][1] = pipe between cmd0 and cmd1
         * pipes[1][0], pipes[1][1] = pipe between cmd1 and cmd2
         * etc.
         */
    }
    
    /* FORK PHASE: Create child processes
     * 
     * Loop creates one child per command
     * Each child will exec its command
     * Parent tracks all children
     */
    for (int i = 0; i < pl->ncmds; i++) {
        /* fork() - syscall: clone() on Linux
         * 
         * Kernel operation:
         *   1. Allocate new task_struct
         *   2. Copy parent's memory (COW)
         *   3. Duplicate FD table (all pipes visible!)
         *   4. Inherit signal handlers
         *   5. Child gets new PID, same PPID
         * 
         * Returns:
         *   0 in child
         *   child's PID in parent
         *   -1 on error
         * 
         * Critical: After fork, both parent and child continue here!
         */
        pid_t pid = fork();
        if (pid < 0) die("fork");
        
        /* ============ CHILD PROCESS ============ */
        if (pid == 0) {
            /* CHILD STEP 1: Reset signal handlers
             * 
             * Why?
             *   - Child inherited SIG_IGN from shell
             *   - exec() preserves SIG_IGN (special case!)
             *   - If we don't reset, ^C won't kill child!
             * 
             * signal(sig, SIG_DFL) - library function
             * Sets signal disposition to default action
             */
            signal(SIGINT, SIG_DFL);   /* ^C: Terminate */
            signal(SIGQUIT, SIG_DFL);  /* ^\: Quit with core */
            signal(SIGTSTP, SIG_DFL);  /* ^Z: Stop */
            signal(SIGTTIN, SIG_DFL);  /* Background read */
            signal(SIGTTOU, SIG_DFL);  /* Background write */
            signal(SIGCHLD, SIG_DFL);  /* Child status change */
            
            /* CHILD STEP 2: Set process group
             * 
             * Process group protocol:
             *   - First child (i==0): Creates new group
             *   - Other children: Join first child's group
             * 
             * Why?
             *   - All commands in pipeline must be in same group
             *   - Terminal sends signals to entire group
             *   - ^C kills all commands simultaneously
             */
            if (i == 0) {
                /* First child: Create new process group
                 * 
                 * pgid = getpid() - Use my PID as group ID
                 * setpgid(0, pgid) - Put myself in new group
                 * 
                 * setpgid(0, pgid) - syscall: setpgid()
                 * Kernel: task->signal->__pgrp = pgid
                 */
                pgid = getpid();
                setpgid(0, pgid);
                
                /* Give terminal to this group (if foreground)
                 * 
                 * tcsetpgrp(fd, pgid) - syscall: ioctl(fd, TIOCSPGRP, &pgid)
                 * Kernel: tty->pgrp = pgid
                 * 
                 * Effect:
                 *   - Terminal input goes to this group
                 *   - Terminal signals (^C, ^Z) go to this group
                 *   - Shell no longer receives terminal signals
                 */
                if (interactive && !pl->background) {
                    tcsetpgrp(shell_terminal, pgid);
                }
            } else {
                /* Other children: Join first child's group
                 * 
                 * pgid was set by first child
                 * setpgid(0, pgid) - Put myself in that group
                 */
                setpgid(0, pgid);
            }
            
            /* CHILD STEP 3: Setup pipes
             * 
             * Pipe plumbing:
             *   - If not first command: stdin comes from previous pipe
             *   - If not last command: stdout goes to next pipe
             * 
             * Example: cmd0 | cmd1 | cmd2
             *   cmd0: stdout → pipes[0][1]
             *   cmd1: stdin ← pipes[0][0], stdout → pipes[1][1]
             *   cmd2: stdin ← pipes[1][0]
             */
            if (i > 0) {
                /* Not first command: Read from previous pipe
                 * 
                 * dup2(pipes[i-1][0], 0) - syscall: dup2()
                 * 
                 * Effect:
                 *   - FD 0 (stdin) now points to read end of previous pipe
                 *   - Original stdin closed
                 *   - Child reads from pipe instead of terminal
                 * 
                 * Kernel operation:
                 *   1. Close FD 0 (if open)
                 *   2. Make FD 0 point to same file as pipes[i-1][0]
                 *   3. Increment file's reference count
                 */
                dup2(pipes[i-1][0], 0);
            }
            
            if (i < pl->ncmds - 1) {
                /* Not last command: Write to next pipe
                 * 
                 * dup2(pipes[i][1], 1) - syscall: dup2()
                 * 
                 * Effect:
                 *   - FD 1 (stdout) now points to write end of current pipe
                 *   - Original stdout closed
                 *   - Child writes to pipe instead of terminal
                 */
                dup2(pipes[i][1], 1);
            }
            
            /* CHILD STEP 4: Close all pipe FDs
             * 
             * CRITICAL: Must close ALL pipe ends!
             * 
             * Why?
             *   - Child inherited all pipe FDs from parent
             *   - After dup2(), we have duplicates
             *   - If we don't close, pipe never gets EOF
             *   - Next command hangs waiting for EOF!
             * 
             * Example: ls | grep foo
             *   ls (child 0):
             *     - dup2(pipes[0][1], 1) - stdout → pipe write end
             *     - Must close pipes[0][1] (duplicate!)
             *     - Must close pipes[0][0] (unused read end)
             *   grep (child 1):
             *     - dup2(pipes[0][0], 0) - stdin ← pipe read end
             *     - Must close pipes[0][0] (duplicate!)
             *     - Must close pipes[0][1] (unused write end)
             * 
             * If ls doesn't close pipes[0][1]:
             *   - Pipe has 2 write ends open (ls's stdout + pipes[0][1])
             *   - ls exits, closes stdout
             *   - But pipes[0][1] still open!
             *   - grep's read() never returns EOF
             *   - grep hangs forever!
             */
            for (int j = 0; j < pl->ncmds - 1; j++) {
                close(pipes[j][0]);  /* Close read end */
                close(pipes[j][1]);  /* Close write end */
            }
            
            /* CHILD STEP 5: Setup redirections
             * 
             * Redirections override pipe setup
             * Example: ls | grep foo > out.txt
             *   grep's stdout was pipes[0][1]
             *   Redirection changes it to out.txt
             * 
             * setup_redirects():
             *   for each redirect:
             *     fd = open(file, flags, mode)
             *     dup2(fd, target_fd)
             *     close(fd)
             */
            setup_redirects(&pl->cmds[i]);
            
            /* CHILD STEP 6: Execute command
             * 
             * Two cases:
             *   1. Builtin: Call function, exit with status
             *   2. External: exec() to replace process image
             */
            if (is_builtin(pl->cmds[i].args[0])) {
                /* Builtin in pipeline or background
                 * 
                 * Must fork because:
                 *   - Builtin might be in pipeline
                 *   - Can't modify shell's state
                 *   - Example: echo foo | cd /tmp | pwd
                 *     cd must not change shell's directory!
                 */
                exit(run_builtin(&pl->cmds[i]));
            }
            
            /* External command: Find in PATH */
            char *path = find_in_path(pl->cmds[i].args[0]);
            if (!path) {
                fprintf(stderr, "%s: command not found\\n", pl->cmds[i].args[0]);
                exit(127);  /* Command not found exit code */
            }
            
            /* execv(path, argv) - syscall: execve(path, argv, environ)
             * 
             * Kernel operation:
             *   1. Discard current process image (text, data, stack)
             *   2. Load new ELF binary from path
             *   3. Setup new stack with argc, argv, envp
             *   4. Reset signal handlers to SIG_DFL (except SIG_IGN)
             *   5. Preserve PID, PPID, FDs (unless FD_CLOEXEC)
             *   6. Jump to entry point (_start)
             * 
             * Stack layout after exec:
             *   High Address
             *   ┌─────────────────────┐
             *   │ Environment strings │
             *   ├─────────────────────┤
             *   │ Argument strings    │
             *   ├─────────────────────┤
             *   │ envp[] array        │
             *   ├─────────────────────┤
             *   │ argv[] array        │
             *   ├─────────────────────┤
             *   │ argc                │ ← rsp
             *   └─────────────────────┘
             * 
             * CRITICAL: exec() never returns on success!
             * If we reach the next line, exec failed!
             */
            execv(path, pl->cmds[i].args);
            
            /* Only reached if exec() failed */
            perror("execv");
            exit(1);
        }
        /* ============ END CHILD PROCESS ============ */
        
        /* ============ PARENT PROCESS ============ */
        /* Parent continues here after fork()
         * 
         * Child is now running in parallel
         * Parent must:
         *   1. Track child's PID
         *   2. Set child's process group (race with child)
         *   3. Give terminal to job (if first child, foreground)
         */
        pids[i] = pid;  /* Save child's PID */
        
        if (i == 0) {
            /* First child: Setup process group
             * 
             * Race condition:
             *   - Parent and child both call setpgid()
             *   - Whoever runs first wins
             *   - Both calls succeed (idempotent)
             * 
             * Why both?
             *   - Parent needs to call tcsetpgrp() before child execs
             *   - Child might exec before parent calls setpgid()
             *   - Solution: Both call setpgid()
             */
            pgid = pid;  /* Use first child's PID as group ID */
            setpgid(pid, pgid);  /* Put child in new group */
            
            /* Give terminal to job (if foreground) */
            if (interactive && !pl->background) {
                tcsetpgrp(shell_terminal, pgid);
                /* Now terminal belongs to job
                 * Shell no longer receives ^C, ^Z
                 * Job receives terminal signals
                 */
            }
        } else {
            /* Other children: Join first child's group */
            setpgid(pid, pgid);
        }
        /* ============ END PARENT PROCESS ============ */
    }
    /* End of fork loop - all children created */
    
    /* CLEANUP PHASE: Close all pipes in parent
     * 
     * Why?
     *   - Parent inherited all pipe FDs
     *   - Children have their own copies
     *   - If parent doesn't close, pipes never get EOF
     *   - Children hang waiting for EOF!
     * 
     * Example: ls | grep foo
     *   Parent has: pipes[0][0], pipes[0][1]
     *   ls has: pipes[0][1] (as stdout)
     *   grep has: pipes[0][0] (as stdin)
     *   
     *   If parent doesn't close pipes[0][1]:
     *     - Pipe has 2 write ends (ls + parent)
     *     - ls exits, closes its write end
     *     - But parent's write end still open!
     *     - grep's read() never returns EOF
     *     - grep hangs!
     */
    for (int i = 0; i < pl->ncmds - 1; i++) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }
    
    /* Save last background PID for $! expansion */
    last_bg_pid = pgid;
    
    /* BACKGROUND JOB: Don't wait, return immediately */
    if (pl->background) {
        add_job(pgid, "background job", 1);
        /* Job added to jobs[] array
         * SIGCHLD handler will reap when it exits
         * Shell continues immediately
         */
        return 0;
    }
    
    /* WAIT PHASE: Wait for foreground job to complete
     * 
     * Must wait for ALL children in pipeline
     * Last child's exit status is pipeline's status
     */
    int status = 0;
    for (int i = 0; i < pl->ncmds; i++) {
        int wstatus;
        
        /* waitpid(pid, &status, options) - syscall: wait4()
         * 
         * Blocks until child changes state
         * WUNTRACED: Report stopped children (^Z)
         * 
         * Returns:
         *   - PID of child that changed state
         *   - Status in wstatus
         * 
         * Status encoding (wstatus):
         *   - If exited: (status << 8) | 0
         *   - If signaled: signal | 0x80
         *   - If stopped: (signal << 8) | 0x7f
         */
        waitpid(pids[i], &wstatus, WUNTRACED);
        
        /* Check if job was stopped (^Z) */
        if (WIFSTOPPED(wstatus)) {
            /* Job stopped: Add to job table
             * 
             * User pressed ^Z:
             *   - Terminal sent SIGTSTP to job's group
             *   - All processes in group stopped
             *   - Shell receives SIGCHLD
             *   - waitpid() returns with WIFSTOPPED
             */
            add_job(pgid, "stopped job", 0);
            printf("[%d] Stopped\\n", njobs);
            
            /* Reclaim terminal */
            if (interactive) {
                tcsetpgrp(shell_terminal, shell_pgid);
                /* Shell is foreground again
                 * Shell can receive input
                 * Job is stopped, not receiving signals
                 */
            }
            return 0;  /* Don't wait for other children */
        }
        
        /* Last command's status is pipeline's status */
        if (i == pl->ncmds - 1) {
            if (WIFEXITED(wstatus)) {
                /* Normal exit: Extract exit code
                 * WEXITSTATUS(s) = (s >> 8) & 0xff
                 */
                status = WEXITSTATUS(wstatus);
            } else if (WIFSIGNALED(wstatus)) {
                /* Killed by signal: Return 128 + signal number
                 * Convention: 128+N means killed by signal N
                 * Example: SIGINT (2) → 130
                 */
                status = 128 + WTERMSIG(wstatus);
            }
        }
    }
    
    /* RECLAIM TERMINAL: Give terminal back to shell */
    if (interactive) {
        tcsetpgrp(shell_terminal, shell_pgid);
        /* Shell is foreground again
         * Shell receives terminal input
         * Shell receives ^C, ^Z (but ignores them)
         */
    }
    
    /* Return exit status (possibly negated) */
    return pl->negate ? !status : status;
}
```

## EXECUTION FLOW DIAGRAM

```
execute_pipeline("ls | grep foo")
    ↓
1. Create pipes
    pipes[0][0] = read end
    pipes[0][1] = write end
    ↓
2. Fork child 0 (ls)
    ↓
    Child 0:                          Parent:
    - Reset signals                   - Save PID
    - setpgid(0, getpid())           - setpgid(child0, child0)
    - tcsetpgrp(tty, getpid())       - tcsetpgrp(tty, child0)
    - dup2(pipes[0][1], 1)           - Continue
    - close all pipes
    - exec("ls")
    ↓
3. Fork child 1 (grep)
    ↓
    Child 1:                          Parent:
    - Reset signals                   - Save PID
    - setpgid(0, child0_pgid)        - setpgid(child1, child0_pgid)
    - dup2(pipes[0][0], 0)           - Continue
    - close all pipes
    - exec("grep")
    ↓
4. Parent closes all pipes
    close(pipes[0][0])
    close(pipes[0][1])
    ↓
5. Parent waits
    waitpid(child0, ...)  → ls exits
    waitpid(child1, ...)  → grep exits
    ↓
6. Parent reclaims terminal
    tcsetpgrp(tty, shell_pgid)
    ↓
7. Return exit status
```

## KEY INSIGHTS

1. **Pipes before forks**: All children need to see all pipes
2. **Close unused ends**: Critical to avoid deadlock
3. **Both parent and child call setpgid()**: Race condition handling
4. **Terminal ownership**: Passed to job, then reclaimed
5. **Signal reset**: Children must reset SIG_IGN to SIG_DFL
6. **Builtin special case**: Must run in shell for state changes
7. **Last command's status**: Pipeline status is last command's status
8. **Background jobs**: Don't wait, track in jobs[] array

This function is the culmination of all the concepts: fork, exec, wait, pipes, FDs, process groups, signals, and terminal control!
