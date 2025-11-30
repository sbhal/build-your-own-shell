# SHELL ARCHITECTURE - STATE MACHINES & STACK LAYOUT

## STATE MACHINES IN THE SHELL

You're absolutely correct! The shell is built from multiple state machines working together:

### 1. TOKENIZER STATE MACHINE

```
States:
  - NORMAL: Reading regular characters
  - IN_SINGLE_QUOTE: Inside 'single quotes'
  - IN_DOUBLE_QUOTE: Inside "double quotes"
  - ESCAPED: After backslash \

Transitions:
  NORMAL + ' → IN_SINGLE_QUOTE
  IN_SINGLE_QUOTE + ' → NORMAL
  NORMAL + " → IN_DOUBLE_QUOTE
  IN_DOUBLE_QUOTE + " → NORMAL
  NORMAL + \ → ESCAPED
  ESCAPED + any → NORMAL
  NORMAL + whitespace → Token boundary
```

Example: `echo "hello world" 'foo bar'`
```
State flow:
  e → NORMAL
  " → IN_DOUBLE_QUOTE
  h,e,l,l,o, ,w,o,r,l,d → IN_DOUBLE_QUOTE
  " → NORMAL
  ' → IN_SINGLE_QUOTE
  f,o,o, ,b,a,r → IN_SINGLE_QUOTE
  ' → NORMAL
```

### 2. PARSER STATE MACHINE

```
States:
  - EXPECT_COMMAND: Looking for command or !
  - IN_ASSIGNMENTS: Parsing VAR=value at command start
  - IN_ARGUMENTS: Parsing command arguments
  - EXPECT_REDIRECT_FILE: After <, >, >>
  - EXPECT_PIPE: After |

Transitions:
  EXPECT_COMMAND + ! → EXPECT_COMMAND (negate flag set)
  EXPECT_COMMAND + word → IN_ASSIGNMENTS
  IN_ASSIGNMENTS + VAR=value → IN_ASSIGNMENTS
  IN_ASSIGNMENTS + word → IN_ARGUMENTS
  IN_ARGUMENTS + | → EXPECT_PIPE
  IN_ARGUMENTS + < → EXPECT_REDIRECT_FILE
  IN_ARGUMENTS + & → END (background flag set)
```

### 3. EXPANSION STATE MACHINE

```
States:
  - LITERAL: Copying characters as-is
  - IN_VAR: After $, reading variable name
  - IN_BRACE: Inside ${...}
  - IN_TILDE: After ~, reading username

Transitions:
  LITERAL + $ → IN_VAR
  IN_VAR + { → IN_BRACE
  IN_BRACE + } → LITERAL
  LITERAL + ~ → IN_TILDE
  IN_TILDE + / → LITERAL
```

### 4. REPL STATE MACHINE

```
States:
  - PROMPT: Showing prompt, waiting for input
  - READ: Reading line from stdin
  - TOKENIZE: Splitting into tokens
  - PARSE: Building pipeline structure
  - EXECUTE: Running commands
  - WAIT: Waiting for foreground job
  - REAP: Collecting exit status

Cycle:
  PROMPT → READ → TOKENIZE → PARSE → EXECUTE → WAIT → REAP → PROMPT
```

### 5. JOB CONTROL STATE MACHINE

```
Job States:
  - RUNNING: Executing
  - STOPPED: Suspended (^Z)
  - DONE: Completed

Transitions:
  RUNNING + ^Z → STOPPED
  STOPPED + fg → RUNNING
  STOPPED + bg → RUNNING (background)
  RUNNING + exit → DONE
  RUNNING + ^C → DONE
```

## PROCESS STACK LAYOUT AFTER EXEC

When execve() is called, the kernel sets up the new process stack:

```
High Address (0x7fffffffffff on x86-64)
┌─────────────────────────────────────┐
│  Environment strings                │  ← "PATH=/usr/bin:/bin\0"
│  "VAR1=value1\0"                    │     "HOME=/home/user\0"
│  "VAR2=value2\0"                    │     "SHELL=/bin/bash\0"
│  ...                                │
├─────────────────────────────────────┤
│  Argument strings                   │  ← "ls\0"
│  "arg0\0"                           │     "-la\0"
│  "arg1\0"                           │     "/home\0"
│  ...                                │
├─────────────────────────────────────┤
│  Auxiliary vector (ELF info)        │  ← AT_PHDR, AT_ENTRY, etc.
│  AT_NULL                            │
├─────────────────────────────────────┤
│  envp[n] = NULL                     │  ← NULL terminator
│  envp[n-1] = &"VARn=valuen"         │
│  ...                                │
│  envp[1] = &"VAR2=value2"           │
│  envp[0] = &"VAR1=value1"           │  ← envp array (pointers)
├─────────────────────────────────────┤
│  argv[argc] = NULL                  │  ← NULL terminator
│  argv[argc-1] = &"argN"             │
│  ...                                │
│  argv[1] = &"arg1"                  │
│  argv[0] = &"arg0"                  │  ← argv array (pointers)
├─────────────────────────────────────┤
│  argc = N                           │  ← Argument count
├─────────────────────────────────────┤  ← Stack pointer (rsp) points here
│                                     │
│  (Stack grows downward)             │
│                                     │
└─────────────────────────────────────┘
Low Address
```

### Stack Setup Details:

1. **Kernel copies strings to high addresses**
   - Environment strings first
   - Argument strings next
   - All NULL-terminated

2. **Kernel builds pointer arrays**
   - envp[] array points to environment strings
   - argv[] array points to argument strings
   - Both NULL-terminated

3. **Kernel pushes argc**
   - Single integer at top of stack
   - Stack pointer (rsp) points here

4. **_start receives control**
   ```c
   // In assembly (x86-64):
   // rsp → argc
   // rsp+8 → argv[0]
   // rsp+8+8*argc+8 → envp[0]
   
   void _start() {
       int argc = *(int*)rsp;
       char **argv = (char**)(rsp + 8);
       char **envp = argv + argc + 1;  // Skip NULL
       
       __libc_start_main(main, argc, argv, envp, ...);
   }
   ```

5. **main() signature**
   ```c
   int main(int argc, char *argv[], char *envp[])
   ```
   - argc: Passed in register (rdi on x86-64)
   - argv: Passed in register (rsi on x86-64)
   - envp: Passed in register (rdx on x86-64)

### Example: `ls -la /home`

```
execve("/bin/ls", ["ls", "-la", "/home"], ["PATH=/usr/bin", "HOME=/home/user"])

Stack after exec:
High Address
┌─────────────────────────────────────┐
│ "PATH=/usr/bin\0"                   │  ← 0x7fff0100
│ "HOME=/home/user\0"                 │  ← 0x7fff0080
├─────────────────────────────────────┤
│ "ls\0"                              │  ← 0x7fff0070
│ "-la\0"                             │  ← 0x7fff0068
│ "/home\0"                           │  ← 0x7fff0060
├─────────────────────────────────────┤
│ NULL                                │  ← envp[2]
│ 0x7fff0080                          │  ← envp[1] → "HOME=/home/user"
│ 0x7fff0100                          │  ← envp[0] → "PATH=/usr/bin"
├─────────────────────────────────────┤
│ NULL                                │  ← argv[3]
│ 0x7fff0060                          │  ← argv[2] → "/home"
│ 0x7fff0068                          │  ← argv[1] → "-la"
│ 0x7fff0070                          │  ← argv[0] → "ls"
├─────────────────────────────────────┤
│ 3                                   │  ← argc
├─────────────────────────────────────┤  ← rsp points here
```

### Why This Layout?

#### 1. **Strings at High Addresses: Growth Without Collision**

The kernel places actual string data at the highest addresses, then builds pointer arrays below them. This prevents a critical problem:

```
If strings were at LOW addresses:
┌─────────────────────┐
│ argc = 3            │  ← rsp
│ argv[0,1,2,NULL]    │  ← Growing down
│ envp[0,1,...,NULL]  │  ← Growing down
│ "ls\0"              │  ← COLLISION RISK!
│ "-la\0"             │
└─────────────────────┘

Actual layout (strings HIGH):
┌─────────────────────┐
│ "ls\0", "-la\0"     │  ← Safe at top
│ envp[...NULL]       │  ← Can grow down
│ argv[...NULL]       │  ← Can grow down  
│ argc = 3            │  ← rsp
└─────────────────────┘
```

**Why this matters:**
- If a program modifies `argv` or `envp` arrays (adding elements), they grow downward
- Strings remain untouched at high addresses
- No risk of pointer arrays overwriting string data
- Stack can grow downward for local variables without hitting strings

**Real-world example:** `setenv()` may reallocate `environ` array, adding new pointers. With strings at high addresses, this is safe.

**CRITICAL: What Happens When envp Grows?**

You're right to question this! Here's the truth:

```
Initial stack (kernel sets up):
┌─────────────────────────────────────┐
│ Env strings (HIGH, immutable)       │  ← 0x7fff0100
│ Arg strings (HIGH, immutable)       │  ← 0x7fff0080
├─────────────────────────────────────┤
│ envp[0,1,2,NULL]                    │  ← 0x7fff0040 (fixed size)
│ argv[0,1,2,NULL]                    │  ← 0x7fff0020 (fixed size)
│ argc                                │  ← 0x7fff0010
└─────────────────────────────────────┘
```

**The kernel's arrays are FIXED SIZE and IMMUTABLE:**
- The kernel allocates exactly `argc+1` slots for argv
- The kernel allocates exactly `envc+1` slots for envp
- These arrays CANNOT grow in place
- They are on the stack, which is read-only after exec

**What happens when you call setenv()?**

```c
setenv("NEW_VAR", "value", 1);

// libc does this:
1. Allocate NEW array on heap:
   char **new_environ = malloc((envc + 2) * sizeof(char*));

2. Copy old pointers:
   for (i = 0; i < envc; i++)
       new_environ[i] = environ[i];

3. Add new entry:
   new_environ[envc] = strdup("NEW_VAR=value");
   new_environ[envc+1] = NULL;

4. Update global:
   environ = new_environ;  // Now points to HEAP, not stack!
```

**After setenv(), memory looks like:**
```
Stack (original, unchanged):
┌─────────────────────────────────────┐
│ Env strings (still here)            │
│ Arg strings (still here)            │
│ envp[0,1,2,NULL] (orphaned!)        │  ← No longer used
│ argv[0,1,2,NULL] (still used)       │
│ argc                                │
└─────────────────────────────────────┘

Heap (new allocation):
┌─────────────────────────────────────┐
│ new_environ[0] → old env string     │  ← environ now points here
│ new_environ[1] → old env string     │
│ new_environ[2] → old env string     │
│ new_environ[3] → "NEW_VAR=value"    │  ← New string on heap
│ new_environ[4] = NULL               │
└─────────────────────────────────────┘
```

**Key insights:**

1. **Original arrays never grow** - they're fixed size on stack
2. **setenv() moves to heap** - allocates new array, abandons stack version
3. **argv never moves** - programs don't typically modify argv
4. **No collision possible** - growth happens on heap, not stack

**Why strings at high addresses still matters:**
- Even though arrays don't grow in place, the initial layout must be safe
- If strings were low, the kernel couldn't safely build the arrays
- The design assumes arrays might be modified (even though they're moved to heap)
- It's about the initial setup being correct

**What about argv modification?**
```c
// This is LEGAL but rare:
argv[0] = "new_name";  // Change pointer (OK)

// This is ILLEGAL:
argv[3] = "extra";     // Beyond original size (SEGFAULT)
argv[argc] = "extra";  // Overwrite NULL terminator (BREAKS SCANNING)
```

Programs can modify existing argv pointers but cannot extend the array.

**Summary:**
- Kernel's arrays are fixed size, cannot grow in place
- setenv() allocates new array on heap, doesn't modify stack
- No collision between envp and argv because neither grows in place
- Strings at high addresses is about initial setup safety, not runtime growth

#### 2. **NULL Terminators: Why exec() Doesn't Pass Counts**

**The fundamental problem:** `execve()` signature is:
```c
int execve(const char *pathname, char *const argv[], char *const envp[]);
```

Notice: **No `argc` or `envc` parameters!** The kernel doesn't know how many elements are in these arrays.

**Why not pass counts?**

1. **ABI Simplicity:** Fewer parameters = simpler calling convention
   - x86-64: Only 6 register parameters (rdi, rsi, rdx, rcx, r8, r9)
   - Adding counts would require stack parameters or more registers
   - Every architecture would need different conventions

2. **Variadic Nature:** Environment can be arbitrarily large
   - Passing count doesn't help if you need to scan anyway
   - NULL termination is self-describing

3. **Historical Compatibility:** Original Unix (1970s) used NULL termination
   - Changing this would break every program ever written
   - C strings are NULL-terminated, so it's consistent

**How the kernel handles this:**
```c
// In kernel's execve() implementation:
int count_strings(char *const arr[]) {
    int count = 0;
    while (arr[count] != NULL) count++;
    return count;
}

// Kernel must scan to find the end:
int argc = count_strings(argv);
int envc = count_strings(envp);

// Then copy strings and build stack
```

**Why NULL termination works:**
- Sentinel value: `NULL` (0x0) is never a valid pointer
- Simple scanning: `while (*p++) { ... }`
- No separate count to keep in sync
- Works with variable-length arrays

**CRITICAL CLARIFICATION: Two Different NULL Terminators**

```c
// argv is a NULL-terminated array of NULL-terminated strings!
char *argv[] = {
    "ls\0",      // String is NULL-terminated (\0)
    "-la\0",     // String is NULL-terminated (\0)
    NULL         // Array is NULL-terminated (NULL pointer)
};

// In memory:
argv[0] → "ls\0"     // String ends with \0 byte
argv[1] → "-la\0"    // String ends with \0 byte  
argv[2] = NULL       // Pointer is NULL (0x0)
```

**Two levels of termination:**
1. **String level:** Each string ends with `'\0'` (byte value 0)
2. **Array level:** The pointer array ends with `NULL` (pointer value 0x0)

**These are NOT the same:**
```c
'\0'  = 0x00        (char, 1 byte)
NULL  = 0x00000000  (pointer, 8 bytes on x86-64)
```

**Scanning example:**
```c
// Scan array of strings:
for (int i = 0; argv[i] != NULL; i++) {  // Check pointer
    // Scan individual string:
    for (int j = 0; argv[i][j] != '\0'; j++) {  // Check char
        putchar(argv[i][j]);
    }
}
```

**In C, arrays are NOT automatically NULL-terminated:**
```c
// Regular C array - NOT NULL-terminated:
int numbers[3] = {1, 2, 3};  // No NULL at end!

// String literal - IS NULL-terminated:
char *str = "hello";  // Compiler adds \0: "hello\0"

// Array of strings - YOU must add NULL:
char *args[] = {"ls", "-la", NULL};  // Must explicitly add NULL
```

**Why argv/envp are special:**
- The kernel explicitly adds the NULL terminator to these arrays
- This is part of the exec ABI contract
- Regular C arrays don't get this treatment

**Alternative approaches (not used):**

```c
// Alternative 1: Pass counts (rejected)
execve(path, argv, argc, envp, envc);
// Problem: More parameters, still need to validate

// Alternative 2: Length-prefixed (rejected)
struct array { int len; char **data; };
execve(path, &argv_array, &envp_array);
// Problem: Requires heap allocation, more complex

// Alternative 3: Sentinel struct (rejected)
struct { char **argv; char **envp; } args;
execve(path, &args);
// Problem: Indirect access, cache-unfriendly
```

#### 3. **argc on Stack: Calling Convention & ABI**

**Why argc is special:**

On x86-64, the stack layout at `_start` is:
```
rsp+0:  argc (integer)
rsp+8:  argv[0] (pointer)
rsp+16: argv[1] (pointer)
...
rsp+8*(argc+1): NULL
rsp+8*(argc+2): envp[0] (pointer)
```

**This enables `_start` to find everything:**
```asm
_start:
    ; rsp points to argc
    mov rdi, [rsp]        ; argc → rdi (1st param)
    lea rsi, [rsp+8]      ; argv → rsi (2nd param)
    
    ; Calculate envp = argv + argc + 1
    mov rdx, rdi          ; rdx = argc
    inc rdx               ; rdx = argc + 1
    lea rdx, [rsi+rdx*8]  ; envp → rdx (3rd param)
    
    call __libc_start_main
```

**Why not pass argc in a register?**
- Kernel doesn't know which register convention the program uses
- Stack is universal: every architecture has one
- Allows `_start` to be written in assembly without kernel coordination

**ABI Compatibility Across Architectures:**

```
x86-64:     rsp → argc, rsp+8 → argv[0]
ARM64:      sp → argc, sp+8 → argv[0]
RISC-V:     sp → argc, sp+8 → argv[0]
32-bit x86: esp → argc, esp+4 → argv[0]
```

All follow the same pattern: argc at stack pointer, argv immediately after.

#### 4. **envp After argv: ABI Stability**

**Why this order is mandated:**

1. **Backward Compatibility:** Programs compiled in 1980 still run today
   - Changing order would break every binary
   - ABI is a contract between kernel, libc, and programs

2. **Pointer Arithmetic:** `_start` calculates `envp = argv + argc + 1`
   - This formula is baked into every C runtime
   - Reversing order would break this calculation

3. **Optional envp:** Some programs don't use `envp`
   ```c
   int main(int argc, char *argv[]);  // Valid!
   ```
   - If `envp` were before `argv`, this wouldn't work
   - Current layout allows ignoring `envp`

**What if we changed it?**
```
Hypothetical layout (envp first):
rsp+0:  argc
rsp+8:  envc          ← Need to add this!
rsp+16: envp[0]
...
rsp+16+8*envc: argv[0]

Problems:
- Need to pass envc (extra parameter)
- Can't calculate argv without knowing envc
- Breaks all existing binaries
- No benefit over current design
```

#### 5. **Auxiliary Vector: ELF Metadata**

After `envp[NULL]`, the kernel adds an auxiliary vector:
```
AT_PHDR:  Address of program headers
AT_ENTRY: Entry point address  
AT_EXECFD: File descriptor of executable
AT_RANDOM: 16 random bytes (for ASLR)
AT_NULL:  End marker
```

**Why after envp?**
- Programs can ignore it (backward compatibility)
- Dynamic linker (ld.so) needs this info
- Extensible: can add new AT_* types without breaking old programs

**Accessing auxiliary vector:**
```c
// In _start or __libc_start_main:
ElfW(auxv_t) *auxv = (ElfW(auxv_t)*)(envp + envc + 1);
for (; auxv->a_type != AT_NULL; auxv++) {
    if (auxv->a_type == AT_RANDOM) {
        // Use random bytes for stack canary
    }
}
```

#### Summary: Why This Exact Layout?

| Design Choice | Reason | Alternative Rejected |
|--------------|--------|---------------------|
| Strings at high addresses | Prevent collision with growing arrays | Strings at low addresses (collision risk) |
| NULL termination | exec() doesn't pass counts, self-describing | Pass counts (more parameters, complexity) |
| argc on stack | Universal across architectures | argc in register (architecture-specific) |
| envp after argv | Backward compatibility, optional envp | envp before argv (breaks pointer arithmetic) |
| Auxiliary vector last | Extensible, ignorable by old programs | Mixed with envp (breaks compatibility) |

This layout is a **30+ year old ABI contract** that balances:
- Simplicity (minimal parameters to exec)
- Efficiency (pointer arithmetic, no indirection)
- Compatibility (works across all Unix systems)
- Extensibility (can add auxiliary vector entries)
- Safety (strings can't be overwritten by array growth)

### Accessing Environment:

```c
// Method 1: main() parameter
int main(int argc, char *argv[], char *envp[]) {
    for (int i = 0; envp[i]; i++) {
        printf("%s\n", envp[i]);
    }
}

// Method 2: Global environ
extern char **environ;
int main() {
    for (int i = 0; environ[i]; i++) {
        printf("%s\n", environ[i]);
    }
}

// Method 3: getenv()
int main() {
    char *path = getenv("PATH");
    printf("PATH=%s\n", path);
}
```

## STATE MACHINE COORDINATION

All state machines work together:

```
User Input: echo "hello $USER" > file.txt &

1. REPL: PROMPT → READ
   ↓
2. TOKENIZER: Splits into tokens
   ["echo", "hello $USER", ">", "file.txt", "&"]
   ↓
3. PARSER: Builds pipeline
   - Sees &: background = 1
   - Sees >: redirect stdout to file.txt
   - Sees "hello $USER": needs expansion
   ↓
4. EXPANSION: Processes $USER
   "hello $USER" → "hello john"
   ↓
5. EXECUTION: Fork/exec/wait
   - Fork child
   - Child: dup2(filefd, 1), exec("echo")
   - Parent: Don't wait (background)
   ↓
6. JOB CONTROL: Track background job
   - Add to jobs[] array
   - Print [1] 12345
   ↓
7. REPL: PROMPT (ready for next command)
```

## KEY INSIGHTS

1. **State machines enable modularity**: Each phase has clear inputs/outputs
2. **State transitions are explicit**: Easy to debug and reason about
3. **Error handling at boundaries**: Each state machine validates its input
4. **Composition**: Complex behavior emerges from simple state machines
5. **Stack layout is ABI**: Kernel and libc must agree on layout

This architecture makes the shell:
- **Maintainable**: Each state machine is independent
- **Extensible**: Add new states without breaking others
- **Debuggable**: Can trace state transitions
- **Correct**: Clear state invariants at each step
