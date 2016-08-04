/* 
 * Main source code file for lsh shell program
 */

/* Defining that we are running on a POSIX system */
#define _POSIX_C_SOURCE 1

/* Includes */
#include <stdio.h>
#include <stdlib.h>
#include <readline/readline.h>
#include <readline/history.h>
#include "parse.h"
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <string.h>
#include <signal.h>
#include <sys/types.h>


/* Function declarations */
void PrintCommand(int, Command *);
void PrintPgm(Pgm *);
void stripwhite(char *);
void childhandler(int signal_nr);
void interrupthandler(int signal_nr);
void sigtstp_handler(int signal_nr);
char* concat(char *s1, char *s2);
void runPgm(Pgm *nextPgm);

/* When non-zero, this global means the user is done using this program. */
int done = 0;


/* Variables for handling processes 
 * Needs to be global since we use them
 * in many functions. Could maybe 
 * pass them to the functions instead?
 */
pid_t pid;
int status;

/*
 * Name: main
 *
 * Description: Gets the ball rolling...
 *
 */
int main(void)
{
  Command cmd;
  int n;

  /* 
   * Struct for background processes, listen for SIGCHILD signals 
   * then call childhandler() in order to terminate them.
   * This is done in order to avoid zombies. 
   */
  struct sigaction sigchld_action;
  memset (&sigchld_action, 0, sizeof (sigchld_action));
  sigchld_action.sa_handler = &childhandler;
  /* sigaction(SIGCHLD, &sigchld_action, NULL); */

  /*
   * Struct for interrupting processes without
   * exiting our lsh shell. 
   * Handles SIGINT signals.
   */
   struct sigaction sigint_action;
   memset(&sigint_action, 0, sizeof (sigint_action));
   sigint_action.sa_handler = &interrupthandler;
   sigaction(SIGINT, &sigint_action, NULL);

  /* 
   * Saving STDOUT and STDIN in case we need to restore
   * them later in the program.
   */  
  int saved_stdout;
  saved_stdout = dup(STDOUT_FILENO);
  int saved_stdin;
  saved_stdin = dup(STDIN_FILENO);


  /* Main loop of the program */
  while(!done){

    char *line;
    line = readline("> ");
    if(!line){
      /* Encountered EOF at top level */
      done = 1;
    }
    else{
      /*
       * Remove leading and trailing whitespace from the line
       * Then, if there is anything left, add it to the history list
       * and execute it.
       */
      stripwhite(line);

      if(*line){
        add_history(line);

        n = parse(line, &cmd);
        
        /* Print the command for debugging
         * purposes. Not used in "release".
         */
        //PrintCommand(n, &cmd);


        /* Does the user want to exit shell? */
        if(strcmp(cmd.pgm->pgmlist[0],"exit") == 0){
          return 0;
        }

        /* Does the user want to change directory? */
        else if(strcmp(cmd.pgm->pgmlist[0],"cd") == 0){
          if(cmd.pgm->pgmlist[1] != NULL){
            if((chdir(cmd.pgm->pgmlist[1])) == -1){
              perror("cd failed");
            }
          }
          else{
            /* If the user does not specify a path, go to HOME */
            chdir(getenv("HOME"));
          }
        }

        /* Spawning a new process in order to handle commands */
        else{
          /* 
           * Check if the rstdin or rstdout is set and then 
           * redirect the input/output to that file. 
           * Should maybe be moved to the child process instead 
           * since we always need to restore them at the moment?
           */
          if(cmd.rstdin != NULL){
            FILE * input;
            input = fopen(cmd.rstdin,"r");
            dup2(fileno(input), STDIN_FILENO);  
            fclose(input);
          }

          if(cmd.rstdout != NULL){
            FILE * output;
            output = fopen(cmd.rstdout,"w+");
            dup2(fileno(output), STDOUT_FILENO);
            fclose(output);
          }


          /* Forking child process to handle commands */
          if((pid = fork()) < 0){
            perror("Failed to fork()");
            exit(EXIT_FAILURE);
          }

          else if(pid == 0){
            if(cmd.background){
              setpgid(pid, pid);
            }
            
            /* Initiate the recursive function */
            Pgm *nextPgm;
            nextPgm = cmd.pgm;
            runPgm(nextPgm);
          }

          /* This is the parent process. */
          else{            
            if(cmd.background){
              printf("Spawned process %d in background\n", pid);
              /* signal(SIGCHLD, childhandler) */
              sigaction(SIGCHLD, &sigchld_action, NULL);
              /* setpgid(pid, pid); */
            }
            else{
              /* Wait for the process to complete */
              if (waitpid (pid, &status, 0) != pid){
                status = -1;
              }
            }
            /* Restore terminal in case we piped stdout or stdin somewhere */
            dup2(saved_stdin, STDIN_FILENO);
            dup2(saved_stdout, STDOUT_FILENO);
          }
        }
      }
    }
    if(line){
      free(line);
    }
  }
  return 0;
}


/*
 * Name: runPgm 
 * Arguments: nextPgm in order
 * Description: Executes a chain of Pgms
 * recursively.
 */
void runPgm(Pgm *nextPgm){
  /* 
   * Base case of function
   * This happens when we reached the end
   * of the chain since there is no more 
   * commands to run after this. 
   */
  if(nextPgm->next == NULL){
    if(execvp(nextPgm->pgmlist[0], nextPgm->pgmlist) < 0){
      char * err_msg = concat("Execution of command failed: ", nextPgm->pgmlist[0]);
      perror(err_msg);
      free(err_msg);
      exit(EXIT_FAILURE);
    }
  }

  /* Rest of the cases where we
   * will spawn a child that 
   * executes command for us
   * before we execute our
   * command.
   */
  else{
    /* Create variables for piping input/output */ 
    int fds[2];
    if(pipe(fds) != 0){
      perror("Pipe error");
    }

    /* Should maybe create a define for these?
     * instead of creating ints and referencing
     * them to the fds.
     */
    int read_fd = fds[0];
    int write_fd = fds[1];

    /* Fork a new process */
    if ((pid = fork()) < 0){
      perror("Failed to fork()");
      exit(EXIT_FAILURE);
    }

    /* We are a child, duplicate FDs 
     * and run the function again
     */
    else if(pid == 0){
      dup2(write_fd, STDOUT_FILENO);
      close(write_fd);


      nextPgm = nextPgm->next;
      runPgm(nextPgm);

    }


    /*
     * We are a parent, wait
     * for the child before 
     * executing. 
     */

    else{
      dup2(read_fd, STDIN_FILENO);
      close(read_fd);
      close(write_fd);

      /* 
       * We need to wait for our child to execute
       * first since we want it's output
       */
      if(waitpid(pid, &status, 0) != pid) {
        status = -1; // error
      }

      if(execvp(nextPgm->pgmlist[0], nextPgm->pgmlist) < 0){
        char * err_msg = concat("Execution of command failed: ", nextPgm->pgmlist[0]);
        perror(err_msg);
        free(err_msg);
        exit(EXIT_FAILURE);
      }
    }
  }
}

/* 
 * Name: childhandler
 * Arguments: Number of signal (ID)
 * Description: Handle termination 
 * of children in order to avoid zombies.
 */
void childhandler(int signal_nr){
  int childstatus;
  pid_t childpid;
  /* childpid = wait(&childstatus); */
  

  while(waitpid((pid_t)-1, 0, WNOHANG) > 0){

  } /*
  if (childpid == -1){
    char pidstring[6]; // Assuming no longer PIDs than 6
    sprintf(pidstring, "%d", childpid);
    char * err_msg = concat("Could not shutdown child:", pidstring);
    perror(err_msg);
    free(err_msg);
    exit(EXIT_FAILURE);
  }
  else{
    /* This is only written for debugging purposes */
    //printf("Child with pid: %d has terminated with status: %d\n", childpid, childstatus );
}

/*
 * Name: interrupthandler
 * Arguments: Number of signal (ID)
 * Description: Handles CTRL+C 
 * commands issued in Shell.
 */
void interrupthandler(int signal_nr){
  //printf("Do nothing...");
  signal(SIGINT, SIG_IGN);
}

void sigtstp_handler(int signal_nr){
  //printf("Do nothing...");
}

/*
 * Name: concat
 * Arguments: Pointer to string s1 and s2
 * Returns: Result
 * Description: Concatenetes two strings
 * and returns them.
 */
char* concat(char *s1, char *s2){
  char *result = malloc(strlen(s1)+strlen(s2)+1);
  if (result == NULL){
    perror("Function concat failed!");
    exit(EXIT_FAILURE);
  }
  else{
    strcpy(result, s1);
    strcat(result, s2);
    return result;
  }
}

/*
 * Name: PrintCommand
 * Arguments: 
 * Description: Prints a Command structure as returned by parse on stdout.
 */
void PrintCommand(int n, Command *cmd){
  printf("Parse returned %d:\n", n);
  printf("   stdin : %s\n", cmd->rstdin  ? cmd->rstdin  : "<none>" );
  printf("   stdout: %s\n", cmd->rstdout ? cmd->rstdout : "<none>" );
  printf("   bg    : %s\n", cmd->background ? "yes" : "no");
  PrintPgm(cmd->pgm);
}

/*
 * Name: PrintPgm
 * Arguments: Pgm pointer
 * Description: Prints a list of Pgm:s
 */
void PrintPgm (Pgm *p)
{
  if (p == NULL) {
    return;
  }
  else {
    char **pl = p->pgmlist;
    /* 
     * The list is in reversed order so print
     * it reversed to get right
     */
    PrintPgm(p->next);
    printf("    [ ");
    while (*pl) {
      printf("%s ", *pl++);
    }
    printf("]\n");
  }
}

/*
 * Name: stripwhite
 * Arguments: A string in which to remove whitespace
 * Description: Strip whitespace from the start and end of STRING.
 */
void stripwhite(char *string){
  register int i = 0;
  /* Replaced whitespace with isspace, since whitespace is not supported. */
  while(isspace(string[i])){
    i++;
  }
  if(i){
    strcpy (string, string + i);
  }
  i = strlen(string) - 1;
  while(i> 0 && isspace (string[i])){
    i--;
  }
  string[++i] = '\0';
}