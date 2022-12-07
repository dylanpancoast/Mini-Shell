#include "sh61.hh"
#include <cstring>
#include <cerrno>
#include <vector>
#include <sys/stat.h>
#include <sys/wait.h>

// For the love of God
#undef exit
#define exit __DO_NOT_CALL_EXIT__READ_PROBLEM_SET_DESCRIPTION__


// struct command
//    Data structure describing a command. Add your own stuff.

struct command {
    std::vector<std::string> args;
    pid_t pid = -1;               // process ID running this command, -1 if none
    int status;

    command();
    ~command();

    // Pipes
    int pfd[2];

    // Redirects
    bool in = false;
    bool out = false;
    bool err = false;
    std::string inpath;
    std::string outpath;
    std::string errpath;

    // Vars for all commands
    command* next = nullptr;
    command* prev = nullptr;
    int link = TYPE_SEQUENCE;

    void run();
};


// command::command()
//    This constructor function initializes a `command` structure. You may
//    add stuff to it as you grow the command structure.

command::command() {
}


// command::~command()
//    This destructor function is called to delete a command.

command::~command() {
    delete next;
}


// Helper function for handling failed syscalls
void error_msg() {
    fprintf(stderr, "%m\n");
    _exit(EXIT_FAILURE);
}

void redir(std::string path, int flags, int data_stream) {
    int n = open(path.c_str(), flags, 0666);
    if (n == -1) {
        error_msg();
    }
    if (dup2(n, data_stream) == -1) {
        error_msg();
    }
    if (close(n) == -1) {
        error_msg();
    }
}

void connect_pipes(command* c, int pfd_end, int data_stream) {
    if (dup2(c->pfd[pfd_end], data_stream) == -1) {
        error_msg();
    }
    if (close(c->pfd[pfd_end]) == -1) {
        error_msg();
    }
}


// COMMAND EXECUTION

// command::run()
//    Creates a single child process running the command in `this`, and
//    sets `this->pid` to the pid of the child process.
//
//    If a child process cannot be created, this function should call
//    `_exit(EXIT_FAILURE)` (that is, `_exit(1)`) to exit the containing
//    shell or subshell. If this function returns to its caller,
//    `this->pid > 0` must always hold.
//
//    Note that this function must return to its caller *only* in the parent
//    process. The code that runs in the child process must `execvp` and/or
//    `_exit`.
//
//    PART 1: Fork a child process and run the command using `execvp`.
//       This will require creating a vector of `char*` arguments using
//       `this->args[N].c_str()`. Note that the last element of the vector
//       must be a `nullptr`.
//    PART 4: Set up a pipeline if appropriate. This may require creating a
//       new pipe (`pipe` system call), and/or replacing the child process's
//       standard input/output with parts of the pipe (`dup2` and `close`).
//       Draw pictures!
//    PART 7: Handle redirections.

void command::run() {
    assert(this->pid == -1);
    assert(this->args.size() > 0);

    // Create a pipe if needed
    if (this->link == TYPE_PIPE) {
        // Parent is piped to something
        if (pipe(this->pfd) == -1) {
            error_msg();
        }
    }

    int m = 0;
    if (this->args[0] == "cd") {
        // Handle redirects if any
        // Change directory
        m = chdir(this->args[1].c_str());
    }

    // Fork current process 
    pid_t child_pid = fork();
    if (child_pid == -1) {
        error_msg();
    }
    if (child_pid == 0) {
        // Child process executes this code
        // Connect pipes if any
        if (this->args[0] == "cd") {
            if (m == -1) {
                _exit(EXIT_FAILURE);
            } else {
                _exit(EXIT_SUCCESS);
            }
        }
        
        if (this->prev && this->prev->link == TYPE_PIPE) {
            // Something is piped to this
            connect_pipes(this->prev, 0, STDIN_FILENO);
        }
        if (this->link == TYPE_PIPE) {
            // This is piped to something
            connect_pipes(this, 1, STDOUT_FILENO);
            if (close(this->pfd[0]) == -1) {
                error_msg();
            }
        }
        
        // Handle redirects if any
        if (this->in) {
            redir(this->inpath, O_RDONLY, STDIN_FILENO);
        }
        if (this->out) {
            redir(this->outpath, O_CREAT | O_WRONLY, STDOUT_FILENO);
        }
        if (this->err) {
            redir(this->errpath, O_WRONLY | O_CREAT | O_TRUNC, STDERR_FILENO);
        }

        // Create an array of arguments from user input that ends in a nullptr
        size_t n = this->args.size();
        const char* str[n + 1];
        for (size_t i = 0; i < n; ++i) {
            str[i] = this->args[i].c_str();
        }
        str[n] = nullptr;

        // Replaces the current process image
        execvp(str[0], (char**) str);
        error_msg();
    } 

    // Parent process executes this code
    this->pid = child_pid;

    if (this->prev && this->prev->link == TYPE_PIPE) {
        // Something is piped to parent
        if (close(this->prev->pfd[0]) == -1) {
            error_msg();
        }
    }
    if (this->link == TYPE_PIPE) {
        // Parent is piped to something
        if (close(this->pfd[1]) == -1) {
            error_msg();
        }
    }
}


// run_list(c)
//    Run the command *list* starting at `c`. Initially this just calls
//    `c->run()` and `waitpid`; you’ll extend it to handle command lists,
//    conditionals, and pipelines.
//
//    It is possible, and not too ugly, to handle lists, conditionals,
//    *and* pipelines entirely within `run_list`, but many students choose
//    to introduce `run_conditional` and `run_pipeline` functions that
//    are called by `run_list`. It’s up to you.
//
//    PART 1: Start the single command `c` with `c->run()`,
//        and wait for it to finish using `waitpid`.
//    The remaining parts may require that you change `struct command`
//    (e.g., to track whether a command is in the background)
//    and write code in `command::run` (or in helper functions).
//    PART 2: Introduce a loop to run a list of commands, waiting for each
//       to finish before going on to the next.
//    PART 3: Change the loop to handle conditional chains.
//    PART 4: Change the loop to handle pipelines. Start all processes in
//       the pipeline in parallel. The status of a pipeline is the status of
//       its LAST command.
//    PART 5: Change the loop to handle background conditional chains.
//       This may require adding another call to `fork()`!

void run_pipeline(command* &c) {
    // Run the pipeline
    while (c && c->link == TYPE_PIPE) {
        // Run all but the last command in a pipeline
        c->run();
        c = c->next;
    }
    // Run last command in a pipeline
    c->run();

    // Wait for output of final command in this pipeline
    if (waitpid(c->pid, &c->status, 0) == -1) {
        error_msg();
    }
    return;
}

void run_conditional(command* &c) {
    // Run all processes
    while (c) {
        // Run current pipeline
        run_pipeline(c);

        // Apply logic given pipeline output
        if (WIFEXITED(c->status) != 0) {
            // Pipeline exited normally
            if (c && WEXITSTATUS(c->status) != 0 && c->link == TYPE_AND) {
                // Encountered false AND condition, skip all following AND conditions
                while (c && (c->link == TYPE_AND || c->link == TYPE_PIPE)) {
                    c = c->next;
                }
            } else if (c && WEXITSTATUS(c->status) == 0 && c->link == TYPE_OR) {
                // Encountered true OR condition, skip all following OR conditions
                while (c && (c->link == TYPE_OR || c->link == TYPE_PIPE)) {
                    c = c->next;
                }
            }
        } else {  
            // Pipeline did not exit normally      
            if (c && c->link == TYPE_AND && WEXITSTATUS(c->status) == 0) {
                _exit(EXIT_FAILURE);
            } else if (c && c->link == TYPE_OR && WEXITSTATUS(c->status) != 0) {
                c = c->next;
            }
        }
        if (c->link == TYPE_SEQUENCE) {
            // Process has finished a sequence of commands
            return;
        }
        c = c->next;
    }
}

// Helper function to permit running processes in the background
command* scan(command* c) {
    while (c && c->link != TYPE_SEQUENCE && c->link != TYPE_BACKGROUND) {
        c = c->next;
    }
    // Returns the command which terminates a:
        // sequence, 
        // background process, 
        // or user input
    return c;
}

void run_list(command* c) {
    // Run all processes
    while (c) {
        command* c_tmp = scan(c);
        if (c_tmp->link == TYPE_BACKGROUND) {
            // We have a child run the background process
            pid_t child_pid = fork();
            if (child_pid == -1) {
                error_msg();
            }
            if (child_pid == 0) {
                // Child process executes this code
                c_tmp->next = nullptr;   // Truncate to background sequence only
                run_conditional(c);
                _exit(0);
            }
            // Parent process executes this code
            // Skip commands being run by child process
            c = c_tmp;
        } else {
            // Parent runs a non-background sequence of commands
            run_conditional(c); 
        }
        if (c) {
            c = c->next;
        }
    }
    
    // Clean up zombie processes
    int status;
    while (waitpid(-1, &status, WNOHANG) > 0) {
        // Continue calling waitpid on any potential zombie processes
    }
}


// parse_line(s)
//    Parse the command list in `s` and return it. Returns `nullptr` if
//    `s` is empty (only spaces). You’ll extend it to handle more token
//    types.

// for a tree: return the initial pointer, create structs for each level of the tree struct, 
// and after we finish a sequence. move all 3 pointers to the next column.

command* parse_line(const char* s) {
    shell_parser parser(s);
    command* chead = nullptr;    // first command in list
    command* clast = nullptr;    // last command in list
    command* ccur = nullptr;     // current command being built
    for (auto it = parser.begin(); it != parser.end(); ++it) {
        switch (it.type()) {
        case TYPE_NORMAL:
            // Add a new argument to the current command.
            // Might require creating a new command.
            if (!ccur) {
                ccur = new command;
                if (clast) {
                    clast->next = ccur;
                    ccur->prev = clast;
                } else {
                    chead = ccur;
                }
            }
            ccur->args.push_back(it.str());
            break;
        case TYPE_REDIRECT_OP:
            assert(ccur);
            clast = ccur;

            // Save the most recent redirect operation and its path
            if (it.str() == "<") {
                clast->in = true;
                ++it;
                clast->inpath = it.str();
            }
            if (it.str() == ">") {
                clast->out = true;
                ++it;
                clast->outpath = it.str();
            }
            if (it.str() == "2>") {
                clast->err = true;
                ++it;
                clast->errpath = it.str();
            }
            assert(it.type() == TYPE_NORMAL);
            break;
        case TYPE_SEQUENCE:
        case TYPE_BACKGROUND:
        case TYPE_PIPE:
        case TYPE_AND:
        case TYPE_OR:
            // These operators terminate the current command.
            assert(ccur);
            clast = ccur;
            clast->link = it.type();
            ccur = nullptr;
            break;
        }
    }
    return chead;
}


int main(int argc, char* argv[]) {
    FILE* command_file = stdin;
    bool quiet = false;

    // Check for `-q` option: be quiet (print no prompts)
    if (argc > 1 && strcmp(argv[1], "-q") == 0) {
        quiet = true;
        --argc, ++argv;
    }

    // Check for filename option: read commands from file
    if (argc > 1) {
        command_file = fopen(argv[1], "rb");
        if (!command_file) {
            perror(argv[1]);
            return 1;
        }
    }

    // - Put the shell into the foreground
    // - Ignore the SIGTTOU signal, which is sent when the shell is put back
    //   into the foreground
    claim_foreground(0);
    set_signal_handler(SIGTTOU, SIG_IGN);

    char buf[BUFSIZ];
    int bufpos = 0;
    bool needprompt = true;

    while (!feof(command_file)) {
        // Print the prompt at the beginning of the line
        if (needprompt && !quiet) {
            printf("sh61[%d]$ ", getpid());
            fflush(stdout);
            needprompt = false;
        }

        // Read a string, checking for error or EOF
        if (fgets(&buf[bufpos], BUFSIZ - bufpos, command_file) == nullptr) {
            if (ferror(command_file) && errno == EINTR) {
                // ignore EINTR errors
                clearerr(command_file);
                buf[bufpos] = 0;
            } else {
                if (ferror(command_file)) {
                    perror("sh61");
                }
                break;
            }
        }

        // If a complete command line has been provided, run it
        bufpos = strlen(buf);
        if (bufpos == BUFSIZ - 1 || (bufpos > 0 && buf[bufpos - 1] == '\n')) {
            if (command* c = parse_line(buf)) {
                run_list(c);
                delete c;
            }
            bufpos = 0;
            needprompt = 1;
        }

        // Handle zombie processes and/or interrupt requests
        // Your code here!
    }

    return 0;
}
