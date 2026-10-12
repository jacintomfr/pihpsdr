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
static volatile double lv_sum = 0, lv_peak = 0, lv_si = 0, lv_sq = 0; static volatile long long lv_n = 0;
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
    if (r > 0) { total += r; for (int k = 0; k < r; k++) { double i0 = buf[2*k], q0 = buf[2*k+1]; lv_si += i0; lv_sq += q0; lv_sum += i0 * i0 + q0 * q0; } lv_n += r; }
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

static int stream_restart_flow(void) {   // S1: stop / start the stream only
  pause_reader(); SoapySDRDevice_deactivateStream(sdr, rx, 0, 0); SoapySDRDevice_activateStream(sdr, rx, 0, 0, 0);
  streaming = 1; return time_to_flow(2.0) >= 0;
}
static void full_reopen(void) { pause_reader(); close_sdr(); if (open_sdr() == 0) { streaming = 1; time_to_flow(3.0); } }
static void recover(const char *tag) {
  double a = now();
  if (stream_restart_flow()) { printf("   %s: stream restart OK after %.2f s\n", tag, now() - a); return; }
  full_reopen(); printf("   %s: needed a full reopen, %.2f s\n", tag, now() - a);
}

#include <math.h>
static void level(double secs, double *rms_db, double *peak_db) {
  lv_sum = 0; lv_peak = 0; lv_si = 0; lv_sq = 0; lv_n = 0; usleep((useconds_t)(secs * 1e6));
  double mi = lv_n ? lv_si / lv_n : 0, mq = lv_n ? lv_sq / lv_n : 0;
  double ac = lv_n ? lv_sum / lv_n - mi * mi - mq * mq : 0; double rms = ac > 0 ? sqrt(ac) : 0;
  lv_peak = sqrt(mi * mi + mq * mq);   /* reported as "peak": the DC level */
  *rms_db = rms > 0 ? 20 * log10(rms) : -200; *peak_db = lv_peak > 0 ? 20 * log10(lv_peak) : -200;
}
int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
  struct sockaddr_in sa = {0}; sa.sin_family = AF_INET; sa.sin_port = htons(7373); inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
  cat = socket(AF_INET, SOCK_STREAM, 0);
  if (connect(cat, (struct sockaddr *)&sa, sizeof(sa)) != 0) { printf("CAT connect failed\n"); return 1; }
  char md0[64], pc0[64], r[256];
  cat_query("MD;", md0, sizeof(md0)); printf("start mode %s\n", md0);
  if (!strstr(md0, "MD")) { printf("no CAT answer, abort\n"); return 1; }
  cat_send("MD5;"); usleep(400000);
  cat_query("PC;", pc0, sizeof(pc0)); cat_send("PC005;"); usleep(300000); cat_query("PC;", r, sizeof(r)); printf("AM power %s (was %s)\n", r, pc0);
  if (!strstr(r, "PC005")) { printf("5 W not set, abort\n"); goto restore; }
  if (open_sdr()) { printf("SDR open failed\n"); goto restore; }
  SoapySDRDevice_setGain(sdr, SOAPY_SDR_RX, 0, 30.0);   // modest gain, room for a strong TX signal
  pthread_t th; pthread_create(&th, NULL, reader, NULL);
  streaming = 1; usleep(500000);
  for (int i = 1; i <= 3; i++) {
    double a, b, c, d, e, f;
    level(1.0, &a, &b);
    ptt(1); usleep(400000); level(1.4, &c, &d); ptt(0); usleep(400000);
    level(1.0, &e, &f);
    printf("round %d (gain 30):  RX rms %6.1f dBFS DC %6.1f | TX rms %6.1f DC %6.1f | RX again rms %6.1f DC %6.1f | rate %.2f MS/s\n",
           i, a, b, c, d, e, f, rate_over(0.3) / 1e6);
  }
  pause_reader(); quit = 1; pthread_join(th, NULL); close_sdr();
restore:
  ptt(0); usleep(400000);
  { char cmd[32]; int w0 = atoi(pc0 + 2); if (w0 < 5) w0 = 25; snprintf(cmd, sizeof(cmd), "PC%03d;", w0); cat_send(cmd); usleep(300000); }
  { char m = (strlen(md0) >= 3) ? md0[2] : '2'; char cmd[8]; snprintf(cmd, sizeof(cmd), "MD%c;", m); cat_send(cmd); usleep(400000); }
  cat_query("MD;", r, sizeof(r)); printf("restored %s", r); cat_query("PC;", r, sizeof(r)); printf(" %s\n", r);
  closesocket(cat);
  return 0;
}
