/* 
 * tsh - A tiny shell program with job control
 * 
 * <Name: Pradeep Kumar Vikraman, ID: pvikrama@andrew.cmu.edu>
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <errno.h>

/* Misc manifest constants */
#define MAXLINE    1024   /* max line size */
#define MAXARGS     128   /* max args on a command line */
#define MAXJOBS      16   /* max jobs at any point in time */
#define MAXJID    1<<16   /* max job ID */

/* Job states */
#define UNDEF         0   /* undefined */
#define FG            1   /* running in foreground */
#define BG            2   /* running in background */
#define ST            3   /* stopped */

/* 
 * Jobs states: FG (foreground), BG (background), ST (stopped)
 * Job state transitions and enabling actions:
 *     FG -> ST  : ctrl-z
 *     ST -> FG  : fg command
 *     ST -> BG  : bg command
 *     BG -> FG  : fg command
 * At most 1 job can be in the FG state.
 */

/* Parsing states */
#define ST_NORMAL   0x0   /* next token is an argument */
#define ST_INFILE   0x1   /* next token is the input file */
#define ST_OUTFILE  0x2   /* next token is the output file */


/* Global variables */
extern char **environ;      /* defined in libc */
char prompt[] = "tsh> ";    /* command line prompt (DO NOT CHANGE) */
int verbose = 0;            /* if true, print additional output */
int nextjid = 1;			/* next job ID to allocate */
char sbuf[MAXLINE];         /* for composing sprintf messages */
int state1;
int bg,max;				/* should the job run in bg or fg? */
pid_t foreground;
int parsing_state;			/* indicates if the next token is the
							   input or output file */

struct job_t {              /* The job struct */
	pid_t pid;              /* job PID */
	int jid;                /* job ID [1, 2, ...] */
	int state;              /* UNDEF, BG, FG, or ST */
	char cmdline[MAXLINE];  /* command line */
};
struct job_t job_list[MAXJOBS]; /* The job list */

struct cmdline_tokens {
	int argc;               /* Number of arguments */
	char *argv[MAXARGS];    /* The arguments list */
	char *infile;           /* The input file */
	char *outfile;          /* The output file */
	enum builtins_t {       /* Indicates if argv[0] is a builtin command */
		BUILTIN_NONE,
		BUILTIN_QUIT,
		BUILTIN_JOBS,
		BUILTIN_BG,
		BUILTIN_FG} builtins;
};
/* End global variables */


/* Function prototypes */
void eval(char *cmdline);

void sigchld_handler(int sig);
void sigtstp_handler(int sig);
void sigint_handler(int sig);

/* Here are helper routines that we've provided for you */
int parseline(const char *cmdline, struct cmdline_tokens *tok); 
void sigquit_handler(int sig);
void clearjob(struct job_t *job);
void initjobs(struct job_t *job_list);
int maxjid(struct job_t *job_list); 
int addjob(struct job_t *job_list, pid_t pid, int state, char *cmdline);
int deletejob(struct job_t *job_list, pid_t pid); 
pid_t fgpid(struct job_t *job_list);
struct job_t *getjobpid(struct job_t *job_list, pid_t pid);
struct job_t *getjobjid(struct job_t *job_list, int jid); 
int pid2jid(pid_t pid); 
void listjobs(struct job_t *job_list, int output_fd);
void usage(void);
void unix_error(char *msg);
void app_error(char *msg);
typedef void handler_t(int);
handler_t *Signal(int signum, handler_t *handler);

/*My wrapper functions*/
pid_t Fork(void);
void Kill(pid_t pid, int sig);
void Sigaddset(sigset_t *set, int signum);
void Sigdelset(sigset_t *set, int signum);
void Sigfillset(sigset_t *set);
void Sigemptyset(sigset_t *set);
void Setpgid(pid_t pid, pid_t pgid);
void Sigprocmask(int how, const sigset_t *set, sigset_t *oldset);
void Close(int fd);
int Dup2(int fd1, int fd2);
int Open(const char *pathname, int flags, mode_t mode);
void Execve(const char *filename, char *const argv[], char *const envp[]);

/*
 * main - The shell's main routine 
 */
	int 
main(int argc, char **argv) 
{
	char c;
	char cmdline[MAXLINE];    /* cmdline for fgets */
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
	Signal(SIGINT,  sigint_handler);   /* ctrl-c */
	Signal(SIGTSTP, sigtstp_handler);  /* ctrl-z */
	Signal(SIGCHLD, sigchld_handler);  /* Terminated or stopped child */
	Signal(SIGTTIN, SIG_IGN);
	Signal(SIGTTOU, SIG_IGN);

	/* This one provides a clean way to kill the shell */
	Signal(SIGQUIT, sigquit_handler); 

	/* Initialize the job list */
	initjobs(job_list);


	/* Execute the shell's read/eval loop */
	while (1) {

		if (emit_prompt) {
			printf("%s", prompt);
			fflush(stdout);
		}
		if ((fgets(cmdline, MAXLINE, stdin) == NULL) && ferror(stdin))
			app_error("fgets error");
		if (feof(stdin)) { 
			/* End of file (ctrl-d) */
			printf ("\n");
			fflush(stdout);
			fflush(stderr);
			exit(0);
		}
		/* Remove the trailing newline */
		cmdline[strlen(cmdline)-1] = '\0';
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

/* Functions and system calls written with their starting letter in capitals
 * have their wrapper functions implemented at the bottom of this file. The 
 * wrapper functions where implemented with the help of csapp.c file.
 */

	void 
eval(char *cmdline) 
{
	struct cmdline_tokens tok;
	pid_t pid;
	sigset_t mask,masksuspend;
	char *ptr;
	int id,fd1,fd2,fd3,fdtemp;
	struct job_t *fg,*bg1;

	/* Parse command line */
	bg = parseline(cmdline, &tok);
	if(bg)
		state1=BG;
	else
		state1=FG;

	/* fg built-in command */
	if((tok.builtins)== BUILTIN_FG)
	{
		/*Extract the job number, convert it to int using atoi and store in id*/
		ptr=tok.argv[1];
		ptr=ptr+1;
		id = atoi(ptr);


		/* If there is a job with that job id retrieve it and check if its state
		 * is equal to Stopped(ST), if so convert its state to foreground(FG) 
		 * and send it the SIGCONT signal. After sending the SIGCONT signal, 
		 * wait for the child to finish executing using sigsuspend and 
		 * hence running it effectively in the foreground
		 */
		if((fg=getjobjid(job_list,id))>0)
		{
			if(fg->state==ST)
			{	
				fg->state=FG;
				Kill(-(fg->pid),SIGCONT);
				while(fgpid(job_list))
					sigsuspend(&masksuspend);
				return;
			}
			else
				printf("There is no stopped process right now\n");
			return;
		}
		else
			printf("%s: No such job\n",tok.argv[1]);
	}

	/* bg built-in command */
	if((tok.builtins)== BUILTIN_BG)
	{
		/*Extract the job number, convert it to int using atoi and store in id*/
		ptr=tok.argv[1];
		ptr=ptr+1;
		id = atoi(ptr);

		/* If there is a job with that job id retrieve it and check if its state
		 * is equal to Stopped(ST), if so convert its state to background(BG) 
		 * and send it the SIGCONT signal
		 */
		if((bg1=getjobjid(job_list,id))>0)
		{
			if(bg1->state==ST)
			{
				bg1->state=BG;
				printf("[%d] (%d) %s\n",bg1->jid,bg1->pid,bg1->cmdline);
				Kill(-(bg1->pid),SIGCONT);
				return;
			}
			else
				printf("There is no stopped process right now\n");
			return;
		}
		else
			printf("%s: No such job\n",tok.argv[1]);
	}

	/* quit built-in command */

	if(tok.builtins == BUILTIN_QUIT)
		/* Exit the shell if you get a quit command */
		exit(0);

	/* jobs built-in command */
	if((tok.builtins)== BUILTIN_JOBS)
	{
		/* If the jobs output has to be redirected to another file, open two 
		 * files fd3 and fdtemp and first point to the filetable pointed by 
		 * STDOUT using fdtemp so that when we later reassign the STDOUT
		 * file descriptor to point to the file table pointed by fd3 we still 
		 * have not lost the original file table pointed to by STDOUT. Now since
		 * STDOUT points to file table pointed by fd3, output is printed in that
		 * file. After printing the output restore STDOUT using fdtemp and close
		 * files.
		 */
		if(tok.outfile != NULL)
		{
			fd3=Open(tok.outfile,O_WRONLY,0);
			fdtemp=Open(tok.outfile,O_WRONLY,0);

			Dup2(STDOUT_FILENO,fdtemp);
			Dup2(fd3,STDOUT_FILENO);
			listjobs(job_list,STDOUT_FILENO);
			Dup2(fdtemp,STDOUT_FILENO);

			Close(fd3);
			Close(fdtemp);
		}
		else
			listjobs(job_list,STDOUT_FILENO);
	}

	if(tok.builtins== BUILTIN_NONE)
	{
		/* Delete only SIGCHLD, SIGINT and SIGTSTP from sigsuspend's mask so as
		 * to ensure that it waits for only these three signals
		 */
		Sigfillset(&masksuspend);
		Sigdelset(&masksuspend, SIGCHLD);
		Sigdelset(&masksuspend, SIGINT);
		Sigdelset(&masksuspend, SIGTSTP);

		/* Now for the sigprocmask fill in only SIGCHLD, SIGINT and SIGTSTP so 
		 * as to ensure that these three signals do not interrupt the parent 
		 * until a job is added by blocking them till then , because otherwise 
		 * these lead to race conditions
		 */
		Sigemptyset(&mask);
		Sigaddset(&mask, SIGCHLD);
		Sigaddset(&mask, SIGINT);
		Sigaddset(&mask, SIGTSTP);
		Sigprocmask(SIG_BLOCK,&mask,NULL);
		if((pid=Fork())==0)
		{
			/* Set the group ID of the child to be equal to its PID and put it
			 * in a different group than the parent tsh shell, so as to 
			 * ensure that if it gets a sigint or sigtstp signal only the child 
			 * is terminated or stopped and not the parent tsh shell
			 */
			Setpgid(0,0);

			/* Unblock SIGCHLD, SIGINT and SIGTSTP in the child */
			Sigprocmask(SIG_UNBLOCK, &mask,NULL);

			/* If input redirection redirect stdin to fd2 */
			if(tok.infile != NULL)
			{
				fd2=Open(tok.infile,O_RDONLY,0);
				Dup2(fd2,STDIN_FILENO);
			}

			/* If output redirection redirect stdout to fd1 */
			if(tok.outfile != NULL)
			{
				fd1=Open(tok.outfile,O_WRONLY|O_TRUNC,0);
				Dup2(fd1,STDOUT_FILENO);
			}


			Execve(tok.argv[0],tok.argv,environ);

		}
		addjob(job_list,pid,state1,cmdline);
		/* As seen below unblocking the signals is done only after addjob */

		/* If the bg flag is not set, i.e. if its a foreground job wait for
		 * it to execute using sigsuspend on the parent and making the parent 
		 * wait until it gets a SIGCHLD when the foreground job terminates or it
		 * gets a SIGINT or SIGTSTP signal
		 */
		if(!bg)
		{
			while(fgpid(job_list))
				sigsuspend(&masksuspend);
			Sigprocmask(SIG_UNBLOCK, &mask, NULL);
		}

		/* If its a backgroud process print the details of the job and wait for
		 * the users next command line input
		 */
		else
		{
			Sigprocmask(SIG_UNBLOCK, &mask, NULL);
			printf("[%d] (%d) %s\n",pid2jid(pid),pid,cmdline);
		}

		return;
	}


	if (bg == -1) return;               /* parsing error */
	if (tok.argv[0] == NULL)  return;   /* ignore empty lines */

	return;
}
/* 
 * parseline - Parse the command line and build the argv array.
 * 
 * Parameters:
 *   cmdline:  The command line, in the form:
 *
 *                command [arguments...] [< infile] [> oufile] [&]
 *
 *   tok:      Pointer to a cmdline_tokens structure. The elements of this
 *             structure will be populated with the parsed tokens. Characters 
 *             enclosed in single or double quotes are treated as a single
 *             argument. 
 * Returns:
 *   1:        if the user has requested a BG job
 *   0:        if the user has requested a FG job  
 *  -1:        if cmdline is incorrectly formatted
 * 
 * Note:       The string elements of tok (e.g., argv[], infile, outfile) 
 *             are statically allocated inside parseline() and will be 
 *             overwritten the next time this function is invoked.
 */
	int 
parseline(const char *cmdline, struct cmdline_tokens *tok) 
{

	static char array[MAXLINE];          /* holds local copy of command line */
	const char delims[10] = " \t\r\n";   /* argument delimiters (white-space) */
	char *buf = array;                   /* ptr that traverses command line */
	char *next;                          /* ptr to the end of the current arg */
	char *endbuf;                        /* ptr to the end of the 
											cmdline string */
	int is_bg;                           /* background job? */

	if (cmdline == NULL) {
		(void) fprintf(stderr, "Error: command line is NULL\n");
		return -1;
	}

	(void) strncpy(buf, cmdline, MAXLINE);
	endbuf = buf + strlen(buf);

	tok->infile = NULL;
	tok->outfile = NULL;

	/* Build the argv list */
	parsing_state = ST_NORMAL;
	tok->argc = 0;

	while (buf < endbuf) {
		/* Skip the white-spaces */
		buf += strspn (buf, delims);
		if (buf >= endbuf) break;
		/* Check for I/O redirection specifiers */
		if (*buf == '<') {
			if (tok->infile) {
				(void) fprintf(stderr, "Error: Ambiguous I/O redirection\n");
				return -1;
			}
			parsing_state |= ST_INFILE;
			buf++;
			continue;
		}
		if (*buf == '>') {
			if (tok->outfile) {
				(void) fprintf(stderr, "Error: Ambiguous I/O redirection\n");
				return -1;
			}
			parsing_state |= ST_OUTFILE;
			buf ++;
			continue;
		}

		if (*buf == '\'' || *buf == '\"') {
			/* Detect quoted tokens */
			buf++;
			next = strchr (buf, *(buf-1));
		} else {
			/* Find next delimiter */
			next = buf + strcspn (buf, delims);
		}

		if (next == NULL) {
			/* Returned by strchr(); this means that the closing
			   quote was not found. */
			(void) fprintf (stderr, "Error: unmatched %c.\n", *(buf-1));
			return -1;
		}

		/* Terminate the token */
		*next = '\0';

		/* Record the token as either the next argument or the 
		 * input/output file */
		switch (parsing_state) {
			case ST_NORMAL:
				tok->argv[tok->argc++] = buf;
				break;
			case ST_INFILE:
				tok->infile = buf;
				break;
			case ST_OUTFILE:
				tok->outfile = buf;
				break;
			default:
				(void) fprintf(stderr, "Error: Ambiguous I/O redirection\n");
				return -1;
		}
		parsing_state = ST_NORMAL;

		/* Check if argv is full */
		if (tok->argc >= MAXARGS-1) break;

		buf = next + 1;
	}

	if (parsing_state != ST_NORMAL) {
		(void)fprintf(stderr,"Error: must provide file name for redirection\n");
		return -1;
	}

	/* The argument list must end with a NULL pointer */
	tok->argv[tok->argc] = NULL;

	if (tok->argc == 0)  /* ignore blank line */
		return 1;

	if (!strcmp(tok->argv[0], "quit")) {                 /* quit command */
		tok->builtins = BUILTIN_QUIT;
	} else if (!strcmp(tok->argv[0], "jobs")) {          /* jobs command */
		tok->builtins = BUILTIN_JOBS;
	} else if (!strcmp(tok->argv[0], "bg")) {            /* bg command */
		tok->builtins = BUILTIN_BG;
	} else if (!strcmp(tok->argv[0], "fg")) {            /* fg command */
		tok->builtins = BUILTIN_FG;
	} else {
		tok->builtins = BUILTIN_NONE;
	}

	/* Should the job run in the background? */
	if ((is_bg = (*tok->argv[tok->argc-1] == '&')) != 0)
		tok->argv[--tok->argc] = NULL;

	return is_bg;
}


/*****************
 * Signal handlers
 *****************/

/* 
 * sigchld_handler - The kernel sends a SIGCHLD to the shell whenever
 *     a child job terminates (becomes a zombie), or stops because it
 *     received a SIGSTOP, SIGTSTP, SIGTTIN or SIGTTOU signal. The 
 *     handler reaps all available zombie children, but doesn't wait 
 *     for any other currently running children to terminate.  
 * Implementation - We use waitpid to reap all the zombie children processes 
 *     whenever the SIGCHLD signal is received by the parent due to child 
 *     termination. The WNOHANG option ensures that the waitpid does not 
 *	   wait for any other currently running children to terminate. In case of 
 *	   SIGCHLD being called due to a stopped process the WUNTRACED option helps 
 *	   us to return from the waitpid function without waiting.
 */
	void 
sigchld_handler(int sig) 
{
	int status,chldjid;
	pid_t pidchld;
	struct job_t *a;
	while((pidchld=waitpid(-1,&status,WNOHANG|WUNTRACED))>0)
	{
		/* WIFSIGNALED is used to check if SIGCHLD was received due to child
		 * terminating because of an uncaught signal, and if this returns 
		 * true the WTERMSIG is used to get the number of this signal.
		 */
		if((WIFSIGNALED(status)) && (WTERMSIG(status)))
		{
			printf("Job [%d] (%d) terminated by signal %d\n",
					pid2jid(pidchld),pidchld,WTERMSIG(status));
		}
		/* WIFSTOPPED is used to check if SIGCHLD was received due to child 
		 * stopping and if this returns to true WSTOPSIG gives the number of
		 * the signal that caused the child to stop.
		 */
		else if((WIFSTOPPED(status)) && (WSTOPSIG(status)))
		{	
			chldjid=pid2jid(pidchld);
			a=getjobpid(job_list,pidchld);
			printf("Job [%d] (%d) stopped by signal %d\n",chldjid,pidchld,
					WSTOPSIG(status));
			a->state=ST;
			return;
		}
		/* deletejob is called whenever SIGCHLD is received due to child 
		 * termination.
		 */
		deletejob(job_list,pidchld);
	}
	return;
}

/* 
 * sigint_handler - The kernel sends a SIGINT to the shell whenver the
 *    user types ctrl-c at the keyboard.  Catch it and send it along
 *    to the foreground job.
 * Implementation: Since sigint is sent only to foreground jobs, I first 
 *    check if there is a foreground job using the fgpid function, and if there
 *    is I send the entire group the sigint signal using the Kill function
 */
	void 
sigint_handler(int sig) 
{
	if((foreground=fgpid(job_list))>0)
	{
		Kill(-foreground,SIGINT);
		return;
	}
	else
		return;
}

/*
 * sigtstp_handler - The kernel sends a SIGTSTP to the shell whenever
 *     the user types ctrl-z at the keyboard. Catch it and suspend the
 *     foreground job by sending it a SIGTSTP.  
 * Implementation: Since sigtstp is sent only to foreground jobs, I first
 *	   check if there is a foreground job using the fgpid function, and if there
 *	   is I send the entire group the sigtstp signal using the Kill function
 */
	void 
sigtstp_handler(int sig) 
{
	if((foreground=fgpid(job_list))>0)
	{
		Kill(-foreground,SIGTSTP);
		return;
	}
	else
		return;
}

/*********************
 * End signal handlers
 *********************/

/***********************************************
 * Helper routines that manipulate the job list
 **********************************************/

/* clearjob - Clear the entries in a job struct */
void 
clearjob(struct job_t *job) {
	job->pid = 0;
	job->jid = 0;
	job->state = UNDEF;
	job->cmdline[0] = '\0';
}

/* initjobs - Initialize the job list */
void 
initjobs(struct job_t *job_list) {
	int i;

	for (i = 0; i < MAXJOBS; i++)
		clearjob(&job_list[i]);
}

/* maxjid - Returns largest allocated job ID */
	int 
maxjid(struct job_t *job_list) 
{
	int i;

	for (i = 0; i < MAXJOBS; i++)
		if (job_list[i].jid > max)
			max = job_list[i].jid;
	return max;
}

/* addjob - Add a job to the job list */
	int 
addjob(struct job_t *job_list, pid_t pid, int state, char *cmdline) 
{
	int i;

	if (pid < 1)
		return 0;

	for (i = 0; i < MAXJOBS; i++) {
		if (job_list[i].pid == 0) {
			job_list[i].pid = pid;
			job_list[i].state = state;
			job_list[i].jid = nextjid++;
			if (nextjid > MAXJOBS)
				nextjid = 1;
			strcpy(job_list[i].cmdline, cmdline);
			if(verbose){
				printf("Added job [%d] %d %s\n", job_list[i].jid, job_list[i].pid, job_list[i].cmdline);
			}
			return 1;
		}
	}
	printf("Tried to create too many jobs\n");
	return 0;
}

/* deletejob - Delete a job whose PID=pid from the job list */
	int 
deletejob(struct job_t *job_list, pid_t pid) 
{
	int i;

	if (pid < 1)
		return 0;

	for (i = 0; i < MAXJOBS; i++) {
		if (job_list[i].pid == pid) {
			clearjob(&job_list[i]);
			nextjid = maxjid(job_list)+1;
			return 1;
		}
	}
	return 0;
}

/* fgpid - Return PID of current foreground job, 0 if no such job */
pid_t 
fgpid(struct job_t *job_list) {
	int i;

	for (i = 0; i < MAXJOBS; i++)
		if (job_list[i].state == FG)
			return job_list[i].pid;
	return 0;
}

/* getjobpid  - Find a job (by PID) on the job list */
struct job_t 
*getjobpid(struct job_t *job_list, pid_t pid) {
	int i;

	if (pid < 1)
		return NULL;
	for (i = 0; i < MAXJOBS; i++)
		if (job_list[i].pid == pid)
			return &job_list[i];
	return NULL;
}

/* getjobjid  - Find a job (by JID) on the job list */
struct job_t *getjobjid(struct job_t *job_list, int jid) 
{
	int i;

	if (jid < 1)
		return NULL;
	for (i = 0; i < MAXJOBS; i++)
		if (job_list[i].jid == jid)
			return &job_list[i];
	return NULL;
}

/* pid2jid - Map process ID to job ID */
	int 
pid2jid(pid_t pid) 
{
	int i;

	if (pid < 1)
		return 0;
	for (i = 0; i < MAXJOBS; i++)
		if (job_list[i].pid == pid) {
			return job_list[i].jid;
		}
	return 0;
}

/* listjobs - Print the job list */
	void 
listjobs(struct job_t *job_list, int output_fd) 
{
	int i;
	char buf[MAXLINE];
	for (i = 0; i < MAXJOBS; i++) {
		memset(buf, '\0', MAXLINE);
		if (job_list[i].pid != 0) {
			sprintf(buf, "[%d] (%d) ", job_list[i].jid, job_list[i].pid);
			if(write(output_fd, buf, strlen(buf)) < 0) {
				fprintf(stderr, "Error writing to output file\n");
				exit(1);
			}
			memset(buf, '\0', MAXLINE);
			switch (job_list[i].state) {
				case BG:
					sprintf(buf, "Running    ");
					break;
				case FG:
					sprintf(buf, "Foreground ");
					break;
				case ST:
					sprintf(buf, "Stopped    ");
					break;
				default:
					sprintf(buf, "listjobs: Internal error: job[%d].state=%d ",
							i, job_list[i].state);
			}
			if(write(output_fd, buf, strlen(buf)) < 0) {
				fprintf(stderr, "Error writing to output file\n");
				exit(1);
			}
			memset(buf, '\0', MAXLINE);
			sprintf(buf, "%s\n", job_list[i].cmdline);
			if(write(output_fd, buf, strlen(buf)) < 0) {
				fprintf(stderr, "Error writing to output file\n");
				exit(1);
			}
		}
	}
	if(output_fd != STDOUT_FILENO)
		close(output_fd);
}

/*
 * usage - print a help message
 */
	void 
usage(void) 
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
	void 
unix_error(char *msg)
{
	fprintf(stdout, "%s: %s\n", msg, strerror(errno));
	exit(1);
}

/*
 * app_error - application-style error routine
 */
	void 
app_error(char *msg)
{
	fprintf(stdout, "%s\n", msg);
	exit(1);
}

/*
 * Signal - wrapper for the sigaction function
 */
	handler_t 
*Signal(int signum, handler_t *handler) 
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
	void 
sigquit_handler(int sig) 
{
	printf("Terminating after receipt of SIGQUIT signal\n");
	exit(1);
}

/*
 * Fork - wrapper for the fork function for error checking
 */
pid_t Fork(void)
{
	pid_t pid;
	if((pid = fork())<0)
		unix_error("Fork error");
	return pid;
}


/*
 * Kill - wrapper for the kill function for error checking
 */
	void 
Kill(pid_t pid, int sig)
{
	if((kill(pid,sig))<0)
		unix_error("Kill error");
	return;
}

/*
 * Sigaddset - wrapper for the sigaddset function for error checking
 */
	void
Sigaddset(sigset_t *set, int signum)
{
	if((sigaddset(set, signum))<0)
		unix_error("Sigaddset error");
}


/*
 * Sigdelset - wrapper for the sigdelset function for error checking
 */
	void 
Sigdelset(sigset_t *set, int signum)
{
	if((sigdelset(set, signum))<0)
		unix_error("Sigdelset error");
}


/*
 * Sigprocmask - wrapper for the sigprocmask function for error checking
 */
	void
Sigprocmask(int how, const sigset_t *set, sigset_t *oldset)
{
	if((sigprocmask(how,set,oldset))<0)
		unix_error("Sigprocmask error");
	return;
}

/*
 * Sigemptyset - wrapper for the sigemptyset function for error checking
 */
	void
Sigemptyset(sigset_t *set)
{
	if(sigemptyset(set)<0)
		unix_error("Sigemptyset error");
	return;
}

/*
 * Sigfillset - wrapper for the sigfillset function for error checking
 */
	void 
Sigfillset(sigset_t *set)
{
	if(sigfillset(set)<0)
		unix_error("Sigfillset error");
	return;
}

/*
 * Setpgid - wrapper for the setpgid function for error checking
 */
	void
Setpgid(pid_t pid, pid_t pgid)
{
	int rc;
	if((rc=setpgid(pid,pgid))<0)
		unix_error("Setpgid error");
	return;
}

/*
 * Open - wrapper for the open function for error checking
 */
int Open(const char *pathname, int flags, mode_t mode)
{
	int rc;
	if((rc = open(pathname , flags, mode)) < 0)
		unix_error("Open error");
	return rc;
}

/*
 * Close - wrapper for the close function for error checking
 */
void Close(int fd)
{
	int rc;
	if((rc = close(fd)) < 0)
		unix_error("Close error");
}

/*
 * Dup2 - wrapper for the dup2 function for error checking
 */
int Dup2(int fd1, int fd2)
{
	int rc;
	if((rc=dup2(fd1,fd2))<0)
		unix_error("Dup2 error");
	return rc;
}

/*
 * Execve - wrapper for the Execve function for error checking
 */
void Execve(const char *filename, char *const argv[], char *const envp[])
{
	if(execve(filename, argv, envp)<0)
		unix_error("Execve error");
}

