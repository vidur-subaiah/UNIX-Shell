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
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <sys/stat.h>

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
char * username;            /* The name of the user currently logged into the shell */
struct job_t {              /* The job struct */
    pid_t pid;              /* job PID */
    int jid;                /* job ID [1, 2, ...] */
    int state;              /* UNDEF, BG, FG, or ST */
    char cmdline[MAXLINE];  /* command line */
};
struct job_t jobs[MAXJOBS]; /* The job list */

char * file_start = "./home/";
char * file_end = "/.tsh_history";
char * proc_start = "./proc/";
char * proc_end ="/status";
char history[10][MAXLINE];
int history_index = 0;
pid_t fg_pid = 0;
pid_t session_leader_pid = 0;
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
char * login();
void usage(void);
void unix_error(char *msg);
void app_error(char *msg);
typedef void handler_t(int);
handler_t *Signal(int signum, handler_t *handler);

void update_tsh_history(char * cmdline);
void add_user(char **argv);
static void sio_reverse(char s[]);
static void sio_ltoa(long v, char s[], int b);
static size_t sio_strlen(char s[]);
ssize_t sio_puts(char s[]);
ssize_t sio_putl(long v);
void sio_error(char s[]);
ssize_t Sio_putl(long v);
ssize_t Sio_puts(char s[]);
void Sio_error(char s[]);

/*
 * main - The shell's main routine 
 */
int main(int argc, char **argv) 
{
    char c;
    char cmdline[MAXLINE];
    int emit_prompt = 1; /* emit prompt (default) */
    int first_use = 0;

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

    /* Have a user log into the shell */
    username = login();
    
    while (username == NULL){
        username = login();
    }

    /* Reload history is user logging in again */
    for (int i = 0; i < 10; i++){
        strcpy(history[i], "");
    }

    char file_choice[20 + strlen(username)];
    strcpy(file_choice, file_start);
    strcat(file_choice, username);
    strcat(file_choice, file_end);

    FILE * fp;
    fp = fopen(file_choice, "r");

    char line[MAXLINE];

    if (fp != NULL){
        while (fgets(line, MAXLINE, fp)) {
            strcpy(history[history_index], line);
            history_index = (history_index + 1) % 10;
        }
    }
    else {
        perror("fopen");
        exit(EXIT_FAILURE);
    }

    fclose(fp);

    /* Create a proc entry for the shell */
    pid_t pid = getpid();
    session_leader_pid = getpid();
    pid_t parent_pid = getppid();
    pid_t process_group_id = getpgid(pid);

    char pid_string[MAXLINE];
    sprintf(pid_string, "%d", pid);
    char proc_choice[7 + strlen(pid_string)];
    strcpy(proc_choice, proc_start);
    strcat(proc_choice, pid_string);
    mkdir(proc_choice, 0700);

    char status_file[14 + strlen(pid_string)];
    strcpy(status_file, proc_start);
    strcat(status_file, pid_string);
    strcat(status_file, proc_end);

    FILE * fp7;
    fp7 = fopen(status_file, "w");

    fprintf(fp7, "Name: %s\nPid: %d\nPPid: %d\nPGid: %d\nSid: %d\nSTAT: %s\nUsername: %s", "Shell", pid, parent_pid, process_group_id, session_leader_pid, "Ss", username);

    fclose(fp7);

    /* Execute the shell's read/eval loop */
    while (1) {

	/* Read command line */
	if (emit_prompt) {
        if (first_use == 0){
            first_use = 1;
        }
        else{
            printf("%s", prompt);
	        fflush(stdout);
        }
	}
	if ((fgets(cmdline, MAXLINE, stdin) == NULL) && ferror(stdin))
	    app_error("fgets error");
	if (feof(stdin)) { /* End of file (ctrl-d) */
	    fflush(stdout);
	    exit(0);
	}

	/* Evaluate the command line */
    if (first_use == 1){
        first_use = 2;
    }
    else{
        eval(cmdline);
	    fflush(stdout);
	    fflush(stdout);
    }
    } 

    exit(0); /* control never reaches here */
}

/*
 * login - Performs user authentication for the shell
 *
 * See specificaiton for how this function should act
 *
 * This function returns a string of the username that is logged in
 */
char * login() {

    static char user_name[MAXLINE];
    printf("username: ");
    scanf("%s", user_name);

    if (strcmp(user_name, "quit") == 0){
        exit(0);
    }

    static char password[MAXLINE];
    printf("password: ");
    scanf("%s", password);

    if (strcmp(password, "quit") == 0){
        exit(0);
    }

    FILE * fp1;
    fp1 = fopen("./etc/passwd.txt", "r");

    int username_check = 0;
    int password_check = 0;

    if (fp1 != NULL){
        char *line = NULL;
        size_t size = 0;
        long nRead = getline(&line, &size, fp1);
        while (nRead != -1){
            char * token = strtok(line, ":");
            if (token != NULL){
                if (strcmp(user_name, token) == 0){
                    username_check = 1;
                }
            }
            token = strtok(NULL, ":");
            if (token != NULL){
                if (strcmp(password, token) == 0){
                    password_check = 1;
                }
            }
            free(line);
            line = NULL;
            nRead = getline(&line, &size, fp1);
        }
    }
    else {
        perror("fopen");
        exit(EXIT_FAILURE);
    }

    fclose(fp1);

    if (username_check == 1 && password_check == 1){
        return user_name;
    }
    else {
        printf("User Authentication failed. Please try again.\n");
        return NULL;
    }
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
void eval(char *cmdline) 
{
    int bg;
    pid_t pid;
    sigset_t mask_all, mask_one, prev_one;

    sigfillset(&mask_all);
    sigemptyset(&mask_one);
    sigaddset(&mask_one, SIGCHLD);

    char **arguments = malloc(MAXARGS * sizeof(*arguments));
    for (int i =0; i < MAXARGS; i++){
        arguments[i] = malloc(MAXLINE * sizeof(**arguments));
    }
    
    bg = parseline(cmdline, arguments);

    // printf("%d\n", bg);

    // for (int i = 0; i < MAXARGS; i++){
    //     printf("%s\n", arguments[i]);
    // }

    if (arguments[0] == NULL){
        return;
    }

    if (strcmp(arguments[0], "!1") == 0 || strcmp(arguments[0], "!2") == 0 || strcmp(arguments[0], "!3") == 0
        || strcmp(arguments[0], "!4") == 0 || strcmp(arguments[0], "!5") == 0 || strcmp(arguments[0], "!6") == 0
        || strcmp(arguments[0], "!7") == 0 || strcmp(arguments[0], "!8") == 0 || strcmp(arguments[0], "!9") == 0
        || strcmp(arguments[0], "!10") == 0 || strcmp(arguments[0],"bg") == 0 || strcmp(arguments[0], "fg") == 0 || strcmp(arguments[0], "adduser") == 0
        || strcmp(arguments[0],"quit") == 0 || strcmp(arguments[0],"logout") == 0 || strcmp(arguments[0],"history") == 0 || strcmp(arguments[0],"jobs") == 0){
        
        if (strcmp(arguments[0], "!1") == 0 || strcmp(arguments[0], "!2") == 0 || strcmp(arguments[0], "!3") == 0
        || strcmp(arguments[0], "!4") == 0 || strcmp(arguments[0], "!5") == 0 || strcmp(arguments[0], "!6") == 0
        || strcmp(arguments[0], "!7") == 0 || strcmp(arguments[0], "!8") == 0 || strcmp(arguments[0], "!9") == 0
        || strcmp(arguments[0], "!10") == 0){
            builtin_cmd(arguments);
            update_tsh_history(cmdline);
            return;
        }
        else {
            update_tsh_history(cmdline);
            builtin_cmd(arguments);
            return;
        }
    }

    update_tsh_history(cmdline);

    sigprocmask(SIG_BLOCK, &mask_one, &prev_one);
    if ((pid = fork()) == 0) {   /* Child runs user job */
        setpgid(0, 0);
        pid = getpid();
        pid_t parent_pid = getppid();
        pid_t process_group_id = getpgid(pid);

        if (bg ==0){
            fg_pid = 0;

            char pid_string[MAXLINE];
            sprintf(pid_string, "%d", pid);
            char proc_choice[7 + strlen(pid_string)];
            strcpy(proc_choice, proc_start);
            strcat(proc_choice, pid_string);
            mkdir(proc_choice, 0700);

            char status_file[14 + strlen(pid_string)];
            strcpy(status_file, proc_start);
            strcat(status_file, pid_string);
            strcat(status_file, proc_end);

            FILE * fp6;
            fp6 = fopen(status_file, "w");

            fprintf(fp6, "Name: %s\nPid: %d\nPPid: %d\nPGid: %d\nSid: %d\nSTAT: %s\nUsername: %s", arguments[0], pid, parent_pid, process_group_id, session_leader_pid, "R+", username);

            fclose(fp6);
        }
        else {
            char pid_string[MAXLINE];
            sprintf(pid_string, "%d", pid);
            char proc_choice[7 + strlen(pid_string)];
            strcpy(proc_choice, proc_start);
            strcat(proc_choice, pid_string);
            mkdir(proc_choice, 0700);

            char status_file[14 + strlen(pid_string)];
            strcpy(status_file, proc_start);
            strcat(status_file, pid_string);
            strcat(status_file, proc_end);

            FILE * fp6;
            fp6 = fopen(status_file, "w");

            fprintf(fp6, "Name: %s\nPid: %d\nPPid: %d\nPGid: %d\nSid: %d\nSTAT: %s\nUsername: %s", arguments[0], pid, parent_pid, process_group_id, session_leader_pid, "R", username);

            fclose(fp6);
        }
        sigprocmask(SIG_SETMASK, &prev_one, NULL);

        if (execve(arguments[0], arguments, environ) < 0) {
            printf("%s: Command not found.\n", arguments[0]);
            exit(0);
        }
    }

    sigprocmask(SIG_BLOCK, &mask_all, NULL);
    if (bg == 0){
        addjob(jobs, pid, FG, cmdline);
    }
    else {
        addjob(jobs, pid, BG, cmdline);   
     }
    sigprocmask(SIG_SETMASK, &prev_one, NULL);

    if (bg == 0) { // Foreground Job
        int status;
        waitfg(pid);
    }
    else {
        printf("%d %s", pid, cmdline);
    }
    

    return;
}

void update_tsh_history(char * cmdline){
    
    strcpy(history[history_index], cmdline);
    history_index = (history_index + 1) % 10;
    
    char file_choice[20 + strlen(username)];
    strcpy(file_choice, file_start);
    strcat(file_choice, username);
    strcat(file_choice, file_end);

    FILE * fp2;
    fp2 = fopen(file_choice, "w");

    if (fp2 != NULL){
        for (int i = 0; i < 10; i++){
            if (strcmp(history[i], "") == 0) {
                break;
            }
            fprintf(fp2, "%s", history[i]);
        }
    }
    else {
        perror("fopen");
        exit(EXIT_FAILURE);
    }

    fclose(fp2);
}

/* 
 * parseline - Parse the command line and build the argv array.
 * 
 * Characters enclosed in single quotes are treated as a single
 * argument.  Return true if the user has requested a BG job, false if
 * the user has requested a FG job.  
 */
int parseline(const char *cmdline, char **argv) 
{
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
    }
    else {
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
	}
	else {
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
int builtin_cmd(char **argv) 
{  

    if (strcmp(argv[0],"quit") == 0) {
        
        char pid_string[MAXLINE];
        sprintf(pid_string, "%d", session_leader_pid);

        char status_file[14 + strlen(pid_string)];
        strcpy(status_file, proc_start);
        strcat(status_file, pid_string);
        strcat(status_file, proc_end);

        remove(status_file);

        char proc_choice[7 + strlen(pid_string)];
        strcpy(proc_choice, proc_start);
        strcat(proc_choice, pid_string);
        
        rmdir(proc_choice);

        exit(0);
    }

    if (strcmp(argv[0],"logout") == 0) {
        int suspended_jobs = 0;
        for (int i = 0; i < MAXJOBS; i++){
            if (jobs[i].state == ST){
                suspended_jobs = suspended_jobs + 1;
            }
        }
        if (suspended_jobs != 0){
            printf("There are suspended jobs.\n");
        }
        else {
            char pid_string[MAXLINE];
            sprintf(pid_string, "%d", session_leader_pid);

            char status_file[14 + strlen(pid_string)];
            strcpy(status_file, proc_start);
            strcat(status_file, pid_string);
            strcat(status_file, proc_end);

            remove(status_file);

            char proc_choice[7 + strlen(pid_string)];
            strcpy(proc_choice, proc_start);
            strcat(proc_choice, pid_string);
            
            rmdir(proc_choice);

            exit(0);
        }
        
    }

    if (strcmp(argv[0],"history") == 0) {
        int position = history_index;
        int number_label = 1;
        for (int i = 0; i < 10; i++){
            if (strcmp(history[history_index], "") == 0){
                if (strcmp(history[i], "") == 0){
                    break;
                }
                else {
                    printf("%d %s\n", number_label, history[i]);
                }
            }
            else {
                printf("%d %s\n", number_label, history[position]);
                position = (position + 1) % 10;
            }
            number_label = number_label + 1;
        }
    }

    if (strcmp(argv[0],"jobs") == 0) {
        listjobs(jobs);
    }

    if (strcmp(argv[0], "!1") == 0 || strcmp(argv[0], "!2") == 0 || strcmp(argv[0], "!3") == 0
    || strcmp(argv[0], "!4") == 0 || strcmp(argv[0], "!5") == 0 || strcmp(argv[0], "!6") == 0
    || strcmp(argv[0], "!7") == 0 || strcmp(argv[0], "!8") == 0 || strcmp(argv[0], "!9") == 0
    || strcmp(argv[0], "!10") == 0) {
        if (strlen(argv[0]) == 2){
            char numb = argv[0][1];
            int option = atoi(&numb);
            if (strcmp(history[history_index], "") == 0){
                eval(history[option - 1]);
            }
            else {
                int correct_index = (option + (history_index - 1)) % 10;
                eval(history[correct_index]);
            }
        }
        else {
            int option = 10;
            if (strcmp(history[history_index], "") == 0){
                eval(history[option - 1]);
            }
            else {
                int correct_index = (option + (history_index - 1)) % 10;
                eval(history[correct_index]);
            }
        }
    }

    if (strcmp(argv[0], "bg") == 0){
        do_bgfg(argv);
    }

    if (strcmp(argv[0], "fg") == 0){
        do_bgfg(argv);
    }

    if (strcmp(argv[0], "adduser") == 0){
        add_user(argv);
    }

    return 0;     /* not a builtin command */
}

/* 
 * do_bgfg - Execute the builtin bg and fg commands
 */
void do_bgfg(char **argv) 
{
    if (strcmp(argv[0], "bg") == 0){
        int value = atoi(argv[1]);
        struct job_t * job1 = getjobjid(jobs, value);
        struct job_t * job2 = getjobpid(jobs, value);

        if (job1 != NULL){
            job1->state = BG;
            kill(job1->pid, SIGCONT);
            // update proc status file to running 
        }
        else if (job2 != NULL){
            job2->state = BG;
            kill(job2->pid, SIGCONT);
        }
        else {
            printf("Invalid JID/PID Entered.\n");
        }
    }
    else {
        int value = atoi(argv[1]);
        struct job_t * job1 = getjobjid(jobs, value);
        struct job_t * job2 = getjobpid(jobs, value);

        if (job1 != NULL){
            job1->state = FG;
            kill(job1->pid, SIGCONT);
            //update proc status file to running 
        }
        else if (job2 != NULL){
            job2->state = FG;
            kill(job2->pid, SIGCONT);
        }
        else {
            printf("Invalid JID/PID Entered.\n");
        }
    }
    return;
}

/*
* 
*/
void add_user(char **argv)
{
    if (strcmp(username, "root") != 0){
        printf("root privileges required to run adduser.\n");
        return;
    }
    else {

        FILE * fp3;
        fp3 = fopen("./etc/passwd.txt", "r");

        int username_check = 0;

        if (fp3 != NULL){
            char *line = NULL;
            size_t size = 0;
            long nRead = getline(&line, &size, fp3);
            while (nRead != -1){
                char * token = strtok(line, ":");
                if (token != NULL){
                    if (strcmp(argv[1], token) == 0){
                        username_check = 1;
                    }
                }
                free(line);
                line = NULL;
                nRead = getline(&line, &size, fp3);
            }
        }
        else {
            perror("fopen");
            exit(EXIT_FAILURE);
        }

        fclose(fp3);

        if (username_check == 1){
            printf("User already exists.\n");
            return;
        }

        FILE * fp4;
        fp4 = fopen("./etc/passwd.txt", "a");

        fprintf(fp4, "\n%s:%s:/home/%s", argv[1], argv[2], argv[1]);

        fclose(fp4);

        char file_choice[7 + strlen(argv[1])];
        strcpy(file_choice, file_start);
        strcat(file_choice, argv[1]);

        mkdir(file_choice, 0700);

        char create_file[20 + strlen(argv[1])];
        strcpy(create_file, file_start);
        strcat(create_file, argv[1]);
        strcat(create_file, file_end);

        FILE * fp5;
        fp5 = fopen(create_file, "w");

        fclose(fp5);
    }

    return;
}

/* 
 * waitfg - Block until process pid is no longer the foreground process
 */
void waitfg(pid_t pid)
{
    while (1) {
        if (pid == fg_pid) {
            break;
        }
        sleep(1);
    }
    return;
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
void sigchld_handler(int sig) 
{
    int olderrno = errno;
    pid_t pid;
    
    while((pid = wait(NULL)) > 0){ 
        fg_pid = pid;

        if(verbose){
            Sio_puts("Handler reaped child ");
            Sio_putl((long)pid);
            Sio_puts(" \n");
        }

        char pid_string[MAXLINE];
        sprintf(pid_string, "%d", pid);

        char status_file[14 + strlen(pid_string)];
        strcpy(status_file, proc_start);
        strcat(status_file, pid_string);
        strcat(status_file, proc_end);

        remove(status_file);

        char proc_choice[7 + strlen(pid_string)];
        strcpy(proc_choice, proc_start);
        strcat(proc_choice, pid_string);
        
    
        rmdir(proc_choice);

        deletejob(jobs, pid);

    }
    if (errno != ECHILD){
        Sio_error("wait error");
    }
    errno = olderrno;

    return;
}

/* 
 * sigint_handler - The kernel sends a SIGINT to the shell whenver the
 *    user types ctrl-c at the keyboard.  Catch it and send it along
 *    to the foreground job.  
 */
void sigint_handler(int sig) 
{
    pid_t foreground_pid = fgpid(jobs);
    
    if (foreground_pid == 0){
        return;
    }
    else {
        killpg(foreground_pid, SIGINT);
        deletejob(jobs, foreground_pid);

        char pid_string[MAXLINE];
        sprintf(pid_string, "%d", foreground_pid);

        char status_file[14 + strlen(pid_string)];
        strcpy(status_file, proc_start);
        strcat(status_file, pid_string);
        strcat(status_file, proc_end);

        remove(status_file);

        char proc_choice[7 + strlen(pid_string)];
        strcpy(proc_choice, proc_start);
        strcat(proc_choice, pid_string);

        rmdir(proc_choice);
    }
    return;
}

/*
 * sigtstp_handler - The kernel sends a SIGTSTP to the shell whenever
 *     the user types ctrl-z at the keyboard. Catch it and suspend the
 *     foreground job by sending it a SIGTSTP.  
 */
void sigtstp_handler(int sig) 
{
    pid_t foreground_pid = fgpid(jobs);
    
    if (foreground_pid == 0){
        return;
    }
    else {
        killpg(foreground_pid, SIGTSTP);
        // change job state to stopped in job list 
        // struct job_t * job1 = getjobpid(jobs, foreground_pid);
        // job1->state = ST;
        // Now edit the proc status file to indicate the process has stopped 
    }
    return;
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
int maxjid(struct job_t *jobs) 
{
    int i, max=0;

    for (i = 0; i < MAXJOBS; i++)
	if (jobs[i].jid > max)
	    max = jobs[i].jid;
    return max;
}

/* addjob - Add a job to the job list */
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline) 
{
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
int deletejob(struct job_t *jobs, pid_t pid) 
{
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
struct job_t *getjobjid(struct job_t *jobs, int jid) 
{
    int i;

    if (jid < 1)
	return NULL;
    for (i = 0; i < MAXJOBS; i++)
	if (jobs[i].jid == jid)
	    return &jobs[i];
    return NULL;
}

/* pid2jid - Map process ID to job ID */
int pid2jid(pid_t pid) 
{
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
void listjobs(struct job_t *jobs) 
{
    // for (int i =0; i < MAXJOBS; i++) {
    //     printf("%d\n", jobs[i].pid);
    // }
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
void usage(void) 
{
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


/*************************************************************
 * The Sio (Signal-safe I/O) package - simple reentrant output
 * functions that are safe for signal handlers.
 * Citation: csapp.c - Functions for the CS:APP3e book
 *************************************************************/

/* Private sio functions */

/* $begin sioprivate */
/* sio_reverse - Reverse a string (from K&R) */
static void sio_reverse(char s[])
{
    int c, i, j;

    for (i = 0, j = strlen(s)-1; i < j; i++, j--) {
        c = s[i];
        s[i] = s[j];
        s[j] = c;
    }
}

/* sio_ltoa - Convert long to base b string (from K&R) */
static void sio_ltoa(long v, char s[], int b) 
{
    int c, i = 0;
    int neg = (v < 0);

    if (neg) {
    v = -v;
    }
    
    do {  
        s[i++] = ((c = (v % b)) < 10)  ?  c + '0' : c - 10 + 'a';
    } while ((v /= b) > 0);
    if (neg)
    s[i++] = '-';
    s[i] = '\0';
    sio_reverse(s);
}

/* sio_strlen - Return length of string (from K&R) */
static size_t sio_strlen(char s[])
{
    int i = 0;

    while (s[i] != '\0')
        ++i;
    return i;
}
/* $end sioprivate */

/* Public Sio functions */
/* $begin siopublic */

ssize_t sio_puts(char s[]) /* Put string */
{
    return write(STDOUT_FILENO, s, sio_strlen(s)); //line:csapp:siostrlen
}

ssize_t sio_putl(long v) /* Put long */
{
    char s[128];
    
    sio_ltoa(v, s, 10); /* Based on K&R itoa() */  //line:csapp:sioltoa
    return sio_puts(s);
}

void sio_error(char s[]) /* Put error message and exit */
{
    sio_puts(s);
    _exit(1);                                      //line:csapp:sioexit
}
/* $end siopublic */

/*******************************
 * Wrappers for the SIO routines
 ******************************/
ssize_t Sio_putl(long v)
{
    ssize_t n;
  
    if ((n = sio_putl(v)) < 0)
    sio_error("Sio_putl error");
    return n;
}

ssize_t Sio_puts(char s[])
{
    ssize_t n;
  
    if ((n = sio_puts(s)) < 0)
    sio_error("Sio_puts error");
    return n;
}
void Sio_error(char s[])
{
    sio_error(s);
}
/*************************************************************/

