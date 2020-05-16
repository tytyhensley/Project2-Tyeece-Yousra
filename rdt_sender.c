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

#include"packet.h"
#include"common.h"

#define STDIN_FD    0
#define RETRY  120 //milli second 
#define ssh 32


int slowstart();


int next_seqno=0, send_base=0, exp_seqno, dup_ack, sockfd, serverlen, slow_cnt=0, slow_srt=0, cwnd=1, count=0, end=0;
struct sockaddr_in serveraddr;
struct itimerval timer;
char buffer[DATA_SIZE]; 
FILE *fp;
tcp_packet *sndpkt;
tcp_packet *recvpkt;
tcp_packet *window_pkt[ssh]; // a pointer array of all apckets in the current window
sigset_t sigmask;

void resend_packets(int sig)
{
    if (sig == SIGALRM)
    {
        //Resend all packets range between 
        //sendBase and nextSeqNum
        VLOG(INFO, "Timeout happend");
        for (int i = 0; i<count; i++){
            if(sendto(sockfd, window_pkt[i], TCP_HDR_SIZE + get_data_size(window_pkt[i]), 0, 
                        ( const struct sockaddr *)&serveraddr, serverlen) < 0)
            {
                error("sendto");
            }
         VLOG(DEBUG, "Sending packet %d to %s",window_pkt[i]->hdr.seqno, inet_ntoa(serveraddr.sin_addr));
        }
    }
    if(sig == 0){
         VLOG(INFO, "Duplicate Ack happend");
        for (int i = 0; i<count; i++){
            if(sendto(sockfd, window_pkt[i], TCP_HDR_SIZE + get_data_size(window_pkt[i]), 0, 
                        ( const struct sockaddr *)&serveraddr, serverlen) < 0)
            {
                error("sendto");
            }
         VLOG(DEBUG, "Sending packet %d to %s",window_pkt[i]->hdr.seqno, inet_ntoa(serveraddr.sin_addr));
        }
    } 
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
    signal(SIGALRM, resend_packets);
    timer.it_interval.tv_sec = delay / 1000;    // sets an interval of the timer
    timer.it_interval.tv_usec = (delay % 1000) * 1000;  
    timer.it_value.tv_sec = delay / 1000;       // sets an initial value
    timer.it_value.tv_usec = (delay % 1000) * 1000;

    sigemptyset(&sigmask);
    sigaddset(&sigmask, SIGALRM);
}


int main (int argc, char **argv)
{
    int portno;
    char *hostname;
    //int start = 0;

    /* check command line arguments */
    if (argc != 4) {
        fprintf(stderr,"usage: %s <hostname> <port> <FILE>\n", argv[0]);
        exit(0);
    }
    hostname = argv[1];
    portno = atoi(argv[2]);
    fp = fopen(argv[3], "r");
    if (fp == NULL) {
        error(argv[3]);
    }

    /* socket: create the socket */
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) 
        error("ERROR opening socket");


    /* initialize server server details */
    bzero((char *) &serveraddr, sizeof(serveraddr));
    serverlen = sizeof(serveraddr);

    /* covert host into network byte order */
    if (inet_aton(hostname, &serveraddr.sin_addr) == 0) {
        fprintf(stderr,"ERROR, invalid host %s\n", hostname);
        exit(0);
    }

    /* build the server's Internet address */
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_port = htons(portno);

    assert(MSS_SIZE - TCP_HDR_SIZE > 0);

    //Stop and wait protocol

    init_timer(RETRY, resend_packets);
    do
    {
        if (slowstart() == 0){//ends if the file is completely sent in the slow start phase
            return 0;
        }

    }while(cwnd != ssh);
    printf("time for avoidance\n\n");
    return 0;

}

int slowstart() {// initiates slow start at the beginning of a cycle 
    int len, num=0;
    do{
        len = fread(buffer, 1, DATA_SIZE, fp);
        if ( len <= 0)//checks if we've  reached the end of the file and breaks out of the for loop
        {
            end = end+1;//accumulates to keep track of when the EOF is reached and when all the packets in window have been acked.
            VLOG(INFO, "End Of File has been reached");
            sndpkt = make_packet(0);
            sendto(sockfd, sndpkt, TCP_HDR_SIZE,  0,
                    (const struct sockaddr *)&serveraddr, serverlen);
            break;
        }

        if (end > 0){//allows  for the iteration of the window size ot to grow pass the EOF 
            count = cwnd-1;
        }else{
            count=cwnd;
        }

        send_base = next_seqno;
        next_seqno = send_base + len;//creates the next sequence number 
        sndpkt = make_packet(len);
        memcpy(sndpkt->data, buffer, len);
        sndpkt->hdr.seqno = send_base;

        window_pkt[slow_cnt] = sndpkt;//adds the current send packet to the ith position in the window

        VLOG(DEBUG, "Sending packet %d to %s",window_pkt[slow_cnt]->hdr.seqno, inet_ntoa(serveraddr.sin_addr));
        if(sendto(sockfd, sndpkt, TCP_HDR_SIZE + get_data_size(sndpkt), 0, 
                    ( const struct sockaddr *)&serveraddr, serverlen) < 0)
        {
            error("sendto");
        }
        slow_cnt+=1;
    }while(slow_cnt < cwnd);

    if (end >= count){//once all pakets in window have been ACKed and EOF reached, end program
        return 0;
    }

    start_timer();
    if(recvfrom(sockfd, buffer, MSS_SIZE, 0,
        (struct sockaddr *) &serveraddr, (socklen_t *)&serverlen) < 0)
    {
        error("recvfrom");
    }
    recvpkt = (tcp_packet *)buffer;
    assert(get_data_size(recvpkt) <= DATA_SIZE);
    stop_timer();

    exp_seqno = window_pkt[0]->hdr.seqno + window_pkt[0]->hdr.data_size;//gets the expected sequence number for the ACK to be received

    if(exp_seqno <= recvpkt->hdr.ackno){//allows for cumulative ACKS
        int i;
        for (i = 0; i < count; i++){
            if ((window_pkt[i]->hdr.seqno + window_pkt[i]->hdr.data_size) == recvpkt->hdr.ackno){//determines which of the packets is being ACKed
                num = i+1;
            }
            window_pkt[i-num] =  window_pkt[i];//makes window shift to the packets after the last ACKed
        }
        slow_srt = (i-num);//points to the new position in window where the next packets will go 
        slow_cnt = slow_srt;//starts a ccount or the next number of packets and makes it not exceed the current window size
        if (end < 1){//does not increase window size once EOF is reached
            cwnd++;
        }
        dup_ack=0;//resets the duplicate ACK counter once the ACk has been recieved
    }else{
        dup_ack++;//accounts for duplicate ACKS
        if (dup_ack == 3){
            resend_packets(0);
        }
    }
    return 1;
}


