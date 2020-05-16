#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h> 
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <sys/time.h>
#include <time.h>
#include <assert.h>
#include <math.h>
#include <stdbool.h> 

#include "common.h"
#include "packet.h"

#define STDIN_FD    0
#define RETRY  500 //milli second 

int check_buffer ();

/*
 * You ar required to change the implementation to support
 * window size greater than one.
 * In the currenlt implemenetation window size is one, hence we have
 * onlyt one send and receive packet
 */
tcp_packet *recvpkt;
tcp_packet *sndpkt;
tcp_packet *packet_buffer[256];
int seqno=0, start=0, count=0, end=0;
FILE *fp;
struct itimerval timer;
sigset_t sigmask;

void close_server(int signum){
	VLOG(INFO, "End Of File has been reached");
    fclose(fp);
    exit(0);
}

void start_timer()
{
    sigprocmask(SIG_UNBLOCK, &sigmask, NULL);
    setitimer(ITIMER_REAL, &timer, NULL);
}


void stop_timer()
{
    sigprocmask(SIG_BLOCK, &sigmask, NULL);
}


/*
 * init_timer: Initialize timeer
 * delay: delay in milli seconds
 * sig_handler: signal handler function for resending unacknoledge packets
 */
void init_timer(int delay, void (*sig_handler)(int)) 
{
    signal(SIGALRM, close_server);
    timer.it_interval.tv_sec = delay / 1000;    // sets an interval of the timer
    timer.it_interval.tv_usec = (delay % 1000) * 1000;  
    timer.it_value.tv_sec = delay / 1000;       // sets an initial value
    timer.it_value.tv_usec = (delay % 1000) * 1000;

    sigemptyset(&sigmask);
    sigaddset(&sigmask, SIGALRM);
}


int main(int argc, char **argv) {
    int sockfd; /* socket */
    int portno; /* port to listen on */
    int clientlen; /* byte size of client's address */
    struct sockaddr_in serveraddr; /* server's addr */
    struct sockaddr_in clientaddr; /* client addr */
    int optval; /* flag value for setsockopt */
    char buffer[MSS_SIZE];
    struct timeval tp;

    /* 
     * check command line arguments 
     */
    if (argc != 3) {
        fprintf(stderr, "usage: %s <port> FILE_RECVD\n", argv[0]);
        exit(1);
    }
    portno = atoi(argv[1]);

    fp  = fopen(argv[2], "w");
    if (fp == NULL) {
        error(argv[2]);
    }

    /* 
     * socket: create the parent socket 
     */
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) 
        error("ERROR opening socket");

    /* setsockopt: Handy debugging trick that lets 
     * us rerun the server immediately after we kill it; 
     * otherwise we have to wait about 20 secs. 
     * Eliminates "ERROR on binding: Address already in use" error. 
     */
    optval = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, 
            (const void *)&optval , sizeof(int));

    /*
     * build the server's Internet address
     */
    bzero((char *) &serveraddr, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);
    serveraddr.sin_port = htons((unsigned short)portno);

    /* 
     * bind: associate the parent socket with a port 
     */
    if (bind(sockfd, (struct sockaddr *) &serveraddr, 
                sizeof(serveraddr)) < 0) 
        error("ERROR on binding");
    /* 
     * main loop: wait for a datagram, then echo it
     */
    VLOG(DEBUG, "epoch time, bytes received, sequence number");

    clientlen = sizeof(clientaddr);

    init_timer(RETRY, close_server);
    while (1) {
        /*
         * recvfrom: receive a UDP datagram from a client
         */
        //VLOG(DEBUG, "waiting from server \n");
        if (end ==1 &&  count==0){
        	start_timer();
        }
        if (recvfrom(sockfd, buffer, MSS_SIZE, 0,
                (struct sockaddr *) &clientaddr, (socklen_t *)&clientlen) < 0) {
            error("ERROR in recvfrom");
        }
        recvpkt = (tcp_packet *) buffer;
        assert(get_data_size(recvpkt) <= DATA_SIZE);
        if (end ==1 &&  count==0){
        	stop_timer();
        };
        if (recvpkt->hdr.data_size == 0) {
        	end = 1;
        	goto LOOP;
        }
        /* 
         * sendto: ACK back to the client 
         */

        if (recvpkt->hdr.seqno == seqno){ //checks if the packet received is the expected one, if yes then write to file, if not ignore
        	while(1){
        	    gettimeofday(&tp, NULL);
	            VLOG(DEBUG, "%lu, %d, %d", tp.tv_sec, recvpkt->hdr.data_size, recvpkt->hdr.seqno);
	            fseek(fp, recvpkt->hdr.seqno, SEEK_SET);
	            fwrite(recvpkt->data, 1, recvpkt->hdr.data_size, fp);

	            seqno += recvpkt->hdr.data_size;//changes sequence number to the next expected value
	            LOOP: if (check_buffer()== 0){
	             	continue;
	             }else{
	             	break;
	             }
        	}
            
        }else if (recvpkt->hdr.seqno > seqno) {
        	packet_buffer[count] = recvpkt;
        	count++;
        	start = 1;
        }
        sndpkt = make_packet(0);
        sndpkt->hdr.ackno = seqno;
        sndpkt->hdr.ctr_flags = ACK;
        if (sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0, 
                (struct sockaddr *) &clientaddr, clientlen) < 0) {
            error("ERROR in sendto");
        } 
        printf("ACK: %d\n",seqno);      
    }
    return 0;
}

int check_buffer (){
	int num=0;
	if (start==0){
		return 1;
	}
	if (count == 0){
		return 1;
	}
	for (int i = 0; i<count; i++){
		if (packet_buffer[i]->hdr.seqno == seqno){
			recvpkt=packet_buffer[i];
			num = i+1;
			break;
		}
	}
	if (num == 0){
		return 1;
	}
	for(int i=num-1; i<count; i++){
		packet_buffer[i]=packet_buffer[i+1];
	}
	count--;
	return 0;
	
}
