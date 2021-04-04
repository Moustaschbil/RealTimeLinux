/* 
 * Xenomai example for GPIO (POSIX skin + mmap()) + divisor (XDDP)
 */

#include <sys/time.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdlib.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <getopt.h>
#include <time.h>
#include <pthread.h>
#include <fcntl.h>
#include <rtdm/ipc.h>
#include <ctype.h>

#define BCM2710_PERI_BASE   0x3F000000
#define GPIO_BASE           (BCM2710_PERI_BASE + 0x200000) /* GPIO controler */

#define NSEC_PER_SEC    1000000000
#define XDDP_PORT 0
pthread_t rt;

/* the struct timespec consists of nanoseconds
 * and seconds. if the nanoseconds are getting
 * bigger than 1000000000 (= 1 second) the
 * variable containing seconds has to be
 * incremented and the nanoseconds decremented
 * by 1000000000.
 */
static inline void tsnorm(struct timespec *ts)
{
   while (ts->tv_nsec >= NSEC_PER_SEC) {
      ts->tv_nsec -= NSEC_PER_SEC;
      ts->tv_sec++;
   }
}

// I/O access
volatile unsigned int *gpio;

// GPIO setup macros. Always use INP_GPIO(x) before using OUT_GPIO(x) 
// or SET_GPIO_ALT(x,y)
#define INP_GPIO(g) *(gpio+((g)/10)) &= ~(7<<(((g)%10)*3))
#define OUT_GPIO(g) *(gpio+((g)/10)) |=  (1<<(((g)%10)*3))
#define SET_GPIO_ALT(g,a) *(gpio+(((g)/10))) |= (((a)<=3?(a)+4:(a)==4?3:2)<<(((g)%10)*3))

#define GPIO_SET *(gpio+7)  // sets   bits which are 1 ignores bits which are 0
#define GPIO_CLR *(gpio+10) // clears bits which are 1 ignores bits which are 0
// For GPIO# >= 32 (RPi B+)
#define GPIO_SET_EXT *(gpio+8)  // sets   bits which are 1 ignores bits which are 0
#define GPIO_CLR_EXT *(gpio+11) // clears bits which are 1 ignores bits which are 0

void gpio_set (int g)
{
  if (g >= 32)
    GPIO_SET_EXT = (1 << (g % 32));
  else
    GPIO_SET = (1 << g);
}

void gpio_clr (int g)
{
  if (g >= 32)
    GPIO_CLR_EXT = (1 << (g % 32));
  else
    GPIO_CLR = (1 << g);
}

int  mem_fd;
pthread_t thid_square;

#define PERIOD          100000000 // 100 ms
int nibl = 0;
int gpio_nr = 4; // default is led

unsigned long period_ns = 0;
int loop_prt = 100;             /* print every 100 loops: 5 s */ 
unsigned int test_loops = 0;    /* outer loop count */

//
// Set up a memory regions to access GPIO
//
void setup_io()
{
  /* open /dev/mem */
  if ((mem_fd = open("/dev/mem", O_RDWR|O_SYNC) ) < 0) {
    perror ("open");
    exit(1);
  }

  /* mmap GPIO -> should use Linux version (__real_) */
  gpio = (unsigned int *)__real_mmap(
			  NULL,             //Any adddress in our space will do
			  4096,       //Map length
			  PROT_READ|PROT_WRITE,// Enable reading & writting to mapped memory
			  MAP_SHARED,       //Shared with other processes
			  mem_fd,           //File to map
			  GPIO_BASE         //Offset to GPIO peripheral
			  );

  close(mem_fd); // No need to keep mem_fd open after mmap

  if (gpio == MAP_FAILED) {
    perror ("mmap");
    exit(1);
  }
}


/* Thread function*/
void *thread_square (void *dummy)
{
  struct timespec ts, tr;
  time_t t = 0, told = 0, jitter;
  static time_t jitter_max = 0, jitter_avg = 0;

  clock_gettime(CLOCK_REALTIME, &ts);
  ts.tv_sec++;

  /* Main loop */
  for (;;)
    {
      // Absolute wait
      if (clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME, &ts, NULL)) {
	perror ("clock_nanosleep");
	exit (1);
      }

      /* Write to GPIO */
      if (test_loops % 2)
	gpio_set (gpio_nr);
      else
	gpio_clr (gpio_nr);

      // wait 'period' ns
      ts.tv_nsec += period_ns;
      tsnorm (&ts);
      
      /* old_time <-- current_time */
      told = t;

      /* get current time */
      clock_gettime (CLOCK_REALTIME, &tr);
      t = (tr.tv_sec * 1000000000) + tr.tv_nsec;    

      // Calculate jitter + display
      jitter = abs(t - told - period_ns);
      jitter_avg += jitter;
      if (test_loops && (jitter > jitter_max))
	jitter_max = jitter;
  
      if (test_loops && !(test_loops % loop_prt)) {
	jitter_avg /= loop_prt;
	printf ("Loop= %d sec= %ld nsec= %ld delta= %ld ns jitter cur= %ld ns avg= %ld ns max= %ld ns\n", test_loops,  tr.tv_sec, tr.tv_nsec, t-told, jitter, jitter_avg, jitter_max);
	jitter_avg = 0;
      }

      test_loops++;
    }
}

void cleanup_upon_sig(int sig __attribute__((unused)))
{
  pthread_cancel (thid_square);
  pthread_join (thid_square, NULL);
  exit(0);
}

void usage (char *s)
{
  fprintf (stderr, "Usage: %s [-p period (ns)] [-g gpio#]\n", s);
  exit (1);
}

static void *realtime_thread(void *arg)
{
  struct sockaddr_ipc saddr;
  int ret, s=0;
  char buf[128];
  size_t poolsz;
        
  s = socket(AF_RTIPC, SOCK_DGRAM, IPCPROTO_XDDP);
  if (s < 0) {
    perror("socket");
    exit(EXIT_FAILURE);
  }
  poolsz = 16384; /* bytes */
  ret = setsockopt(s, SOL_XDDP, XDDP_POOLSZ,
		   &poolsz, sizeof(poolsz));
  if (ret)
    perror("setsockopt");
  
  memset(&saddr, 0, sizeof(saddr));
  saddr.sipc_family = AF_RTIPC;
  saddr.sipc_port = XDDP_PORT;
  ret = bind(s, (struct sockaddr *)&saddr, sizeof(saddr));
  if (ret)
    perror("bind");
                        
  while(1) {
    printf ("Waiting for XDDP data...\n");

    memset (buf, 0, sizeof(buf));
    ret = recvfrom(s, buf, sizeof(buf), 0, NULL, 0);
    if (ret <= 0)
      perror("recvfrom");
    else{
      printf("received <%s> (%d)\n", buf, ret);
      period_ns/=atoi(buf);
    }
  }
}

int main (int ac, char **av)
{
  int err, c;
  char *progname = (char*)basename(av[0]);

  struct sched_param rtparam = { .sched_priority = 99 };
  pthread_attr_t rtattr;
  struct sched_param param_square = { .sched_priority = 99 };
  pthread_attr_t thattr_square;

  period_ns = PERIOD; /* ns */

  signal(SIGINT, cleanup_upon_sig);
  signal(SIGTERM, cleanup_upon_sig);
  signal(SIGHUP, cleanup_upon_sig);
  signal(SIGALRM, cleanup_upon_sig);

  while ((c = getopt (ac, av, "hp:g:")) != -1) {
    switch (c)
      {
      case 'g':
	gpio_nr = atoi(optarg);
	break;
	
      case 'p':
	period_ns = atoi(optarg);
	break;

      case 'h':
	usage(progname);
	
      default:
	break;
      }
  }

  // Display every 2 sec
  loop_prt = 2000000000 / period_ns;
  
  printf ("Using GPIO %d and period %ld ns\n", gpio_nr, period_ns);

  // Set up gpi pointer for direct register access
  setup_io();

  // Set GPIO  as output
  OUT_GPIO(gpio_nr);

  // Thread attributes
  pthread_attr_init(&rtattr);
  pthread_attr_init(&thattr_square);

  // Priority, set priority to 99
  pthread_attr_setdetachstate(&rtattr, PTHREAD_CREATE_JOINABLE);
  pthread_attr_setinheritsched(&rtattr, PTHREAD_EXPLICIT_SCHED);
  pthread_attr_setschedpolicy(&rtattr, SCHED_FIFO);
  pthread_attr_setschedparam(&rtattr, &rtparam);

  pthread_attr_setinheritsched(&thattr_square, PTHREAD_EXPLICIT_SCHED);
  pthread_attr_setschedpolicy(&thattr_square, SCHED_FIFO);
  pthread_attr_setschedparam(&thattr_square, &param_square);

  if(pthread_create(&rt, &rtattr, &realtime_thread, NULL))
  {
    perror("pthread_create");
    exit (1);
  }

  // Create thread 
  err = pthread_create(&thid_square, &thattr_square, &thread_square, NULL);

  if (err)
    {
      fprintf(stderr,"square: failed to create square thread, code %d\n",err);
      return 0;
    }

  pause();

  return 0;
}
