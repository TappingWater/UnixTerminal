# Summary
This shell will interpret the input as the name of a program to be executed, along with arguments to be passed to it. In this case, the shell will fork a new child process and executes the program in the context of the child. Normally, the shell will wait for a command to complete before reading the next command from the user. Such programs are said to run as “foreground” jobs. If the user appends an ampersand ‘&’ to a command, the command is started in the “background” and the shell will return to the prompt immediately.

# Functionality
Built In Command Functionality. (jobs, fg, bg, kill, stop) </br>
Job Control </br>
Singal Handling (CTRL+Z (SIGSTP), CTRL+C (SIGINT) </br>
Pipes and I/O Redirection.

# Installation
Run make in the src directory.</br>
To start the terminal. type ./esh in command to start the program. </br>



