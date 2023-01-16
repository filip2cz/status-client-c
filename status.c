
/* Socket and relative networking functions demonstration for ETC */

#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <string.h> /* memset() */
#include <arpa/inet.h>
#include <stdlib.h> /* exit() */
#include <netdb.h>  /* gethostbyname */
#include <sys/sysinfo.h>

int main(int argc, char **argv)
{

#define BUF_LEN 8192

  int sfd, afd, pid;
  struct sockaddr_in saddr;
  socklen_t acc_len;
  struct hostent *hen;
  extern int h_errno;

  if (argc != 3)
  {
    fprintf(stderr, "Usage: %s host port\n", argv[0]);
    return 1;
  }

  if ((sfd = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0)
  {
    perror("nd: socket()");
    return 5;
  }

  printf("Socket created, fd=%d.\n", sfd);

  memset(&saddr, 0, sizeof(struct sockaddr_in));

  saddr.sin_family = AF_INET;
  saddr.sin_port = htons(atoi(argv[2]));

  char buf[256];
  int len;

  if (!(hen = gethostbyname(argv[1])))
  {
    herror("nd: gethostbyname()");
    return 5;
  }

  memcpy(&saddr.sin_addr.s_addr, hen->h_addr, sizeof(saddr.sin_addr.s_addr));

  // connection

  if (connect(sfd, (struct sockaddr *)&saddr, sizeof(struct sockaddr_in)) < 0)
  {
    perror("nd: connect()");
    return 5;
  }

  len = read(sfd, &buf, 255);
  printf("%s", buf);

  sleep(2);

  // login
  write(sfd, "filip:testGeLFCSKKgo49\n", 24);
  sleep(1);

  // data
  size_t buffer_size = 1000;
  char *things = (char *)malloc(buffer_size * sizeof(char));
  while (1)
  {
    // sysinfo
    // everything with sysinfo is created helped with https://chat.openai.com/, thanks about it
    struct sysinfo si;
    sysinfo(&si);

    char ipv6[] = "false"; // nemá to ipv6
    int uptime = si.uptime;
    double load = si.loads[0] / 65536;
    int memory_total = si.totalram; // tohle není třeba už víc měnit
    int memory_used = si.totalram - si.freeram;
    int swap_total = 0; // swap to nemá
    int swap_used = 0;  // swap to nemá
    int hdd_total = 3761418;
    int hdd_used = 1161319;
    double cpu = sysconf(_SC_NPROCESSORS_ONLN);
    double network_rx = 0;
    double network_tx = 0;
    sprintf(things, "update {\"online6\": false, \"uptime\": %d, \"load\": %f, \"memory_total\": %d, \"memory_used\": %d, \"swap_total\": %d, \"swap_used\": %d, \"hdd_total\": %d, \"hdd_used\": %d, \"cpu\": %f, \"network_rx\": %f, \"network_tx\": %f }\n", uptime, load, memory_total, memory_used, swap_total, swap_used, hdd_total, hdd_used, cpu, network_rx, network_tx);
    sleep(2);
    write(sfd, things, strlen(things));
    memset(things, 0, buffer_size * sizeof(char));
  }

  sleep(5);

  shutdown(sfd, SHUT_RDWR);
  close(sfd);
}
