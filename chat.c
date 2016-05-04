#include <sys/types.h>
#include <sys/wait.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <signal.h>

#include "chat.h"

int loggedInPos;
shared_mem_t* shm;
int fd[2];

int getinput(char* input, FILE* stream) {
	int i = 0, c;
	while((c = fgetc(stream)) != EOF) {
		if(c == '\n') {
			input[i] = '\0';
			return 1;
		}
		input[i++] = c;
	}
	return 0;
}

void printusers(shared_mem_t* shm) {
	int i;
	for(i = 0; i < MAX_USERS; ++i)
		if(shm->users[i].uid != -1)
			printf("pos: %d uid: %d  name: %s\n", i, shm->users[i].uid, shm->users[i].name);
}

void incSem(int semid) {
	struct sembuf sb;
	sb.sem_op=1;
	sb.sem_flg=0;
	sb.sem_num=0;
	semop(semid, &sb, 1);
}

void decSem(int semid) {
	struct sembuf sb;
	sb.sem_op=-1;
	sb.sem_flg=0;
	sb.sem_num=0;
	semop(semid, &sb, 1);
}

void logout(shared_mem_t* shm, int* loggedInPos, int writepipe) {
	char logoutMessage[100];
	sprintf(logoutMessage, "User logged out: %d, %s", shm->users[*loggedInPos].uid, shm->users[*loggedInPos].name);
	write(writepipe, logoutMessage, strlen(logoutMessage));
	shm->users[*loggedInPos].uid = -1;
}

int login(shared_mem_t* shm, int* loggedInPos, int writepipe) {
	int i;
	for(i = 0; i < MAX_USERS; ++i)
		if(shm->users[i].uid == -1) {
			//enter into user table
			shm->users[i].uid = getuid();
			char name[MAX_NAME_LEN];
			getlogin_r(name, MAX_NAME_LEN);
			strcpy(shm->users[i].name, name);
			*loggedInPos = i;
			//send login message
			char loginMessage[100];
			sprintf(loginMessage, "New user logged on: %d, %s", shm->users[*loggedInPos].uid, shm->users[*loggedInPos].name);
			write(writepipe, loginMessage, strlen(loginMessage));
			return 0;
		}
	return -1;
}

void intHandler(int sig) {
	logout(shm, &loggedInPos, fd[1]);
	_exit(0);
}

int main() {
	int i;
	int shmid;
	int semid;
	char input[100];
	int readPos = 0;

	signal(SIGINT, intHandler);

	shmid = shmget(MEM_KEY, SHM_SIZE, 0);
	if(shmid < 0)
	{
		printf("%s\n", "creating memory");
		shmid = shmget(MEM_KEY, 16384, IPC_CREAT|0666);
		if(shmid<0) {
			printf("%s\n", "error creating memory");
			_exit(1);
		}
		//initialize
		shared_mem_t* temp = shmat(shmid, 0, 0);
		for(i = 0; i < 16; ++i)
			temp->users[i].uid = -1;
		temp->pos=0;
	}
	shm = (shared_mem_t*) shmat(shmid, 0, 0);

	semid = semget(SEM_KEY, 1, 0);
	if(semid == -1) {
		semid = semget(SEM_KEY, 1, 0666 | IPC_CREAT);
		//set semaphore to 1 to start
		if(semctl(semid, 0, GETVAL) == 0)
			incSem(semid);
	}

	/*
	if(semctl(semid, 0, GETVAL) == 0)
		incSem(semid);
	*/

	pipe(fd);
	fcntl(fd[0], F_SETFL, O_NONBLOCK);

	printusers(shm);
	if(login(shm, &loggedInPos, fd[1]) != 0) {
		printf("%s\n", "login failed");
		return 1;
	}
	readPos = shm->pos;

	int cpid;
	cpid = fork();
	if(cpid == 0) {
		while(getinput(input, stdin)) {
			if(strcmp(input, "exit") == 0)
				break;
			else if(strcmp(input, "users") == 0)
				printusers(shm);
			else
				write(fd[1], input, strlen(input));
		}
		logout(shm, &loggedInPos, fd[1]);
	}
	else {
		int status;
		while(waitpid(cpid, &status, WNOHANG) == 0) {
			sleep(1);
			decSem(semid);
			//print new messages
			while(readPos != shm->pos) {
				putchar(shm->messages[readPos]);
				readPos = (readPos + 1) % BUFFER_LEN;
			}
			//read any input from pipe and print
			int bytes = read(fd[0], input, sizeof(input));
			for(i = 0; i < bytes; ++i) {
				shm->messages[shm->pos] = input[i];
				shm->pos = (shm->pos + 1) % BUFFER_LEN;
			}
			//if there was any input, put newline and advance readPos
			if(i) {
				shm->messages[shm->pos] = '\n';
				shm->pos = (shm->pos + 1) % BUFFER_LEN;
				readPos = shm->pos;
			}
			incSem(semid);
		}
	}
}
