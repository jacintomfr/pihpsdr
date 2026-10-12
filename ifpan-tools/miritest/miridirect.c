#include <mirisdr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

static long long bytes = 0, calls = 0;
static void cb(unsigned char *buf, uint32_t len, void *ctx) { bytes += len; calls++; }
static void *stopper(void *arg) {
  mirisdr_dev_t *dev = arg; sleep(5); mirisdr_cancel_async(dev); return NULL;
}
int main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  const char *mode = argc > 1 ? argv[1] : "ISOC";
  printf("devices: %u, name: %s\n", mirisdr_get_device_count(), mirisdr_get_device_name(0));
  mirisdr_dev_t *dev = NULL;
  if (mirisdr_open(&dev, 0) < 0) { printf("open failed\n"); return 1; }
  // same order as libmirisdr's own miri_sdr example
  printf("rate %d\n", mirisdr_set_sample_rate(dev, 1536000));
  printf("freq %d\n", mirisdr_set_center_freq(dev, 73050000));
  printf("format %d\n", mirisdr_set_sample_format(dev, "AUTO"));
  printf("transfer %s %d\n", mode, mirisdr_set_transfer(dev, mode));
  mirisdr_set_if_freq(dev, 0);
  mirisdr_set_bandwidth(dev, 1536000);
  mirisdr_set_tuner_gain_mode(dev, 1);
  mirisdr_set_tuner_gain(dev, 40);
  printf("reset %d\n", mirisdr_reset_buffer(dev));
  pthread_t t; pthread_create(&t, NULL, stopper, dev);
  struct timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a);
  int r = mirisdr_read_async(dev, cb, NULL, 0, 0);
  clock_gettime(CLOCK_MONOTONIC, &b);
  double el = (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) * 1e-9;
  pthread_join(t, NULL);
  printf("read_async=%d: %lld bytes in %lld callbacks over %.2f s = %.3f MB/s (1536k x 4 B = 6.144)\n",
         r, bytes, calls, el, bytes / el / 1e6);
  mirisdr_close(dev);
  return 0;
}
