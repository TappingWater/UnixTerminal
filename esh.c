/*
 * esh - the 'pluggable' shell.
 *
 * Developed by Godmar Back for CS 3214 Fall 2009
 * Virginia Tech.
 */
#include <stdio.h>
#include <readline/readline.h>
#include <unistd.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/wait.h>
#include "esh-sys-utils.h"
#include "esh.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
//PATH=/opt/rh/devtoolset-7/root/usr/bin:$PATH
//from the help code list example
#define iterator(e, list) e = list_begin(list); e != list_end(list); e = list_next(e)

//these must be global, since the sigaction can only take certain kinds of params
//and the update child status must remove jobs from this list
struct list jobList;
struct termios *tty;
int jobID = 0;
pid_t shellPID;

static void
usage(char *progname)
{
    printf("Usage: %s -h\n"
        " -h            print this help\n"
        " -p  plugindir directory from which to load plug-ins\n",
        progname);

    exit(EXIT_SUCCESS);
}

/* Build a prompt by assembling fragments from loaded plugins that 
 * implement 'make_prompt.'
 *
 * This function demonstrates how to iterate over all loaded plugins.
 */
static char *build_prompt_from_plugins(void)
{
    char *prompt = NULL;
	struct list_elem *e;
    for (e = list_begin(&esh_plugin_list);
         e != list_end(&esh_plugin_list); e = list_next(e)) 
    {
        struct esh_plugin *plugin = list_entry(e, struct esh_plugin, elem);

        if (plugin->make_prompt == NULL)
            continue;

        /* append prompt fragment created by plug-in */
        char * p = plugin->make_prompt();
        if (prompt == NULL)
        {
            prompt = p;
        } 
		else
        {
            prompt = realloc(prompt, strlen(prompt) + strlen(p) + 1);
            strcat(prompt, p);
            free(p);
        }
    }

    /* default prompt */
    if (prompt == NULL)
        prompt = strdup("esh> ");

    return prompt;
}

/* The shell object plugins use.
 * Some methods are set to defaults.
 */
struct esh_shell shell =
{
    .build_prompt = build_prompt_from_plugins,
    .readline = readline,       /* GNU readline(3) */ 
    .parse_command_line = esh_parse_command_line /* Default parser */
};

/*
 * Assign ownership of ther terminal to process group
 * pgrp, restoring its terminal state if provided.
 *
 * Before printing a new prompt, the shell should
 * invoke this function with its own process group
 * id (obtained on startup via getpgrp()) and a
 * sane terminal state (obtained on startup via
 * esh_sys_tty_init()).
 */
static void give_terminal_to(pid_t pgrp, struct termios *pg_tty_state)
{
    esh_signal_block(SIGTTOU);
    int rc = tcsetpgrp(esh_sys_tty_getfd(), pgrp);
    if (rc == -1)
        esh_sys_fatal_error("tcsetpgrp: ");
    if (pg_tty_state)
        esh_sys_tty_restore(pg_tty_state);
    esh_signal_unblock(SIGTTOU);
}
/*
 * SIGCHLD handler.
 * Call waitpid() to learn about any child processes that
 * have exited or changed status (been stopped, needed the
 * terminal, etc.)
 * Just record the information by updating the job list
 * data structures.  Since the call may be spurious (e.g.
 * an already pending SIGCHLD is delivered even though
 * a foreground process was already reaped), ignore when
 * waitpid returns -1.
 * Use a loop with WNOHANG since only a single SIGCHLD
 * signal may be delivered for multiple children that have
 * exited.
 */
static void sigchld_handler(int sig, siginfo_t *info, void *_ctxt)
{
    pid_t child;
    int status;

    assert(sig == SIGCHLD);
	
    while ((child = waitpid(-1, &status, WUNTRACED|WNOHANG)) > 0)
    {
        child_status_change(child, status);
    }
}

/* Wait for all processes in this pipeline to complete, or for
 * the pipeline's process group to no longer be the foreground
 * process group.
 * You should call this function from a) where you wait for
 * jobs started without the &; and b) where you implement the
 * 'fg' command.
 *
 * Implement child_status_change such that it records the
 * information obtained from waitpid() for pid 'child.'
 * If a child has exited or terminated (but not stopped!)
 * it should be removed from the list of commands of its
 * pipeline data structure so that an empty list is obtained
 * if all processes that are part of a pipeline have
 * terminated.  If you use a different approach to keep
 * track of commands, adjust the code accordingly.
 */
static void wait_for_job(struct esh_pipeline *pipeline)
{
    assert(esh_signal_is_blocked(SIGCHLD));
	
	while (pipeline->status == FOREGROUND && !list_empty(&pipeline->commands)) 
	{
        int status;
        pid_t child = waitpid(-1, &status, WUNTRACED);
		if (child != -1)
		{
            child_status_change(child, status);
		}
    }
}

static void printCommand(pid_t pipeJobID)
{
    struct list_elem *jobElem = list_begin(&jobList);
    struct esh_pipeline *jobPipe = list_entry(jobElem, struct esh_pipeline, elem);
	struct esh_command *command = list_entry(list_begin(&jobPipe->commands), struct esh_command, elem);
	char **argv = command->argv;
	char* cmd = argv[0];
	argv++;

	if (*argv != NULL) 
	{
		printf("[%d] Stopped (%s %s)\n", pipeJobID, cmd, *argv);
	}
	else 
	{
		printf("[%d] Stopped (%s)\n", pipeJobID, cmd);
	}  
}

//this is based on the stopped and terminated jobs from the FAQ
//http://www.gnu.org/software/libc/manual/html_node/Stopped-and-Terminated-Jobs.html#Stopped-and-Terminated-Jobs
void child_status_change(pid_t child, int status)
{
	struct list_elem *removeElem;
	struct esh_pipeline *jobPipe;
	//if the child exited or terminated (not stopped), remove it from the list of commands
	//use job list because that is the list all of the pipes will be in, since they are
	//removed and put into it
	if(child > 0)
	{
		//need to remove elemens from pipeline, then if pipeline empty, remove pipeline from the job list
		//need to go through the job list to then pull the correct pid out of the 
		//pipeline to kill or stop or whatever
		//using the example 1 from the help session code to remove
		struct list_elem *jobElem;
		//bool needsRemoved = false;
		for(iterator(jobElem, &jobList))
		{
			jobPipe = list_entry(jobElem, struct esh_pipeline, elem);
			struct list_elem *pipeElem;
			for(iterator(pipeElem, &jobPipe->commands))
			{
				pid_t shellPID = getpgrp();
				struct esh_command *cmd = list_entry(pipeElem, struct esh_command, elem);
				//match the pids
				if(jobPipe->pgrp == child)
				{
					//printf("Got in if child");
					//printf("%d", child);
               		if (WIFSTOPPED(status))
		            {
	                    if (WSTOPSIG(status) == 22) 
	                    {
							esh_sys_tty_save(&jobPipe->saved_tty_state);
	                        jobPipe->status = STOPPED;
							give_terminal_to(shellPID, tty);
	                    }
	                    else
	                    {
							esh_sys_tty_save(&jobPipe->saved_tty_state);
	                        jobPipe->status = STOPPED;
							printCommand(jobPipe->jid);
							give_terminal_to(shellPID, tty);
	                    }
	                }
					else if(WIFEXITED(status))
					{
						//this is the sigchild we need to handle
						//set the element to be removed after the loop
						removeElem = &cmd->elem;
						list_remove(removeElem);
						//printf("%s", cmd->argv[0]);
						//needsRemoved = true;
						//printf("   exit ");
						//give_terminal_to(shellPID, tty);
					}
					else if(WTERMSIG(status) == 2)
					{
						//ctrl-c
						//according to slides, need to give back control to shell
						//needsRemoved = true;
						removeElem = &cmd->elem;
						list_remove(removeElem);
						printf("\n");
						give_terminal_to(shellPID, tty);
					}
				}  	
			}
			if(list_empty(&jobPipe->commands))
			{
				list_remove(&jobPipe->elem);
			}
		}
	}	
}

//if someone wants to add new built in commands, they can do so right here and increment the num of built in commands
const char* builtInCommands[] = {"jobs", "fg", "bg", "kill", "stop"};
int builtInCmd;
#define NUM_BUILTIN_CMDS 5

/**
 * This arguement simply takes a string arguement and returns a boolean value of whether or not
 * the commands is a built in command or not.
 **/
bool isBuiltIn(char **av)
{
	for (int i = 0; i < 5; i++) 
	{
		if (strncmp(av[0],builtInCommands[i], 5) == 0) 
		{
			builtInCmd = i;
			return true;
		}
	}
	return false;
}

/**
 * Helper function that checks the global variable for the job with the
 * given jobID and returns the appropriate pipeline
 **/
struct esh_pipeline* get_job(int jobID) 
{
	struct list_elem* jobElem = list_begin(&jobList);
	for (iterator(jobElem, &jobList)) 
	{
		struct esh_pipeline *jobPipe = list_entry(jobElem, struct esh_pipeline, elem);
		if (jobPipe->jid == jobID) 
		{
			return jobPipe;
		}
	}
	return NULL;
}

/**
 * Helper function that removes a job from the job list
 **/
static void remove_job(int jobID) 
{
	struct list_elem *removeElem;
	struct list_elem *jobElem = list_begin(&jobList);
	for (iterator(jobElem, &jobList)) 
	{
		struct esh_pipeline *jobPipe = list_entry(jobElem, struct esh_pipeline, elem);
		if (jobPipe->jid == jobID) 
		{
			removeElem = jobElem;
		}
	}
	list_remove(removeElem);
}	

int main(int ac, char *av[])
{
	int opt;
	list_init(&esh_plugin_list);
	//set up to job list and ID for later use	    
	list_init(&jobList);
	esh_signal_sethandler(SIGCHLD, sigchld_handler);
	//need this to give control of terminal back to shell
	shellPID = getpid();
	//printf("%d", shellPID);
	setpgid(0,0);
	/* Process command-line arguments. See getopt(3) */
	while ((opt = getopt(ac, av, "hp:")) > 0)
	{
		switch (opt)
		{
		case 'h':
			usage(av[0]);
		break;

		case 'p':
			esh_plugin_load_from_directory(optarg);
            	break;
        	}
    	}	
	
	esh_plugin_initialize(&shell);
	//need to initialize the terminal state
	tty = esh_sys_tty_init();
	/* Read/eval loop. */
	for (;;)
	{
        	/* Do not output a prompt unless shell's stdin is a terminal */
        	char * prompt = isatty(0) ? shell.build_prompt() : NULL;
        	char * cmdline = shell.readline(prompt);
        	free (prompt);

        	if (cmdline == NULL)  /* User typed EOF */
            		break;

        	struct esh_command_line * cline = shell.parse_command_line(cmdline);
        	free (cmdline);
        	if (cline == NULL)                  /* Error in command line */
            		continue;

        	if (list_empty(&cline->pipes))   /*User hit enter*/
        	{
            		esh_command_line_free(cline);
            		continue;
        	}
        	//our code
        	execCmd(cline, shellPID); 		
	}
	return 0;
}

enum {READ = 0, WRITE = 1};

/**
 * https://www.gnu.org/software/libc/manual/html_node/Descriptor-Flags.html
 * This function basically sets the close on exec function
 **/
static int set_cloexec_flag (int desc, int value)
{
  int oldflags = fcntl (desc, F_GETFD, 0);
  /* If reading the flags failed, return error indication now. */
  if (oldflags < 0)
    return oldflags;
  /* Set just the flag we want to set. */
  if (value != 0)
    oldflags |= FD_CLOEXEC;
  else
    oldflags &= ~FD_CLOEXEC;
  /* Store modified flag word in the descriptor. */
  return fcntl (desc, F_SETFD, oldflags);
}


void execCmd(struct esh_command_line *cline, pid_t shellPID)
{
/****If looping thru commands in different, dont use same list elem, make new one. warns in slides to do so ****/
	/*need to split cline into its components to get argv later
	 *need to check plugins 
     *handle built in commands
     *separate pipe from cline to add jobs to list (like in slides)
     *save the shell state
	 *create the pipes
	 *block sigchild while running the child process
	 *check bg job, else give terminal access
	 *check io redirect
	 *execute the command with error checking
	 *handle parent stuff
	 *make sure both ends of pipe are closed if not at begin
	 *set next pipes if not at end of list
	 *if at end of list close all pipes
	 *if it is a backgroind job, set current as bg
	 *if not, need to wait for job
	 *unblock sigchild
	 */
	
	//retrieving data out of cline
	struct esh_pipeline *eshPipe = list_entry(list_begin(&cline->pipes), struct esh_pipeline, elem);
	struct esh_command *cmds = list_entry(list_begin(&eshPipe->commands), struct esh_command, elem);
	char** argVector = cmds->argv;

	pid_t child;
	bool isBG;
	//first element of argv will be a built in command
	if(!isBuiltIn(cmds->argv))
	{
		//adding jobs to the list
		struct list_elem *eshElem; 
		eshElem = list_pop_front(&cline->pipes);
		list_push_back(&jobList, eshElem);
	
		//Save the current terminal state if we need to suspend a job
		esh_sys_tty_save(&eshPipe->saved_tty_state);
		//increment the job id as needed
		jobID = jobID+1;
		//if the list is empty, we know the only job is the one we are currently in
		if(list_empty(&jobList))
		{
			jobID = 1;
		}

		// if (strcmp(cmds->argv[0], "nano") || strcmp(cmds->argv[0], "vim") == 0) {
		// 	jobID = 1;
		// }

		//now the pipeline needs its job id set and process group id set for later
		eshPipe->jid = jobID;
		eshPipe->pgrp = -1;
		//Get the esh_pipeline struct type
		//Set process pipeline to true if the list size is greater than 1. i.e. has more than 1 command
		
		bool isPipeLine = (list_size(&eshPipe->commands) > 1);
		int pipeA[2];
		int pipeB[2];
		//loop through the list of commands and exec on them
		//Get the first pipe element
		struct list_elem *pipeElem;
		for(iterator(pipeElem, &eshPipe->commands))
		{
			struct esh_command *currCommand = list_entry(pipeElem, struct esh_command, elem);
			//When handling piping there are 3 major cases:
			//the commands within the pipe are either at: the beginnig, the middle or the end
			//Handling pipeline commands. If a command is a pipeline command, we need to create
			//additional pipes if the command being processed is not the last one.
			//Pipes for handling additional commands			
 			if (isPipeLine)
			{
				if((pipeElem == list_begin(&eshPipe->commands)))
				{
					//Create two pipes
					pipe(pipeA);
					pipe(pipeB);
					set_cloexec_flag(pipeA[0], 1);
					set_cloexec_flag(pipeB[0], 1);
					set_cloexec_flag(pipeA[1], 1);
					set_cloexec_flag(pipeB[1], 1);						
				}
			}

			//book, pg 779 has logic for blocking and unblocking
			//have parent block before child, so that add and delete run correctly
			esh_signal_block(SIGCHLD);

			isBG = eshPipe->bg_job;
			//this is based on the documentation from the FAQ
			//http://www.gnu.org/software/libc/manual/html_node/Launching-Jobs.html#Launching-Jobs
			//we are in child process if fork was returned 0
			if((child = fork()) == 0)
			{
				esh_signal_unblock(SIGCHLD);
				//this is to put the process into the process group
				//and give it the apropriate terminal access
				child = getpid();
				currCommand->pid = child;
				if(eshPipe->pgrp == -1)
					eshPipe->pgrp = child;
				if(setpgid(child, eshPipe->pgrp))
					esh_sys_fatal_error("Error setpgid child:\n");
				
				//If we are in a child process, we need to check the pipe to see which command we are at
				//so that
				if (isPipeLine)
				{
					
					//Since the beginning is already handled, we need to change file descriptors to
					//set up pipes for processes in the middle
					if (pipeElem != list_begin(&eshPipe->commands)) 
					{
						close(pipeA[1]);
						dup2(pipeA[0], 0);
						close(pipeA[0]);
					}
					//If the current commands is not the last command set up the pipes
					if (pipeElem != list_rbegin(&eshPipe->commands)) 
					{
						close(pipeB[0]);
						dup2(pipeB[1], 1);
						close(pipeB[1]);
					}
									
				}
				//check for IO redirect
                if (currCommand->iored_input != NULL)
                {
					//Create a input file descriptor to read the input
                	int inputFD = open(currCommand->iored_input, O_RDONLY);
                	if (dup2(inputFD, 0) < 0)
                	{
                			esh_sys_fatal_error("Error dup2\n");
                	}
                	close(inputFD);
					currCommand->iored_input = 0;
                }
                else if (currCommand->iored_output != NULL)
                {
                	//Create a output file descriptor for output
                   	int outFD;
                   	if (currCommand->append_to_output)
                   	{
						//this is if the file exits
                    	outFD = open(currCommand->iored_output, O_WRONLY | O_APPEND | O_CREAT, S_IRUSR | S_IRGRP | S_IWGRP | S_IWUSR);
                   	}	
                   	else
                   	{
						//if the file doesnt exist create it
                    	outFD = open(currCommand->iored_output, O_WRONLY | O_CREAT, S_IRUSR | S_IRGRP | S_IWGRP | S_IWUSR);
					}
                   	if (dup2(outFD, 1) < 0)
                   	{
                       	esh_sys_fatal_error("Error dup2\n");
                   	}
                	//close the file descriptor
                   	close(outFD);				
                }


				if(!isBG)
				{
					give_terminal_to(eshPipe->pgrp, tty);
					eshPipe->status = FOREGROUND;
					//fprintf(stderr, "CHILD %s: i'm process %d, my eshPGRP is %d, my group is %d, and group %d owns my terminal\n",
         				//currCommand->argv[0], getpid(), eshPipe->pgrp, getpgrp(), tcgetpgrp(open("/dev/tty", O_RDONLY)));
				}
				else
				{
					eshPipe->status = BACKGROUND;
				}

				if(execvp(currCommand->argv[0], currCommand->argv) < 0)
				{
					esh_sys_fatal_error("Could not find command");
				}
			}
			else if(child < 0)
			{
				esh_sys_fatal_error("Fork failed");	
			}
			else
			{
				//we are in parent process
				//update the child pgrp
				currCommand->pid = child;				
				if(eshPipe->pgrp == -1)
					eshPipe->pgrp = child;				
				if(setpgid(child, eshPipe->pgrp))
					esh_sys_fatal_error("Error setpgid parent:\n");				
				eshPipe->status = FOREGROUND;
				//fprintf(stderr, "Parent %s: i'm process %d, my eshPGRP is %d, my group is %d, and group %d owns my terminal\n",
         			//currCommand->argv[0], getpid(), eshPipe->pgrp, getpgrp(), tcgetpgrp(open("/dev/tty", O_RDONLY)));
				//We now have to set up the pipes in the parents end
 				if (isPipeLine) 
				{
					//Once we reach the final command all the pipes need to be closed
					if(pipeElem == list_rbegin(&eshPipe->commands))
					{
						close(pipeB[0]);
						close(pipeB[1]);
						close(pipeA[0]);
						close(pipeA[1]);
					}
					//We need to close the pipes to the beginning of the commands till we receive a response from other
					//commands in the pipeline
					if (pipeElem != list_begin(&eshPipe->commands)) 
					{
						close(pipeA[0]);
						close(pipeA[1]);
					}
					//If we are not at the end or the beginning and at the middle of the command we need to set up the pipes
					//to connect the commands
					if (pipeElem != list_rbegin(&eshPipe->commands)) 
					{
						pipeA[0] = pipeB[0];
						pipeA[1] = pipeB[1];
					}					
				}
			}
			
			if((eshPipe->bg_job) == true)
			{
				eshPipe->status = BACKGROUND;
				printf("[%d] %d\n", eshPipe->jid, eshPipe->pgrp);
			}
			//now that we are out of the parent process, before we go back to the shell, we need to
		}
		//1. wait for the job to terminate, if in the foreground
		if((eshPipe->bg_job) == false)
			wait_for_job(eshPipe);
		//2. give the terminal back to the shell
		give_terminal_to(shellPID, tty);
		//3. unblock sig child
		esh_signal_unblock(SIGCHLD);
	}
	//If it is a built in command
	else
	{
		//The built in command variable is set by the isBuiltIn() function
		switch (builtInCmd)
		{
			case 0 : ;//jobs				
				if (list_size(&jobList) > 0) 
				{
					struct list_elem *jobElem = list_begin(&jobList);
					for(iterator(jobElem, &jobList)) 
					{
						struct esh_pipeline *pipe = list_entry(jobElem, struct esh_pipeline, elem);
						struct  list_elem *commandElem = list_begin(&pipe->commands);
						struct esh_command *command = list_entry(commandElem, struct esh_command, elem);
						char** args = command->argv;
						//The number returned by the status function returns the status using indexing			
						char *status[] = {"Running", "Running", "Stopped", "Stopped"};
						printf("[%d] %s (%s", pipe->jid, status[pipe->status], *args);
						args++;
						while (*args)
						{
							printf(" %s", *args);
							args++;
						}
						if (pipe->bg_job) 
						{
							printf(" &");
						}
						printf(")\n");		
				};
			break;					

			case 1 : ; //fg
				esh_signal_block(SIGCHLD);
				char** foregroundArgs = argVector;				
				foregroundArgs++;
				//If there is a job arg
				if (foregroundArgs != NULL) 
				{
					int convertToForeground = atoi(*foregroundArgs);
					struct esh_pipeline *jobPipe = get_job(convertToForeground);
					struct  list_elem *commandElem = list_begin(&jobPipe->commands);
					struct esh_command *command = list_entry(commandElem, struct esh_command, elem);
					char** args = command->argv;
					
					while (*args) 
					{
						printf("%s ", *args);
						args++;
					}
					
					printf("\n");
					
					//Give terminal to job
					give_terminal_to(jobPipe->pgrp, tty);

					if (jobPipe->status == STOPPED)
					{						
						kill(jobPipe->pgrp, SIGCONT);
					}

					jobPipe->status = FOREGROUND;
					//Wait for the child to complete
					wait_for_job(jobPipe);
					esh_signal_unblock(SIGCHLD);
				}
				else
				{
					printf("Please enter fg command as follows: fg jobID");
				}
				break;
				
			case 2 : ;//bg
				char** backgroundArgs = argVector;
				backgroundArgs++;
				//If there is a job arg
				if (backgroundArgs != NULL) 
				{
					int backgroundJob = atoi(*backgroundArgs);
					struct esh_pipeline *jobPipe = get_job(backgroundJob);
					kill(jobPipe->pgrp, SIGCONT);
					jobPipe->status = BACKGROUND;									
				}
				else 
				{
					printf("Please enter the bg command as follows: bg jobID");
				}
				break;

			case 3 : ;//kill;
				//Get the args vector
				char** killCommand = argVector; 
				//Increment by one so that the pid now points to the id args
				killCommand++;
				if (*killCommand != NULL) 
				{
					//Convert the char pointer to the jobID
					int jobToKill = atoi(*killCommand);
					struct esh_pipeline * killPipe = get_job(jobToKill);
					//If the JobID is valid
					if (killPipe != NULL) 
					{
						int killPid = killPipe->pgrp;
						if (kill(killPid, SIGKILL) < 0) 
						{
							printf("Couldn't deliver SIGKILL to jobID : %d \n", jobToKill);
						}
						else 
						{
							jobID = jobID - 1;
							remove_job(jobToKill);
						}		
					}
					//If the jobID isnt entered
					else 
					{
						printf("Please enter a job ID to kill \n");
					}

				};
				break;
			case 4 : ;//stop
				char** stopCommand = argVector;
				stopCommand++;
				if (*stopCommand != NULL) 
				{
					int jobToStop = atoi(*stopCommand);
					struct esh_pipeline *jobPipe = get_job(jobToStop);
					if (jobPipe != NULL) 
					{
						int jobPid = jobPipe->pgrp;
						if(kill(jobPid, SIGSTOP) < 0) 
						{
							printf("Couldn't deliver SIGSTOP to jobID; %d \n", jobToStop);
						}						
					}
					else 
					{
						printf("The jobID: %d doesn't exitst", jobToStop);
					}
				}
				//If the jobID isnt entered
				else 
				{
					printf("Please enter a job ID to stop");
				}
				break;
			}
		
		}
	}
}
