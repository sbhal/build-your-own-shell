# Shell Implementation Guide

## Overview

This is a comprehensive Unix shell implementation covering all 5 stages of the workshop.

## File: mysh_complete.c

A production-quality shell implementation with extensive mental model documentation.

### Key Features Implemented

#### Stage 1: fork/exec/wait
- Basic REPL loop
- Command execution via fork/exec/wait
- PATH search
- Builtin commands (cd, exec)
- Exit status tracking
- ! (negation) operator
- Sequential lists (;, &&, ||)

#### Stage 2: Files and Pipes
- Pipe operator (|)
- Input redirection (<)
- Output redirection (>, >>)
- File descriptor management
- CLOEXEC handling
- Multi-command pipelines

#### Stage 3: Job Control and Signals
- Process groups (setpgid)
- Terminal control (tcsetpgrp)
- Background jobs (&)
- Signal handling (SIGINT, SIGTSTP, SIGCHLD, SIGCONT)
- Job management (fg, bg, jobs)
- Stopped job tracking

#### Stage 4: Variables and Expansion
- Variable assignment (VAR=value)
- Variable expansion ($VAR, ${VAR})
- Special parameters ($?, $$, $!)
- Export builtin
- Tilde expansion (~, ~user)
- Glob expansion (*, ?)
- Environment variable inheritance

#### Stage 5: Interactivity
- Interactive mode detection (isatty)
- Prompt display
- Job control integration
- Signal handling for interactive use

## Building

```bash
make
```

## Testing

Run all tests:
```bash
make test
```

Run specific stage:
```bash
make test-stage STAGE=stage_1
```

## Architecture

### Mental Models

#### 1. Process Model
```
Shell Process
    |
    fork() --> Child Process (copy of shell)
    |              |
    |              exec() --> New Program
    |              |
    wait() <-------+
```

#### 2. Pipeline Model
```
cmd1 | cmd2 | cmd3

cmd1 --[pipe1_write]--> [pipe1_read]-- cmd2 --[pipe2_write]--> [pipe2_read]-- cmd3
```

#### 3. Job Control Model
```
Terminal
    |
    tcsetpgrp() --> Foreground Process Group
                        |
                        +-- Process 1
                        +-- Process 2
                        +-- Process 3

Background Jobs (no terminal access)
    |
    +-- Job 1 (RUNNING)
    +-- Job 2 (STOPPED)
```

#### 4. Signal Flow
```
User presses ^C
    |
    Terminal driver
    |
    SIGINT --> Foreground Process Group
                   |
                   +-- All processes in group receive signal
```

### Critical Implementation Details

#### Pipe Management
- **Always close unused pipe ends** - Most common bug causing hangs
- Close in both parent and child
- Close immediately after dup2()

#### Process Groups
- First child in pipeline creates new PGRP
- Other children join that PGRP
- Both parent and child call setpgid() to avoid races

#### Terminal Control
- Only foreground job can access terminal
- Shell must reclaim terminal after job stops/exits
- tcsetpgrp() must be called in both parent and child

#### Signal Handling
- Shell ignores SIGINT/SIGTSTP (they go to foreground job)
- Children reset signals to SIG_DFL before exec
- SIGCHLD handler reaps background jobs

#### Variable Expansion
- Happens before command execution
- $VAR expands to variable value
- Glob expansion happens after variable expansion
- Tilde expansion happens first

### Common Pitfalls

1. **Forgetting to close pipe FDs** → Processes hang waiting for EOF
2. **Not setting process group** → Job control doesn't work
3. **Not resetting signals in child** → Children inherit shell's handlers
4. **Race between setpgid and tcsetpgrp** → Call both in parent and child
5. **Not reclaiming terminal** → Shell loses control after job stops

### Testing Strategy

Each stage builds on previous stages:
- Stage 1: Basic execution
- Stage 2: Add pipes and redirections
- Stage 3: Add job control
- Stage 4: Add expansions
- Stage 5: Polish for interactive use

Run tests incrementally to catch issues early.

## Code Structure

```
main()
    ↓
init_shell() - Setup job control
    ↓
REPL loop:
    ↓
tokenize() - Split input into tokens
    ↓
parse_pipeline() - Build pipeline structure
    ↓
execute_pipeline() - Fork/exec/wait
    ↓
    ├─ setup_redirects() - Open files, dup2
    ├─ find_in_path() - Search PATH
    ├─ expand_word() - Variable/tilde/glob expansion
    └─ run_builtin() - Execute builtin commands
```

## Performance Considerations

- **Fork overhead**: Each command forks a new process (~1ms)
- **Pipe buffering**: Kernel buffers pipe data (typically 64KB)
- **Signal delivery**: Asynchronous, may interrupt syscalls
- **PATH search**: Cached by kernel (dentry cache)

## Security Considerations

- **CLOEXEC**: Prevent FD leaks to children
- **Signal handling**: Avoid race conditions
- **PATH injection**: Validate command paths
- **Environment**: Sanitize exported variables

## Extensions

Possible enhancements:
- Command history (readline integration)
- Tab completion
- Syntax highlighting
- Command substitution $(...)
- Heredocs (<<EOF)
- Arithmetic expansion $((expr))
- Arrays
- Functions
- Aliases
- Configuration file (~/.myshrc)

## References

- APUE Chapter 8 (Process Control)
- APUE Chapter 9 (Process Relationships)
- APUE Chapter 10 (Signals)
- POSIX Shell Specification
- GNU libc manual: Implementing a Job Control Shell
