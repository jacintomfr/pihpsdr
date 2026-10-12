// MSi2500 clone vs Kenwood PTT: does streaming stall, and what restores it fastest?
// PTT by CAT (TX; / RX;) with the TS-570D in USB and no audio: no RF output.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <SoapySDR/Device.h>
#include <SoapySDR/Formats.h>
#include <SoapySDR/Errors.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

static SoapySDRDevice *sdr; static SoapySDRStream *rx;
static volatile int streaming = 0, reader_busy = 0, quit = 0;
static volatile long long total = 0;
static SOCKET cat = INVALID_SOCKET;

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

static void cat_send(const char *c) { send(cat, c, (int) strlen(c), 0); }
static int wait_readable(double secs) {
  fd_set f; FD_ZERO(&f); FD_SET(cat, &f);
  struct timeval tv = { (long) secs, (long) ((secs - (long) secs) * 1e6) };
  return select(0, &f, NULL, NULL, &tv) > 0;
}
static void cat_drain(void) { char d[256]; while (wait_readable(0.05)) { if (recv(cat, d, sizeof(d), 0) <= 0) break; } }
static void cat_query(const char *c, char *out, int n) {
  char b[256]; int len = 0; cat_drain(); cat_send(c); double t0 = now(); out[0] = 0;
  while (now() - t0 < 0.5 && wait_readable(0.1)) {
    int r = recv(cat, b + len, (int) sizeof(b) - 1 - len, 0);
    if (r <= 0) break;
    len += r; b[len] = 0; if (strchr(b, ';')) break;
  }
  b[len] = 0; snprintf(out, n, "%s", b);
}
static void ptt(int on) { cat_send(on ? "TX;" : "RX;"); }

static void *reader(void *a) {
  static float buf[2 * 32768]; void *bufs[] = { buf };
  while (!quit) {
    if (!streaming) { reader_busy = 0; usleep(5000); continue; }
    reader_busy = 1;
    int fl; long long tn;
    int r = SoapySDRDevice_readStream(sdr, rx, bufs, 32768, &fl, &tn, 100000);
    if (r > 0) total += r;
    reader_busy = 0;
  }
  return NULL;
}
static void pause_reader(void) { streaming = 0; while (reader_busy) usleep(1000); }

static int open_sdr(void) {
  SoapySDRKwargs a = {0}; SoapySDRKwargs_set(&a, "driver", "soapyMiri");
  sdr = SoapySDRDevice_make(&a); SoapySDRKwargs_clear(&a);
  if (!sdr) return -1;
  SoapySDRDevice_setSampleRate(sdr, SOAPY_SDR_RX, 0, 1536000.0);
  SoapySDRDevice_setFrequency(sdr, SOAPY_SDR_RX, 0, 73.10e6, NULL);
  SoapySDRDevice_setGain(sdr, SOAPY_SDR_RX, 0, 60.0);
  SoapySDRKwargs s = {0}; SoapySDRKwargs_set(&s, "transfer", "ISOC");
  rx = SoapySDRDevice_setupStream(sdr, SOAPY_SDR_RX, SOAPY_SDR_CF32, NULL, 0, &s); SoapySDRKwargs_clear(&s);
  if (!rx) return -1;
  SoapySDRDevice_activateStream(sdr, rx, 0, 0, 0);
  return 0;
}
static void close_sdr(void) {
  SoapySDRDevice_deactivateStream(sdr, rx, 0, 0); SoapySDRDevice_closeStream(sdr, rx); SoapySDRDevice_unmake(sdr);
}
// samples per second over a window
static double rate_over(double secs) { long long t0 = total; double a = now(); while (now() - a < secs) usleep(10000); return (total - t0) / (now() - a); }
// time until samples flow again (0.5 MS/s over 100 ms), or -1 after max s
static double time_to_flow(double max) {
  double a = now();
  while (now() - a < max) { long long t0 = total; usleep(100000); if (total - t0 > 50000) return now() - a; }
  return -1;
}

static void report(const char *what) { printf("%-44s %.3f MS/s\n", what, rate_over(0.5) / 1e6); fflush(stdout); }

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
  struct sockaddr_in sa = {0}; sa.sin_family = AF_INET; sa.sin_port = htons(7373); inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
  cat = socket(AF_INET, SOCK_STREAM, 0);
  if (connect(cat, (struct sockaddr *)&sa, sizeof(sa)) != 0) { printf("CAT connect failed\n"); return 1; }

  char r[256];
  ptt(0); usleep(300000); cat_query("MD;", r, sizeof(r)); printf("CAT: %s\n", r);
  if (!strstr(r, "MD2") && !strstr(r, "MD1")) { printf("Kenwood not in USB/LSB, abort\n"); ptt(0); return 1; }
  if (open_sdr()) { printf("SDR open failed\n"); return 1; }
  pthread_t th; pthread_create(&th, NULL, reader, NULL);
  streaming = 1;
  report("baseline RX");
  for (int round = 1; round <= 3; round++) {
    printf("\n--- round %d ---\n", round);
    // A: PTT while streaming, then see whether it stalls and recovers by itself
    ptt(1); report("A: during TX (streaming)"); usleep(500000); ptt(0); usleep(200000);
    report("A: after RX");
    double t = time_to_flow(1.5);
    if (t >= 0) { printf("A: flowing %.2f s after RX without help\n", t); }
    else {
      // S1: restart only the stream
      double a = now(); pause_reader();
      SoapySDRDevice_deactivateStream(sdr, rx, 0, 0); SoapySDRDevice_activateStream(sdr, rx, 0, 0, 0);
      streaming = 1; double t1 = time_to_flow(2.0);
      printf("S1 stream restart: %s (%.2f s)\n", t1 >= 0 ? "OK" : "still stalled", t1 >= 0 ? now() - a : -1.0);
      if (t1 < 0) {
        // S2: close and open the device
        a = now(); pause_reader(); close_sdr();
        if (open_sdr()) { printf("S2 reopen failed\n"); } else { streaming = 1; double t2 = time_to_flow(3.0); printf("S2 full reopen: %s (%.2f s)\n", t2 >= 0 ? "OK" : "still stalled", now() - a); }
      }
    }
    report("after recovery");
    // B: stream stopped before the PTT, started after RX
    pause_reader(); SoapySDRDevice_deactivateStream(sdr, rx, 0, 0);
    ptt(1); usleep(800000); ptt(0); usleep(200000);
    double a = now(); SoapySDRDevice_activateStream(sdr, rx, 0, 0, 0); streaming = 1;
    double tb = time_to_flow(2.0);
    printf("B: stream stopped over the PTT, restarted: %s (%.2f s)\n", tb >= 0 ? "OK" : "STALLED", tb >= 0 ? now() - a : -1.0);
    if (tb < 0) { pause_reader(); close_sdr(); if (open_sdr() == 0) { streaming = 1; time_to_flow(3.0); } }
    report("after B");
  }
  ptt(0); usleep(200000); cat_query("IF;", r, sizeof(r)); printf("\nfinal CAT IF: %s (pos 28 = TX flag)\n", r);
  pause_reader(); quit = 1; pthread_join(th, NULL); close_sdr(); closesocket(cat);
  return 0;
}
