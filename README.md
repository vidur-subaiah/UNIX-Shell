# UNIX-Shell

The Tiny Shell (tsh) is a simple shell program.

tsh supports the following features:

* Command Evaluation - A command line interface that accepts user input and executes commands. Built-in commands include:

quit - exits the shell
logout - logs the user out of the shell
adduser - adds a new user to the system (requires root privileges)
history - lists the last 10 commands executed
!N - executes the Nth command from the history
jobs - lists all background jobs
bg - resumes a background job
fg - resumes a background job in the foreground

The user may also execute any other command that is available on the system as a runnable script by spawning a child process.

* Job Control - The shell supports running jobs in the background and foreground. The shell also supports suspending (ctrl-z), terminating (ctrl-c) and resuming jobs. The shell also supports the jobs command to list all background jobs and the bg and fg commands to resume a background job in the background or foreground respectively.

* Signal Handling - The shell supports the following signals:

SIGINT - terminates the foreground process
SIGTSTP - suspends the foreground process
SIGCHLD - handles the termination of a child process

* Process File Management - The shell can run any command that is available on the system as a runnable script. In running such commands that are not built-in, the shell creates a folder in the proc directory for each process that is spawned, where the folder name is the process pid and contains a status file containing the following fields that are changed as the state of the process changes:

Name - the command associated with the process
Pid - the process id
PPid - the parent process id
PGid - the process group id
Sid - the session id
STAT - the process state
Username - the username of the user that spawned the process
A struct stat is used to store this information when it is to be written to the status file.

* User Management - The shell supports multiple users along with the root user. The root user has the ability to add new users to the system. All built-in commands are restricted to a particular user. The adduser command can only be executed successfully by the root user.

## Implementation

In this section, we will discuss the implementation of the various features of the shell. All code referenced can be found within tsh.c.

1. Login
The shell starts by prompting the user for a username and password. The shell then checks if the user exists in the system and if the password entered is matches the password stored in the system for that particular user. If the user does not exist or the password does not match, the shell returns an error message stating that the user authentication has failed and the user must try again as shown below -

User Authentication failed. Please try again.

If the username and password are correct, the shell displays the prompt as shown below -

tsh>

Once a user is logged in to the shell, a history of the last 10 commands executed by the user is stored in the <user directory>/.tsh_history file. This file contains at most 10 entries (1 on each line) and the data is loaded in to a history array. Additionally, once the user logs in to the shell, the shell creates a folder in the proc directory for the shell process itself.

While entering the username, if the user enters the command quit the shell exits. The username and password data is stored in the etc/passwd file. The etc/passwd file is a text file that contains the following fields (separated by :) for each user -

username
password
user directory

New users can be added to the system using the adduser command. Given below is the usage for the adduser command -

tsh> adduser <user_name> <password>

When a new user is added to the system, the shell creates a new folder in the home directory with the name of the user and creates a .tsh_history file in the user directory to store a history of at most 10 most recently executed commands by that user (in the future). The user is then added to the etc/passwd file in the format described above.

Only the root user can add new users to the system. If a user who is not the root tries to add a user to the system, the shell displays the following error message -

root privileges required to run adduser.

If the root tries to add a user that already exists (i.e. the username is already present in the etc/passwd file), the shell displays the following error message -

User <user_name> may already exist.

where the user_name refers to the username entered by the user in the adduser command.

2. Command Evaluation

The shell evaluates the commands entered by the user using the eval() function. This function first parses the text entered by the user in the command line using the parseline() function. This function determines whether the command should run in the background or foreground and creates the argv array that contains the command and its arguments. It then checks if the command to be executes is valid i.e. not an empty line. Following this, it writes the command to the .tsh_history file. After doing so, it checks if the command is a built-in command. If it is, the shell executes the built-in command without spawning a new process and in the foreground. Therefore, no proc entery needs to be created for built-in commands. If the command is not a built-in command, the shell starts by blocking the SIGCHLD signal to prevent the shell from handling the termination of the child process before it is spawned. The shell then forks a child process and the child process executes the command. Before the child process is told to execute the command, the SIGCHLD is unblocked, so that the child process can be terminated by the shell if it is terminated by the user. In addition to this, before the child process executes the command, the shell is placed in a new proces group to prevent it from being terminated if the child process is terminated by the user (i.e. ctrl-c) and a proc entry is created with the pid of the child process spawned.

3. Built-in Commands

The built-in commands supported are the following -

quit - This command exits the shell

logout - This command enables the user to logout of the shell. The command first checks whether there are any remaining jobs running or suspended. 

adduser - This command adds a new user to the system (requires root privileges).

history - This command lists the 10 most recent commands entered by the user.

!N - This command executes the Nth command in the history. N can range from 1 to 10. 

jobs - This command lists all jobs that are currently running or suspended. The jobs are listed in the order in which they were added to the job queue. 

bg - This command resumes a suspended job in the background.

fg - This command resumes a suspended job in the foreground. 

Jobs states: FG (foreground), BG (background), ST (stopped)

Job state transitions and enabling actions:

    FG -> ST  : ctrl-z
    ST -> FG  : fg command
    ST -> BG  : bg command
    BG -> FG  : fg command

4. PROC

As mentioned above, the shell can run any command that is available on the system as a runnable script. In running such commands that are not built-in, the shell creates a folder in the proc directory for each process that is spawned, where the folder name is the process pid and contains a status file containing the following fields that are changed as the state of the process changes: 1. Name - the command associated with the process 2. Pid - the process id 3. PPid - the parent process id 4. PGid - the process group id 5. Sid - the session id 6. STAT - the process state 7. Username - the username of the user that spawned the process

A struct stat is used to store this information when it is to be written to the status file. The struct is shown below -

struct stat_t {
    char name[MAXLINE];     /* name of the command */
    pid_t pid;              /* process id */
    pid_t ppid;             /* parent process id */
    pid_t pgid;             /* process group id */
    pid_t sid;              /* session id */
    char state[MAXLINE];    /* state of the process */
    char uname[MAXLINE];    /* user name */
};

5. Job Control

The signals handlers that the shell implements are the following:

SIGCHLD - When a SIGCHLD signal is received, the function checks if the signal received is the correct signal. If so, it blocks all signals to allow for this signal to be processed before handling any other signals that could be received. This is because only one signal of that type can be handled at a time and any signals of the same type are ignored if received when processing the current signal of that type. In the handler, we wait for the child process to complete and then check if the process terminated or was stopped.

SIGTSTP - When a SIGSTP signal is received, the function checks if the signal received is the correct signal. If so, it blocks all signals to allow for this signal to be processed before handling any other signals that could be received. This is because only one signal of that type can be handled at a time and any signals of the same type are ignored if received when processing the current signal of that type.

SIGINT - When a SIGINT signal is received, the function checks if the signal received is the correct signal. If so, it blocks all signals to allow for this signal to be processed before handling any other signals that could be received. This is because only one signal of that type can be handled at a time and any signals of the same type are ignored if received when processing the current signal of that type.
