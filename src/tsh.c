/* 
 * tsh - A tiny shell program with job control
 * 
 * <Put your name and login ID here>
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>

/* Misc manifest constants */
#define MAXLINE    1024   /* max line size */
#define MAXARGS     128   /* max args on a command line */
#define MAXJOBS      16   /* max jobs at any point in time */
#define MAXJID    1<<16   /* max job ID */

/* Job states */
#define UNDEF 0 /* undefined */
#define FG 1    /* running in foreground */
#define BG 2    /* running in background */
#define ST 3    /* stopped */

/* 
 * Jobs states: FG (foreground), BG (background), ST (stopped)
 * Job state transitions and enabling actions:
 *     FG -> ST  : ctrl-z
 *     ST -> FG  : fg command
 *     ST -> BG  : bg command
 *     BG -> FG  : fg command
 * At most 1 job can be in the FG state.
 */

/* Global variables */
extern char **environ;      /* defined in libc */
char prompt[] = "tsh> ";    /* command line prompt (DO NOT CHANGE) */
int verbose = 0;            /* if true, print additional output */
int nextjid = 1;            /* next job ID to allocate */
char sbuf[MAXLINE];         /* for composing sprintf messages */

struct job_t {              /* The job struct */
    pid_t pid;              /* job PID */
    int jid;                /* job ID [1, 2, ...] */
    int state;              /* UNDEF, BG, FG, or ST */
    char cmdline[MAXLINE];  /* command line */
};
struct job_t jobs[MAXJOBS]; /* The job list */
/* End global variables */


/* Function prototypes */

/* Here are the functions that you will implement */
void eval(char *cmdline);
int builtin_cmd(char **argv);
void do_bgfg(char **argv);
void waitfg(pid_t pid);

void sigchld_handler(int sig);
void sigtstp_handler(int sig);
void sigint_handler(int sig);

/* Here are helper routines that we've provided for you */
int parseline(const char *cmdline, char **argv); 
void sigquit_handler(int sig);

void clearjob(struct job_t *job);
void initjobs(struct job_t *jobs);
int maxjid(struct job_t *jobs); 
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline);
int deletejob(struct job_t *jobs, pid_t pid); 
pid_t fgpid(struct job_t *jobs);
struct job_t *getjobpid(struct job_t *jobs, pid_t pid);
struct job_t *getjobjid(struct job_t *jobs, int jid); 
int pid2jid(pid_t pid); 
void listjobs(struct job_t *jobs);

void usage(void);
void unix_error(char *msg);
void app_error(char *msg);
typedef void handler_t(int);
handler_t *Signal(int signum, handler_t *handler);

/*
 * main - The shell's main routine 
 */
int main(int argc, char **argv) 
{
    char c;
    char cmdline[MAXLINE];
    int emit_prompt = 1; /* emit prompt (default) */

    /* Redirect stderr to stdout (so that driver will get all output
     * on the pipe connected to stdout) */
    dup2(1, 2);

    /* Parse the command line */
    while ((c = getopt(argc, argv, "hvp")) != EOF) {
        switch (c) {
        case 'h':             /* print help message */
            usage();
            break;
        case 'v':             /* emit additional diagnostic info */
            verbose = 1;
            break;
        case 'p':             /* don't print a prompt */
            emit_prompt = 0;  /* handy for automatic testing */
            break;
        default:
            usage();
        }
    }

    /* Install the signal handlers */

    /* These are the ones you will need to implement */
    Signal(SIGINT,  sigint_handler);   /* ctrl-c */
    Signal(SIGTSTP, sigtstp_handler);  /* ctrl-z */
    Signal(SIGCHLD, sigchld_handler);  /* Terminated or stopped child */

    /* This one provides a clean way to kill the shell */
    Signal(SIGQUIT, sigquit_handler); 

    /* Initialize the job list */
    initjobs(jobs);

    /* Execute the shell's read/eval loop */
    while (1) {

        /* Read command line */
        if (emit_prompt) {
            printf("%s", prompt);
            fflush(stdout);
        }
        if ((fgets(cmdline, MAXLINE, stdin) == NULL) && ferror(stdin))
            app_error("fgets error");
        if (feof(stdin)) { /* End of file (ctrl-d) */
            fflush(stdout);
            exit(0);
        }

        /* Evaluate the command line */
        eval(cmdline);
        fflush(stdout);
        fflush(stdout);
    } 

    exit(0); /* control never reaches here */
}
  
/* 
 * eval - Evaluate the command line that the user has just typed in
 * 
 * If the user has requested a built-in command (quit, jobs, bg or fg)
 * then execute it immediately. Otherwise, fork a child process and
 * run the job in the context of the child. If the job is running in
 * the foreground, wait for it to terminate and then return.  Note:
 * each child process must have a unique process group ID so that our
 * background children don't receive SIGINT (SIGTSTP) from the kernel
 * when we type ctrl-c (ctrl-z) at the keyboard.  
*/
void eval(char *cmdline) {
    // Prepare a signal set to block and unblock (race conditions)
    sigset_t signal_set;
    sigemptyset(&signal_set);
    sigaddset(&signal_set, SIGCHLD);

    char* argv[MAXARGS];
    int bg;
    pid_t pid;

    // Grab our argv array, check to see whether we are running bg or fg
    bg = parseline(cmdline, argv);
    // return on empty arguments
    if (argv[0] == NULL)
        return;

    // Try to execute a builtin command
    if (!builtin_cmd(argv)){
        // Block sigchld to prevent race conditions
        sigprocmask(SIG_BLOCK, &signal_set, NULL);

        int infd = 0;
        int outfd = 0;
        int pipe_fds[2] = {-1, -1};
        char **arg = argv;
        char **argv2 = NULL;

        // Attempt to open any output redirection, and split the argv array
        while (*arg){
            if (!strcmp(*arg, "<")){
                // Open an input file
                if ((infd = open(*(arg + 1), O_RDONLY)) < 0)
                    perror("Could not open file for reading");
                *arg = NULL;
            } else if (!strcmp(*arg, "|")){
                // Open a pipe
                if (pipe(pipe_fds) < 0)
                    unix_error("pipe");
                *arg = NULL;
                argv2 = arg + 1;
            } else if (!strcmp(*arg, ">")){
                // Open an output file
                if ((outfd = open(*(arg + 1), O_WRONLY | O_CREAT | O_TRUNC, 0666)) < 0)
                    perror("Could not open file for writing");
                *arg = NULL;
            }
            arg++;
        }

        pid = fork();
        if (pid < 0)
            unix_error("fork");
        if (pid == 0){
            // unblock for the child process
            sigprocmask(SIG_UNBLOCK, &signal_set, NULL);
            setpgid(0, 0);
            // Redirect standard input, if appropriate
            if (infd > 0){
                dup2(infd, 0);
                close(infd);
            }
            // Redirect standard output, if appropriate
            if (pipe_fds[1] > 0){
                // Connect stdout to the out port of the pipe
                dup2(pipe_fds[1], 1);
                close(pipe_fds[0]);
                close(pipe_fds[1]);
            } else if (outfd > 0){
                dup2(outfd, 1);
                close(outfd);
            }
            if (execve(argv[0], argv, environ) < 0){
                printf("%s: command not found.\n", argv[0]);
                exit(0);
            }
        }

        // If a pipe was detected, execute the second process
        if (argv2 != NULL){
            pid_t pid2 = fork();
            if (pid2 < 0)
                unix_error("fork");
            if (pid2 == 0){
                // Unblock for the child proecss
                sigprocmask(SIG_UNBLOCK, &signal_set, NULL);
                // Add this process to the process group of the first
                setpgid(pid, pid);
                // Pipe stdin to the input port of our pipe
                dup2(pipe_fds[0], 0);
                close(pipe_fds[0]);
                close(pipe_fds[1]);
                // Redirect stdout, if appropriate
                if (outfd > 0){
                    dup2(outfd, 1);
                    close(outfd);
                }
                if (execve(argv2[0], argv2, environ) < 0){
                    printf("%s: command not found.\n", argv[0]);
                    exit(0);
                }
            }
        }

        // Close any opened file descriptors
        if (infd > 0)
            close(infd);
        if (outfd > 0)
            close(outfd);
        if (pipe_fds[0] > 0){
            close(pipe_fds[0]);
            close(pipe_fds[1]);
        }

        // Add the new job to the job pool
        addjob(jobs, pid, (bg ? BG : FG) , cmdline);
        // Unblock our child signal, so that we can reap our children
        sigprocmask(SIG_UNBLOCK, &signal_set, NULL);
        if (!bg){
            // If we're a fg process, wait for it to complete
            waitfg(pid);
        } else
            printf("[%d] (%d) %s", getjobpid(jobs, pid)->jid, pid, cmdline);
    }
    return;
}

/* 
 * parseline - Parse the command line and build the argv array.
 * 
 * Characters enclosed in single quotes are treated as a single
 * argument.  Return true if the user has requested a BG job, false if
 * the user has requested a FG job.  
 */
int parseline(const char *cmdline, char **argv) {
    static char array[MAXLINE]; /* holds local copy of command line */
    char *buf = array;          /* ptr that traverses command line */
    char *delim;                /* points to first space delimiter */
    int argc;                   /* number of args */
    int bg;                     /* background job? */

    strcpy(buf, cmdline);
    buf[strlen(buf)-1] = ' ';  /* replace trailing '\n' with space */
    while (*buf && (*buf == ' ')) /* ignore leading spaces */
	buf++;

    /* Build the argv list */
    argc = 0;
    if (*buf == '\'') {
        buf++;
        delim = strchr(buf, '\'');
    } else {
        delim = strchr(buf, ' ');
    }

    while (delim) {
        argv[argc++] = buf;
        *delim = '\0';
        buf = delim + 1;
        while (*buf && (*buf == ' ')) /* ignore spaces */
           buf++;

        if (*buf == '\'') {
            buf++;
            delim = strchr(buf, '\'');
        } else {
            delim = strchr(buf, ' ');
        }
    }
    argv[argc] = NULL;
    
    if (argc == 0)  /* ignore blank line */
        return 1;

    /* should the job run in the background? */
    if ((bg = (*argv[argc-1] == '&')) != 0) {
        argv[--argc] = NULL;
    }
    return bg;
}

/* 
 * builtin_cmd - If the user has typed a built-in command then execute
 *    it immediately.  
 */
int builtin_cmd(char **argv)  {
    // Exit the shell
    if (!strcmp(argv[0], "quit"))
        exit(0);
    // Eat solitary & commands
    if (!strcmp(argv[0], "&"))
        return 1;
    // Display the jobs list
    if (!strcmp(argv[0], "jobs")){
        listjobs(jobs);
        return 1;
    }
    // Bring stopped jobs into the foreground or background
    if (!strcmp(argv[0], "fg") || !strcmp(argv[0], "bg")){
        do_bgfg(argv);
        return 1;
    }
    // Kill stopped or background jobs
    if (!strcmp(argv[0], "kill")){
        if (!argv[1] || strlen(argv[1]) == 0){
            printf("%s command requires PID or %%jobid argument.\n", argv[0]);
            return 1;
        }
        // Interpret a prepended % as a job id
        char *startptr = (*(argv[1]) == '%') ? argv[1] + 1 : argv[1];
        char *endptr = NULL;
        errno = 0;
        int id = strtol(startptr, &endptr, 10);
        if (endptr == startptr
                || '\0' != *endptr
                || ((LONG_MIN == id || LONG_MAX == id) && ERANGE == errno)
                || id > INT_MAX
                || id < INT_MIN ) {
            printf("%s: argument must be a PID or %%jobid\n", argv[0]);
            return 1;
        }
        struct job_t *job;

        if ((*(argv[1]) == '%') && !(job = getjobjid(jobs, id))){
            printf("%s: No such job\n", argv[1]);
            return 1;
        } else if (!(job = getjobpid(jobs, id))){
            printf("(%d): No such process\n", id);
            return 1;
        }
        kill(-job->pid, SIGKILL);
        return 1;
    }
    return 0;     /* not a builtin command */
}

/* 
 * do_bgfg - Execute the builtin bg and fg commands
 */
void do_bgfg(char **argv)  {
    // Extra error checking can't hurt, though this should be impossible
    if (fgpid(jobs) != 0){
        printf("Foreground process detected.\n");
        return;
    }
    // If there was no supplied argument, run away
    if (!argv[1]){
        printf("%s command requires PID or %%jobid argument\n", argv[0]);
        return;
    }
    // Interpret a prepended % as a job id
    char *startptr = (*(argv[1]) == '%') ? argv[1] + 1 : argv[1];
    char *endptr = NULL;
    errno = 0;
    int id = strtol(startptr, &endptr, 10);
    if (endptr == startptr
            || '\0' != *endptr
            || ((LONG_MIN == id || LONG_MAX == id) && ERANGE == errno)
            || id > INT_MAX
            || id < INT_MIN ) {
        printf("%s: argument must be a PID or %%jobid\n", argv[0]);
        return;
    }

    struct job_t *job = (*(argv[1]) == '%') ? getjobjid(jobs, id) : getjobpid(jobs, id);
    if (!job){
        if ((*(argv[1]) == '%')){
            printf("%s: No such job\n", argv[1]);
        } else {
            printf("(%d): No such process\n", id);
        }
        return;
    }

    if (!strcmp(argv[0], "fg")){
        // Resume a stopped process in the fg
        if (job->state == ST){
            kill(-job->pid, SIGCONT);
        }
        job->state = FG;
        waitfg(job->pid);
    } else {
        // Resume a stopped process in the bg
        if (job->state == ST){
            kill(-job->pid, SIGCONT);
        }
        job->state = BG;
    }
    return;
}

/* 
 * waitfg - Block until process pid is no longer the foreground process
 */
void waitfg(pid_t pid) {
    struct job_t *job = getjobpid(jobs, pid);

    // While there's a foreground job...
    while (job && job->state == FG){
        sleep(1); // zzzzzzzzz...
        job = getjobpid(jobs, pid);
    }
}

/*****************
 * Signal handlers
 *****************/

/* 
 * sigchld_handler - The kernel sends a SIGCHLD to the shell whenever
 *     a child job terminates (becomes a zombie), or stops because it
 *     received a SIGSTOP or SIGTSTP signal. The handler reaps all
 *     available zombie children, but doesn't wait for any other
 *     currently running children to terminate.  
 */
void sigchld_handler(int sig)  {
    int status;
    pid_t pid;
    while (1){
        // Get status on all stopped and terminated children
        pid = waitpid(-1, &status, WNOHANG | WUNTRACED);
        if (pid < 0){
            if (errno == ECHILD){
                // If we're out of children... time to make some more!
                return;
            } else if (errno == EINTR){
                // If we were interrupted, well that's a crying shame. This shouldn't be possible btw
                continue;
            } else {
                // ABORT! ABORT MISSION! ABANDON THREAD!!!
                unix_error("waitpid returned unspecified error.");
            }
        } else if (pid == 0){
            // This tells us that nothing's terminated or waiting, I'm pretty sure?
            return;
        }
        // If the child exited cleanly, just remove it from the pool
        if (WIFEXITED(status)){
            //printf("Process %d exited with status %d\n", pid, WEXITSTATUS(status));
            deletejob(jobs, pid);
        } else if (WIFSIGNALED(status)){
            // Let users know if their child was killed
            printf("Job [%d] (%d) terminated by signal %d\n", getjobpid(jobs, pid)->jid, pid, WTERMSIG(status));
            deletejob(jobs, pid);
        } else if (WIFSTOPPED(status)) {
            // Let users know if their child was stopped
            printf("Job [%d] (%d) stopped by signal %d\n", getjobpid(jobs, pid)->jid, pid, WSTOPSIG(status));
            struct job_t *job = getjobpid(jobs, pid);
            if (job){
                job->state = ST;
            }
        }
    }
    return;
}

/* 
 * sigint_handler - The kernel sends a SIGINT to the shell whenver the
 *    user types ctrl-c at the keyboard.  Catch it and send it along
 *    to the foreground job.  
 */
void sigint_handler(int sig) {
    pid_t pid;
    // Move the signal along...
    if ((pid = fgpid(jobs)) > 0){
        // Be sure to send it to the entire process group
        if (kill(-pid, sig) < 0)
            unix_error("kill");
    }
}

/*
 * sigtstp_handler - The kernel sends a SIGTSTP to the shell whenever
 *     the user types ctrl-z at the keyboard. Catch it and suspend the
 *     foreground job by sending it a SIGTSTP.  
 */
void sigtstp_handler(int sig) {
    pid_t pid;
    // Move the signal along...
    if ((pid = fgpid(jobs)) > 0){
        // Be sure to send it to the entire process group
        if (kill(-pid, sig) < 0)
            unix_error("kill");
    }
}

/*********************
 * End signal handlers
 *********************/

/***********************************************
 * Helper routines that manipulate the job list
 **********************************************/

/* clearjob - Clear the entries in a job struct */
void clearjob(struct job_t *job) {
    job->pid = 0;
    job->jid = 0;
    job->state = UNDEF;
    job->cmdline[0] = '\0';
}

/* initjobs - Initialize the job list */
void initjobs(struct job_t *jobs) {
    int i;

    for (i = 0; i < MAXJOBS; i++)
	clearjob(&jobs[i]);
}

/* maxjid - Returns largest allocated job ID */
int maxjid(struct job_t *jobs) {
    int i, max=0;

    for (i = 0; i < MAXJOBS; i++)
	if (jobs[i].jid > max)
	    max = jobs[i].jid;
    return max;
}

/* addjob - Add a job to the job list */
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline) {
    int i;
    
    if (pid < 1)
	return 0;

    for (i = 0; i < MAXJOBS; i++) {
        if (jobs[i].pid == 0) {
            jobs[i].pid = pid;
            jobs[i].state = state;
            jobs[i].jid = nextjid++;
            if (nextjid > MAXJOBS)
            nextjid = 1;
            strcpy(jobs[i].cmdline, cmdline);
            if(verbose){
                printf("Added job [%d] %d %s\n", jobs[i].jid, jobs[i].pid, jobs[i].cmdline);
            }
            return 1;
        }
    }
    printf("Tried to create too many jobs\n");
    return 0;
}

/* deletejob - Delete a job whose PID=pid from the job list */
int deletejob(struct job_t *jobs, pid_t pid) {
    int i;

    if (pid < 1)
	return 0;

    for (i = 0; i < MAXJOBS; i++) {
        if (jobs[i].pid == pid) {
            clearjob(&jobs[i]);
            nextjid = maxjid(jobs)+1;
            return 1;
        }
    }
    return 0;
}

/* fgpid - Return PID of current foreground job, 0 if no such job */
pid_t fgpid(struct job_t *jobs) {
    int i;

    for (i = 0; i < MAXJOBS; i++)
	if (jobs[i].state == FG)
	    return jobs[i].pid;
    return 0;
}

/* getjobpid  - Find a job (by PID) on the job list */
struct job_t *getjobpid(struct job_t *jobs, pid_t pid) {
    int i;

    if (pid < 1)
	return NULL;
    for (i = 0; i < MAXJOBS; i++)
	if (jobs[i].pid == pid)
	    return &jobs[i];
    return NULL;
}

/* getjobjid  - Find a job (by JID) on the job list */
struct job_t *getjobjid(struct job_t *jobs, int jid) {
    int i;

    if (jid < 1)
	return NULL;
    for (i = 0; i < MAXJOBS; i++)
	if (jobs[i].jid == jid)
	    return &jobs[i];
    return NULL;
}

/* pid2jid - Map process ID to job ID */
int pid2jid(pid_t pid) {
    int i;

    if (pid < 1)
	return 0;
    for (i = 0; i < MAXJOBS; i++)
	if (jobs[i].pid == pid) {
        return jobs[i].jid;
    }
    return 0;
}

/* listjobs - Print the job list */
void listjobs(struct job_t *jobs) {
    int i;
    
    for (i = 0; i < MAXJOBS; i++) {
        if (jobs[i].pid != 0) {
            printf("[%d] (%d) ", jobs[i].jid, jobs[i].pid);
            switch (jobs[i].state) {
            case BG:
                printf("Running ");
                break;
            case FG:
                printf("Foreground ");
                break;
            case ST:
                printf("Stopped ");
                break;
            default:
                printf("listjobs: Internal error: job[%d].state=%d ",
                   i, jobs[i].state);
            }
            printf("%s", jobs[i].cmdline);
        }
    }
}
/******************************
 * end job list helper routines
 ******************************/


/***********************
 * Other helper routines
 ***********************/

/*
 * usage - print a help message
 */
void usage(void) {
    printf("Usage: shell [-hvp]\n");
    printf("   -h   print this message\n");
    printf("   -v   print additional diagnostic information\n");
    printf("   -p   do not emit a command prompt\n");
    exit(1);
}

/*
 * unix_error - unix-style error routine
 */
void unix_error(char *msg)
{
    fprintf(stdout, "%s: %s\n", msg, strerror(errno));
    exit(1);
}

/*
 * app_error - application-style error routine
 */
void app_error(char *msg)
{
    fprintf(stdout, "%s\n", msg);
    exit(1);
}

/*
 * Signal - wrapper for the sigaction function
 */
handler_t *Signal(int signum, handler_t *handler) 
{
    struct sigaction action, old_action;

    action.sa_handler = handler;  
    sigemptyset(&action.sa_mask); /* block sigs of type being handled */
    action.sa_flags = SA_RESTART; /* restart syscalls if possible */

    if (sigaction(signum, &action, &old_action) < 0)
	unix_error("Signal error");
    return (old_action.sa_handler);
}

/*
 * sigquit_handler - The driver program can gracefully terminate the
 *    child shell by sending it a SIGQUIT signal.
 */
void sigquit_handler(int sig) 
{
    printf("Terminating after receipt of SIGQUIT signal\n");
    exit(1);
}



