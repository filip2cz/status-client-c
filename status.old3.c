
/* Socket and relative networking functions demonstration for ETC */

#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <string.h> /* memset() */
#include <arpa/inet.h>
#include <stdlib.h> /* exit() */
#include <netdb.h>  /* gethostbyname */


int main (int argc, char ** argv) {

#define BUF_LEN 8192

  int sfd, afd, pid;
  struct sockaddr_in saddr;
  socklen_t acc_len;
  struct hostent *hen;
  extern int h_errno;

  if (argc!=3) {
    fprintf(stderr,"Usage: %s host port\n", argv[0]);
    return 1;
  }

  if ((sfd = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0) {
    perror("nd: socket()");
    return 5;
  }

  printf("Socket created, fd=%d.\n", sfd);

  memset(&saddr, 0, sizeof(struct sockaddr_in)); 

  saddr.sin_family=AF_INET;
  saddr.sin_port=htons(atoi(argv[2]));

  char buf[256];
   int len;

   if (!(hen=gethostbyname(argv[1]))) {
     herror("nd: gethostbyname()");
     return 5;
   }

   memcpy(&saddr.sin_addr.s_addr, hen->h_addr, sizeof(saddr.sin_addr.s_addr));

	//připojení

   if (connect(sfd, (struct sockaddr *) &saddr, sizeof(struct sockaddr_in)) <0) {
     perror("nd: connect()");
     return 5;
   }

   len=read(sfd, &buf, 255);
   printf("%s", buf);
    
   sleep(2);

   write(sfd,"filip:testGeLFCSKKgo49\n",24);
   sleep(1);

   while(1){
	char ipv6[] = "false";
	int uptime = 229;
	char things[] = "update {\"online6\": false, \"uptime\": %d, \"load\": 0.93, \"memory_total\": 12200460, \"memory_used\": 2499944, \"swap_total\": 12201980, \"swap_used\": 0, \"hdd_total\": 3761418, \"hdd_used\": 1161319, \"cpu\": 19.0, \"network_rx\": 0, \"network_tx\": 0 }\n", uptime;
	sleep(2);
   	write(sfd, things,strlen(things));
   }

   while(1){	   
    write(sfd,"1\n",2);
    sleep(2);
    printf("ok\n");
   }
   
   sleep(5);

   shutdown(sfd, SHUT_RDWR);
   close(sfd);
}

