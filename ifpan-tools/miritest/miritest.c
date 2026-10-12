#include <SoapySDR/Device.h>
#include <SoapySDR/Formats.h>
#include <SoapySDR/Errors.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

static int run(int pass, double secs) {
  SoapySDRKwargs args = {0};
  SoapySDRKwargs_set(&args, "driver", "soapyMiri");
  double t0 = now();
  SoapySDRDevice *sdr = SoapySDRDevice_make(&args);
  SoapySDRKwargs_clear(&args);
  if (!sdr) { printf("pass %d: make failed: %s\n", pass, SoapySDRDevice_lastError()); return 1; }
  if (SoapySDRDevice_setSampleRate(sdr, SOAPY_SDR_RX, 0, 1536000.0) != 0) printf("setSampleRate: %s\n", SoapySDRDevice_lastError());
  if (SoapySDRDevice_setFrequency(sdr, SOAPY_SDR_RX, 0, 73.05e6, NULL) != 0) printf("setFrequency: %s\n", SoapySDRDevice_lastError());
  SoapySDRDevice_setGain(sdr, SOAPY_SDR_RX, 0, 30.0);
  SoapySDRKwargs sargs = {0};
  const char *mode = getenv("MIRI_TRANSFER");
  if (mode) SoapySDRKwargs_set(&sargs, "transfer", mode);
  SoapySDRStream *rx = SoapySDRDevice_setupStream(sdr, SOAPY_SDR_RX, SOAPY_SDR_CF32, NULL, 0, &sargs);
  SoapySDRKwargs_clear(&sargs);
  if (!rx) { printf("setupStream: %s\n", SoapySDRDevice_lastError()); SoapySDRDevice_unmake(sdr); return 1; }
  SoapySDRDevice_activateStream(sdr, rx, 0, 0, 0);
  double t1 = now();
  static float buf[2 * 65536];
  void *buffs[] = { buf };
  long long total = 0; int timeouts = 0, overflows = 0, errors = 0, reads = 0;
  double peak = 0, sum2 = 0; long long nstat = 0;
  while (now() - t1 < secs) {
    int flags; long long tns;
    int r = SoapySDRDevice_readStream(sdr, rx, buffs, 65536, &flags, &tns, 200000);
    if (r == SOAPY_SDR_TIMEOUT) { timeouts++; continue; }
    if (r == SOAPY_SDR_OVERFLOW) { overflows++; continue; }
    if (r < 0) { errors++; if (errors < 5) printf("read error %d\n", r); continue; }
    reads++; total += r;
    for (int i = 0; i < 2 * r; i++) { double v = fabs(buf[i]); if (v > peak) peak = v; sum2 += buf[i] * (double) buf[i]; nstat++; }
  }
  double el = now() - t1;
  printf("pass %d: open %.2f s, %lld samples in %.2f s = %.4f MS/s (expected 1.536), reads=%d timeouts=%d overflows=%d errors=%d, peak=%.3f rms=%.4f\n",
         pass, t1 - t0, total, el, total / el / 1e6, reads, timeouts, overflows, errors, peak, sqrt(sum2 / (nstat ? nstat : 1)));
  SoapySDRDevice_deactivateStream(sdr, rx, 0, 0);
  SoapySDRDevice_closeStream(sdr, rx);
  SoapySDRDevice_unmake(sdr);
  return 0;
}

int main(void) {
  for (int p = 1; p <= (getenv("MIRI_PASSES") ? atoi(getenv("MIRI_PASSES")) : 3); p++) { run(p, p == 1 ? 8.0 : 3.0); fflush(stdout); }
  return 0;
}
