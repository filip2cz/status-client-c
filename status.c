/*
  This is fork of "Socket and relative networking functions demonstration for ETC" from Pavel Troller.
  Thanks Pavel Troller for networking part of this code.
  Thanks Mikuláš Jan Honys (https://github.com/ur-fault) for help with code and github actions.
  Thanks to OpenAI for help with variables, everything with sysinfo and statvfs.

*/

#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <string.h> /* memset() */
#include <arpa/inet.h>
#include <stdlib.h> /* exit() */
#include <netdb.h>  /* gethostbyname */
#include <sys/sysinfo.h>
#include <sys/statvfs.h>

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

  char user[64];
  char password[64];

  // open file for reading
  FILE* fp = fopen("login.txt", "r");
  if (fp == NULL) {
    perror("Failed to open file");
    exit(EXIT_FAILURE);
  }

  // read username and password from file
  if (fscanf(fp, "%s %s", user, password) != 2) {
    fprintf(stderr, "Failed to read username and password from file\n");
    exit(EXIT_FAILURE);
  }

  // close file
  fclose(fp);

  // calculate size of login buffer
  size_t login_size = strlen(user) + strlen(password) + 2;

  // allocate login buffer
  char* login = malloc(login_size);
  if (login == NULL) {
    perror("Failed to allocate memory");
    exit(EXIT_FAILURE);
  }

  // format login string and send to server
  sprintf(login, "%s:%s\n", user, password);
  write(sfd, login, strlen(login));
  sleep(1);

  // free dynamically allocated memory
  free(login);

  // data
  size_t buffer_size = 1000;
  char *things = (char *)malloc(buffer_size * sizeof(char));
  while (1)
  {
    struct sysinfo si;
    struct statvfs fs_info;
    sysinfo(&si);

    if (statvfs("/", &fs_info) == -1) {
      perror("Failed to get filesystem information");
      return 1;
    }

    // Available space in bytes
    long long avail_space = fs_info.f_frsize * fs_info.f_bfree;

    // Total size of filesystem in bytes
    long long total_size = fs_info.f_frsize * fs_info.f_blocks;

    // Used space in bytes
    long long used_space = total_size - avail_space;

    // Swap
    long long total_swap = si.totalswap;
    long long free_swap = si.freeswap;
    long long used_swap = total_swap - free_swap;

    // load
    double load;
    FILE *fp = fopen("/proc/loadavg", "r");
    if (fp == NULL) {
        perror("Failed to open /proc/loadavg");
        return 1;
    }
    int ret = fscanf(fp, "%lf", &load);
    if (ret != 1) {
        perror("Failed to read load from /proc/loadavg");
        return 1;
    }
    fclose(fp);

    // CPU

    char line[128];
    FILE* file = fopen("/proc/stat", "r");
    fgets(line, sizeof(line), file);
    fclose(file);
    char* cpu_info = strchr(line, ' ') + 1;
    unsigned long long int user, nice, system, idle, iowait, irq, softirq, steal, guest, guest_nice;
    sscanf(cpu_info, "%llu %llu %llu %llu %llu %llu %llu %llu %llu %llu", &user, &nice, &system, &idle, &iowait, &irq, &softirq, &steal, &guest, &guest_nice);
    unsigned long long int idle_total = idle + iowait;
    unsigned long long int total = user + nice + system + idle_total + irq + softirq + steal;
    static unsigned long long int prev_idle = 0;
    static unsigned long long int prev_total = 0;
    unsigned long long int diff_idle = idle_total - prev_idle;
    unsigned long long int diff_total = total - prev_total;
    unsigned int diff_usage = (1000 * (diff_total - diff_idle) / diff_total + 5) / 10;
    prev_idle = idle_total;
    prev_total = total;

    char ipv6[] = "false"; // coming soon
    int uptime = si.uptime;
    //double load = si.loads[0] / 1000;
    int memory_total = si.totalram / 1024;
    int memory_used = (si.totalram - si.freeram) / 1024;
    int swap_total = (int) (total_swap / 1024);
    int swap_used = (int) (used_swap / 1024);
    int hdd_total = (int) (total_size / 1048576);
    int hdd_used = (int) (used_space / 1048576);
    double cpu = diff_usage;
    double network_rx = 0; // coming soon
    double network_tx = 0; // coming soon
    sprintf(things, "update {\"online6\": false, \"uptime\": %d, \"load\": %f, \"memory_total\": %d, \"memory_used\": %d, \"swap_total\": %d, \"swap_used\": %d, \"hdd_total\": %d, \"hdd_used\": %d, \"cpu\": %f, \"network_rx\": %f, \"network_tx\": %f }\n", uptime, load, memory_total, memory_used, swap_total, swap_used, hdd_total, hdd_used, cpu, network_rx, network_tx);
    sleep(2);
    write(sfd, things, strlen(things));
    memset(things, 0, buffer_size * sizeof(char));
  }

  sleep(5);

  shutdown(sfd, SHUT_RDWR);
  close(sfd);
}
