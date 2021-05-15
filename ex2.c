/*
/*
 * ex2.c
 *
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <curl/curl.h>
#include <string.h>
#include <signal.h>

#include <semaphore.h>

#define HTTP_OK 200L
#define REQUEST_TIMEOUT_SECONDS 2L
#define SNAME "/mysem"
#define URL_OK 0
#define URL_UNKNOWN -1
#define URL_ERROR -2

#define MAX_PROCESSES 1024

const char URL_PREFIX[] = "http";

volatile typedef struct ResultStruct{
		double sum;
		int amount, unknown;
} ResultStruct ;


void usage() {
	fprintf(stderr, "usage:\n\t./ex2 num_of_processes FILENAME\n");
	exit(EXIT_FAILURE);
}

double check_url(const char *url) {
	CURL *curl;
	CURLcode res;
	double response_time = URL_UNKNOWN;

	curl = curl_easy_init();

	if(strncmp(url, URL_PREFIX, strlen(URL_PREFIX)) != 0){
		return URL_ERROR;
	}

	if(curl) {
		curl_easy_setopt(curl, CURLOPT_URL, url);
		curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
		curl_easy_setopt(curl, CURLOPT_TIMEOUT, REQUEST_TIMEOUT_SECONDS);
		curl_easy_setopt(curl, CURLOPT_NOBODY, 1L); /* do a HEAD request */

		res = curl_easy_perform(curl);
		if(res == CURLE_OK) {
			curl_easy_getinfo(curl, CURLINFO_NAMELOOKUP_TIME, &response_time);
		}

		curl_easy_cleanup(curl);

	}

	return response_time;

}

void serial_checker(const char *filename) {

	ResultStruct results = {0};

	FILE *toplist_file;
	char *line = NULL;
	size_t len = 0;
	ssize_t read;
	double res;
	int  rc, cd;

	ResultStruct *mmappedData;
	if((mmappedData = mmap(NULL, sizeof(ResultStruct), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0)) == MAP_FAILED)
	{
		perror("unable to create mapping");
		exit(EXIT_FAILURE);
	}
	mmappedData->sum = 0;
	mmappedData->amount = 0;
	mmappedData->unknown = 0;


	toplist_file = fopen(filename, "r");

	if (toplist_file == NULL) {
		exit(EXIT_FAILURE);
	}

	while ((read = getline(&line, &len, toplist_file)) != -1) {
		if (read == -1) {
			perror("unable to read line from file");
		}
		line[read-1] = '\0'; /* null-terminate the URL */
		if (URL_UNKNOWN == (res = check_url(line))) {
			mmappedData->unknown = mmappedData->unknown + 1;
		}
		else if(res == URL_ERROR){
			printf("Illegal url detected, exiting now\n");
			exit(0);
		}
		else {
			mmappedData->sum = mmappedData->sum + res;
			mmappedData->amount = mmappedData->amount + 1;
		}
	}

	free(line);
	fclose(toplist_file);



	if(mmappedData->amount > 0){
		printf("%.4f Average response time from %d sites, %d Unknown\n",
					mmappedData->sum / mmappedData->amount,
					mmappedData->amount,
					mmappedData->unknown);
	}
	else{
		printf("No Average response time from 0 sites, %d Unknown\n", results.unknown);
	}
	if((rc = munmap(mmappedData, sizeof(ResultStruct))) != 0)
	{
		perror("unable to unmapping");
		exit(EXIT_FAILURE);
	}
}

/**
 * @define - handle single worker that run on child process
 */
void worker_mmap_checker(int worker_id, int num_of_workers, const char *filename, ResultStruct *mmappedData) {
	/*
	 * TODO: this checker function should operate almost like serial_checker(), except:
	 * 1. Only processing a distinct subset of the lines (hint: think Modulo)
	 * 2. Writing the results back to the parent using the mmap (i.e. and not to the screen)
 	 * 3. If an URL_ERROR returned, all processes (parent and children) should exit immediately and an error message should be printed (as in 'serial_checker')
	 */

	ResultStruct results = {0};

	double res;
	FILE *toplist_file;
	char *line = NULL;
	ssize_t read;
	int line_number = 0, rc, cd;
	size_t len = 0;
	// TODO
	// open file for read only
	toplist_file = fopen(filename, "r");

	// validate file open successfully
	if (toplist_file == NULL) {
		exit(EXIT_FAILURE);
	}
	
	//use the named semaphore
	sem_t *sem = sem_open(SNAME, 0);
	
	// go over all the lines
	while ((read = getline(&line, &len, toplist_file)) != 0) {

		// TODO
		//liad
		if (read == -1) {
			perror("unable to read line from file");
			exit(EXIT_FAILURE);
		}
		if(line_number % num_of_workers == worker_id){
			line[read-1] = '\0'; /* null-terminate the URL */
			if (URL_UNKNOWN == (res = check_url(line))) {
				results.unknown = results.unknown + 1;
			}
			else if(res == URL_ERROR){
				printf("Illegal url detected, exiting now\n");
				kill(getpgrp(), SIGKILL);
				exit(EXIT_FAILURE);
			}
			else {
				results.sum = results.sum + res;
				results.amount = results.amount + 1;
			}
		}
		//liad

	}
	
	sem_wait(sem); //use semaphore to write in the critical section 
	// TODO write the result to mapping
	//liad
	mmappedData->sum = mmappedData->sum + results.sum;
	mmappedData->amount = mmappedData->amount + results.amount;
	mmappedData->unknown = mmappedData->unknown + results.unknown;
	//liad

	msync(mmappedData, sizeof(ResultStruct), MS_SYNC);
	sem_post(sem);

	// close the resources
	free(line);
	fclose(toplist_file);

}

/**
 * Handle separate the work between process and merge the results
 */
void parallel_mmap_checker(int num_of_processes, const char *filename) {
	ResultStruct *mmappedData;
	ResultStruct results = {0};
	int worker_id, fd, cd, rc;
	// TODO initialize  mapping
	// liad
	if((mmappedData = mmap(NULL, sizeof(ResultStruct), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0)) == MAP_FAILED)
	{
		perror("unable to create mapping");
		exit(EXIT_FAILURE);
	}
	mmappedData->sum = 0;
	mmappedData->amount = 0;
	mmappedData->unknown = 0;
	//liad

	//initailize semaphore for the childern to write in the same file
	sem_t *sem = sem_open(SNAME, O_CREAT, 0644, 1);

	// Start num_of_processes new workers
	for (worker_id = 0; worker_id  < num_of_processes; ++worker_id ) {

		// TODO - fork the children and call worker_mmap_checker.
		// Possible implementation: Let worker_mmap_checker on which rows to perform work (from file).
		// each worker is working in different process
		//liad
		int workerFork = fork();
		if (workerFork < 0 ){
			perror("fork() failed – handle error");
			exit(EXIT_FAILURE);
		}
		if (workerFork == 0){
			worker_mmap_checker(worker_id, num_of_processes, filename);
			exit(EXIT_SUCCESS);
		}
		//liad

	}

	// TODO
	// pending all child process
	//liad
	int padPar;
	int status = 0;
	while((padPar = wait(&status)) > 0 );
	//liad

	// TODO
	// get results
	//liad
	results.sum = mmappedData->sum ;
	results.amount = mmappedData->amount;
	results.unknown = mmappedData->unknown;
	//liad

	// print the total results
	if(results.amount > 0){
		printf("%.4f Average response time from %d sites, %d Unknown\n",
						results.sum / results.amount,
						results.amount,
						results.unknown);
	}
	else{
		printf("No Average response time from 0 sites, %d Unknown\n", results.unknown);
	}
}


void worker_pipe_checker(int worker_id, int num_of_workers, const char *filename, int pipe_write_fd) {
	/*
	 * TODO: this checker function should operate almost like serial_checker(), except:
	 * 1. Only processing a distinct subset of the lines (hint: think Modulo)
	 * 2. Writing the results back to the parent using the pipe_write_fd (i.e. and not to the screen)
	 * 3. If an URL_ERROR returned, all processes (parent and children) should exit immediatly and an error message should be printed (as in 'serial_checker')
	 */

	ResultStruct results = {0};

	double res;
	FILE *toplist_file;
	char *line = NULL;
	size_t len = 0;
	ssize_t read;
	int line_number = 0;

	// TODO

	// go over all the lines
	while ((read = getline(&line, &len, toplist_file)) != -1) {

		// TODO

	}

	// TODO

}

/**
 * Handle separate the work between process and merge the results
 */
void parallel_pipe_checker(int num_of_processes, const char *filename) {
	int worker_id;
	int pipefd[2];

	ResultStruct results = {0};
	ResultStruct results_buffer = {0};

	// initialize  pipe
	pipe(pipefd);

	// Start num_of_processes new workers
	for (worker_id = 0; worker_id  < num_of_processes; ++worker_id ) {

		// TODO - fork the children and call worker_pipe_checker.
		// Possible implementation: Let worker_pipe_checker on which rows to perform work (from file).
		
	}

	// TODO

	for (worker_id = 0; worker_id  < num_of_processes; ++worker_id ) {
		
		// TODO - sum the results
	
	}

	// print the total results
	if(results.amount > 0){
		printf("%.4f Average response time from %d sites, %d Unknown\n",
						results.sum / results.amount,
						results.amount,
						results.unknown);
	}
	else{
		printf("No Average response time from 0 sites, %d Unknown\n", results.unknown);
	}


}


int main(int argc, char **argv) {
	int pipe_flag = 0;
		if(argc == 4 && !strcmp(argv[3],"-f"))
		{
			pipe_flag = 1;
		}
		if (argc != 3 && !pipe_flag) {
			usage();
		} else if (atoi(argv[1]) == 1) {
			serial_checker(argv[2]);
		} else
		{
			if(pipe_flag)
			{
				parallel_pipe_checker(atoi(argv[1]), argv[2]);
			}
			else
			{
				parallel_mmap_checker(atoi(argv[1]), argv[2]);
			}
		}
		return EXIT_SUCCESS;

}
