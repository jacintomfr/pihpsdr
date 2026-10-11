/* Copyright (C)
* RADE V1 End-of-Over callsign codec (C port).
*
* Ported from the Rust port in hpsdr-rs (adapted from SDRoxide, GPL-3.0-or-later),
* itself a port of rade_text.cpp / ldpc_encode.cpp / ldpc_decode.cpp from
* https://github.com/tmiw/freedv-backend (BSD-2-Clause, Mooneer Salem).
*
* SPDX-License-Identifier: GPL-3.0-or-later
*
*   This program is free software: you can redistribute it and/or modify
*   it under the terms of the GNU General Public License as published by
*   the Free Software Foundation, either version 3 of the License, or
*   (at your option) any later version.
*
*   This program is distributed in the hope that it will be useful,
*   but WITHOUT ANY WARRANTY; without even the implied warranty of
*   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*   GNU General Public License for more details.
*
*   You should have received a copy of the GNU General Public License
*   along with this program.  If not, see <https://www.gnu.org/licenses/>.
*
*/

/*
 * callsign -> 6-bit OTA alphabet (<=8 chars) -> CRC-8 -> 56 info bits
 *          -> LDPC(112,56) -> x37 interleaver -> QPSK -> 180 EOO floats
 *
 * The arithmetic mirrors the reference operation for operation (float/double
 * promotion, NaN handling through fmaxf).  Do not build this file with
 * -ffast-math or FMA contraction if bit-exact decoder behaviour matters.
 */

#include <math.h>
#include <pthread.h>
#include <string.h>

#include "rade_text.h"

#define K 56
#define N 112
#define MAX_CHARS RADE_TEXT_MAX_CHARS
#define CODEWORD_FLOATS N
#define EOO_FLOATS RADE_TEXT_EOO_FLOATS
#define INTERLEAVE_STRIDE 37
#define LLR_MAX 10000.0f
#define MAX_ITER 30
#define N_EDGES 279

/* Column indices of the three ones in each row of H_a, ascending. */
static const unsigned char ha_cols[K][3] = {
  {3, 13, 40}, {32, 45, 54}, {21, 38, 46}, {1, 26, 43}, {6, 42, 55},
  {11, 21, 45}, {14, 18, 19}, {40, 44, 54}, {5, 15, 20}, {1, 22, 39},
  {7, 30, 36}, {3, 51, 52}, {0, 2, 12}, {6, 7, 31}, {19, 50, 51},
  {9, 12, 21}, {1, 30, 31}, {0, 10, 23}, {27, 30, 43}, {6, 35, 41},
  {2, 10, 48}, {24, 27, 37}, {17, 29, 47}, {18, 20, 23}, {16, 37, 55},
  {14, 25, 47}, {7, 41, 49}, {12, 28, 38}, {8, 22, 25}, {11, 32, 49},
  {38, 44, 53}, {5, 18, 25}, {16, 27, 36}, {31, 39, 55}, {2, 9, 13},
  {5, 22, 26}, {9, 40, 45}, {17, 24, 43}, {3, 28, 44}, {33, 35, 42},
  {0, 28, 52}, {13, 48, 51}, {8, 37, 39}, {4, 10, 20}, {14, 29, 50},
  {11, 41, 46}, {4, 15, 29}, {34, 36, 49}, {4, 48, 50}, {16, 34, 42},
  {15, 17, 26}, {35, 46, 53}, {19, 23, 52}, {8, 24, 47}, {33, 53, 54},
  {32, 33, 34}
};

typedef struct {
  float re;
  float im;
} Sym;

/* ---------------------------- Tanner graph ---------------------------- */

typedef struct {
  int edge_var[N_EDGES];
  int check_n[K];
  int check_edges[K][5];
  int var_n[N];
  int var_edges[N][5];
} Graph;

static Graph graph;
static pthread_once_t graph_once = PTHREAD_ONCE_INIT;

/* Columns of the ones in row i of the full 56x112 H, ascending. */
static int h_row(int i, int cols[5]) {
  int n = 0;
  int a;

  for (a = 0; a < 3; a++) {
    cols[n++] = ha_cols[i][a];
  }

  if (i > 0) {
    cols[n++] = K + i - 1;
  }

  cols[n++] = K + i;
  return n;
}

static void graph_init(void) {
  int e = 0;
  int i;
  int t;
  memset(&graph, 0, sizeof(graph));

  for (i = 0; i < K; i++) {
    int cols[5];
    int n = h_row(i, cols);

    for (t = 0; t < n; t++) {
      int j = cols[t];
      graph.edge_var[e] = j;
      graph.check_edges[i][graph.check_n[i]++] = e;
      graph.var_edges[j][graph.var_n[j]++] = e;
      e++;
    }
  }
}

/* ------------------------------- LDPC -------------------------------- */

/* Systematic LDPC(112,56) encode by forward substitution. */
static void ldpc_encode(const unsigned char s[K], unsigned char c[N]) {
  int i;
  int t;
  memset(c, 0, N);
  memcpy(c, s, K);

  for (i = 0; i < K; i++) {
    unsigned char parity = 0;

    for (t = 0; t < 3; t++) {
      parity ^= c[ha_cols[i][t]] & 1;
    }

    c[K + i] = (i == 0) ? parity : (parity ^ c[K + i - 1]);
  }
}

static int syndrome_ok(const unsigned char bits[N]) {
  int i;
  int t;

  for (i = 0; i < K; i++) {
    int cols[5];
    int n = h_row(i, cols);
    unsigned char acc = 0;

    for (t = 0; t < n; t++) {
      acc ^= bits[cols[t]] & 1;
    }

    if (acc != 0) {
      return 0;
    }
  }

  return 1;
}

static float clampf(float x, float lo, float hi) {
  /* NaN passes through, as with Rust's f32::clamp */
  if (x < lo) {
    x = lo;
  }

  if (x > hi) {
    x = hi;
  }

  return x;
}

/*
 * phi(x) = ln((e^x + 1) / (e^x - 1)), in double.  Returns NaN for large x;
 * the caller swallows it through fmaxf(0, ...), exactly as the reference does.
 */
static float phi(float x) {
  double ex;

  if (x < 1e-10f) {
    return LLR_MAX;
  }

  ex = exp((double)x);

  if (ex < 1e-10) {
    return LLR_MAX;
  }

  return (float)log((ex + 1.0) / (ex - 1.0));
}

static float max_star(float x, float y) {
  float max_xy = (x < y) ? y : x;
  double absdiff = (double)fabsf(x - y);
  float log_xy;

  if (absdiff <= 2.45) {
    log_xy = (float)(-0.24 * absdiff + 0.596);
  } else if (absdiff <= 3.5) {
    log_xy = 0.048f;
  } else {
    log_xy = 0.0f;
  }

  return max_xy + log_xy;
}

static float calc_likelihood(Sym sym, float var, float re, float im, float amp, float avg_amp) {
  float err_r = sym.re - re;
  float err_i = sym.im - im;
  float a = amp / avg_amp;
  return -var * a * a * (err_r * err_r + err_i * err_i);
}

/* Channel LLRs, positive = bit more likely 0. */
static void channel_llrs(const Sym syms[K], const float amps[K], float noise_var, float llr[N]) {
  float sum = 0.0f;
  float mean_amp;
  float es_no;
  int k;

  for (k = 0; k < K; k++) {
    sum += amps[k];
  }

  mean_amp = sum / (float)K;
  es_no = 1.0f / (2.0f * noise_var);

  for (k = 0; k < N; k++) {
    llr[k] = 0.0f;
  }

  for (k = 0; k < K; k++) {
    float a = amps[k];
    float l[4];
    float num[2] = { -LLR_MAX, -LLR_MAX };
    float den[2] = { -LLR_MAX, -LLR_MAX };
    l[0] = calc_likelihood(syms[k], es_no, 1.0f, 0.0f, a, mean_amp);
    l[1] = calc_likelihood(syms[k], es_no, 0.0f, 1.0f, a, mean_amp);
    l[2] = calc_likelihood(syms[k], es_no, 0.0f, -1.0f, a, mean_amp);
    l[3] = calc_likelihood(syms[k], es_no, -1.0f, 0.0f, a, mean_amp);
    den[0] = max_star(den[0], l[0]);
    den[1] = max_star(den[1], l[0]);
    den[0] = max_star(den[0], l[1]);
    num[1] = max_star(num[1], l[1]);
    num[0] = max_star(num[0], l[2]);
    den[1] = max_star(den[1], l[2]);
    num[0] = max_star(num[0], l[3]);
    num[1] = max_star(num[1], l[3]);
    llr[2 * k] = -clampf(num[0] - den[0], -LLR_MAX, LLR_MAX);
    llr[2 * k + 1] = -clampf(num[1] - den[1], -LLR_MAX, LLR_MAX);
  }
}

/* Soft-decision sum-product decode.  Returns 1 if all parity checks hold. */
static int ldpc_decode(const Sym syms[K], const float amps[K], float noise_var, unsigned char bits[N]) {
  float llr_ch[N];
  float m_vc[N_EDGES];
  float m_cv[N_EDGES];
  int iter;
  int i;
  int j;
  int t;
  pthread_once(&graph_once, graph_init);

  if (noise_var < 1e-10f) {
    noise_var = 1e-10f;
  }

  channel_llrs(syms, amps, noise_var, llr_ch);

  for (t = 0; t < N_EDGES; t++) {
    m_vc[t] = llr_ch[graph.edge_var[t]];
    m_cv[t] = 0.0f;
  }

  memset(bits, 0, N);

  for (iter = 0; iter < MAX_ITER; iter++) {
    /* check -> variable */
    for (i = 0; i < K; i++) {
      float sign = 1.0f;
      float sum = 0.0f;

      for (t = 0; t < graph.check_n[i]; t++) {
        float v = m_vc[graph.check_edges[i][t]];
        sign *= (v >= 0.0f) ? 1.0f : -1.0f;
        sum += fmaxf(phi(fabsf(v)), 0.0f);
      }

      for (t = 0; t < graph.check_n[i]; t++) {
        int e = graph.check_edges[i][t];
        float v = m_vc[e];
        float inv_sign = (v >= 0.0f) ? 1.0f : -1.0f;
        m_cv[e] = clampf(inv_sign * sign * fmaxf(phi(sum - phi(fabsf(v))), 0.0f), -LLR_MAX, LLR_MAX);
      }
    }

    /* variable -> check */
    for (j = 0; j < N; j++) {
      float total = llr_ch[j];

      for (t = 0; t < graph.var_n[j]; t++) {
        total += m_cv[graph.var_edges[j][t]];
      }

      for (t = 0; t < graph.var_n[j]; t++) {
        int e = graph.var_edges[j][t];
        m_vc[e] = clampf(total - m_cv[e], -LLR_MAX, LLR_MAX);
      }
    }

    /* posterior, hard decision, syndrome */
    for (j = 0; j < N; j++) {
      float l = llr_ch[j];

      for (t = 0; t < graph.var_n[j]; t++) {
        l += m_cv[graph.var_edges[j][t]];
      }

      bits[j] = (l < 0.0f) ? 1 : 0;
    }

    if (syndrome_ok(bits)) {
      return 1;
    }
  }

  return 0;
}

/* ------------------------- 6-bit OTA alphabet ------------------------- */

/*
 * 0 NUL, 1..9 ASCII 38..46, 10..19 '0'..'9', 20..45 'A'..'Z', 46 '/'.
 * Only the first 8 input characters (UTF-8 aware) are considered; anything
 * unrepresentable, including SPACE, is dropped.  Returns the count.
 */
static int to_ota(const char *callsign, unsigned char out[MAX_CHARS]) {
  int n = 0;
  int chars = 0;
  const unsigned char *p = (const unsigned char *)callsign;
  memset(out, 0, MAX_CHARS);

  for (; p != NULL && *p != 0; p++) {
    unsigned char c = *p;
    int v;

    if ((c & 0xC0) == 0x80) {
      continue;  /* UTF-8 continuation byte: part of a dropped character */
    }

    if (chars++ >= MAX_CHARS) {
      break;
    }

    if (c >= 'a' && c <= 'z') {
      c = (unsigned char)(c - 'a' + 'A');
    }

    if (c >= '&' && c <= '.') {
      v = c - 37;
    } else if (c >= '0' && c <= '9') {
      v = c - '0' + 10;
    } else if (c >= 'A' && c <= 'Z') {
      v = c - 'A' + 20;
    } else if (c == '/') {
      v = 46;
    } else {
      continue;
    }

    out[n++] = (unsigned char)v;
  }

  return n;
}

/* Value 47 (SPACE) and anything above is skipped; stops at the first NUL. */
static int from_ota(const unsigned char vals[MAX_CHARS], char out[9]) {
  int n = 0;
  int i;

  for (i = 0; i < MAX_CHARS; i++) {
    unsigned char v = vals[i];

    if (v == 0) {
      break;
    }

    if (v <= 9) {
      out[n++] = (char)(v + 37);
    } else if (v <= 19) {
      out[n++] = (char)(v - 10 + '0');
    } else if (v <= 45) {
      out[n++] = (char)(v - 20 + 'A');
    } else if (v == 46) {
      out[n++] = '/';
    }
  }

  out[n] = 0;
  return n;
}

/* CRC-8, poly 0x1D, init 0, MSB first, stops at the first NUL. */
static unsigned char crc8(const unsigned char *data, int len) {
  unsigned char crc = 0;
  int i;
  int b;

  for (i = 0; i < len; i++) {
    if (data[i] == 0) {
      break;
    }

    crc ^= data[i];

    for (b = 0; b < 8; b++) {
      crc = (crc & 0x80) ? (unsigned char)((crc << 1) ^ 0x1D) : (unsigned char)(crc << 1);
    }
  }

  return crc;
}

static void pack_info(unsigned char crc, const unsigned char ota[MAX_CHARS], unsigned char bits[K]) {
  int i;

  for (i = 0; i < 8; i++) {
    bits[i] = (crc >> i) & 1;
  }

  for (i = 8; i < K; i++) {
    int since_crc = i - 8;
    bits[i] = (ota[since_crc / 6] >> (since_crc % 6)) & 1;
  }
}

static void unpack_info(const unsigned char bits[N], unsigned char *crc, unsigned char ota[MAX_CHARS]) {
  int i;
  *crc = 0;
  memset(ota, 0, MAX_CHARS);

  for (i = 0; i < 8; i++) {
    *crc |= (unsigned char)((bits[i] & 1) << i);
  }

  for (i = 8; i < K; i++) {
    int since_crc = i - 8;
    ota[since_crc / 6] |= (unsigned char)((bits[i] & 1) << (since_crc % 6));
  }
}

/* ----------------------------- public API ----------------------------- */

void rade_text_encode(const char *callsign, float eoo_out[180]) {
  unsigned char ota[MAX_CHARS];
  unsigned char info[K];
  unsigned char code[N];
  int i;
  to_ota(callsign, ota);
  pack_info(crc8(ota, MAX_CHARS), ota, info);
  ldpc_encode(info, code);

  for (i = 0; i < K; i++) {
    int dst = (INTERLEAVE_STRIDE * i) % K;
    float re;
    float im;

    switch ((code[2 * i] << 1) | code[2 * i + 1]) {
    case 0:
      re = 1.0f;
      im = 0.0f;
      break;

    case 1:
      re = 0.0f;
      im = 1.0f;
      break;

    case 2:
      re = 0.0f;
      im = -1.0f;
      break;

    default:
      re = -1.0f;
      im = 0.0f;
      break;
    }

    eoo_out[2 * dst] = re;
    eoo_out[2 * dst + 1] = im;
  }

  for (i = CODEWORD_FLOATS; i < EOO_FLOATS; i++) {
    eoo_out[i] = (i % 2 == 0) ? 1.0f : 0.0f;
  }
}

int rade_text_decode(const float eoo_in[180], char call_out[9]) {
  Sym syms[K];
  float amps[K];
  unsigned char bits[N];
  unsigned char ota[MAX_CHARS];
  unsigned char rx_crc;
  float ss = 0.0f;
  float noise_var;
  int ss_count = 0;
  int n_syms = EOO_FLOATS / 2;
  int i;
  call_out[0] = 0;

  /* deinterleave */
  for (i = 0; i < K; i++) {
    int src = (INTERLEAVE_STRIDE * i) % K;
    syms[i].re = eoo_in[2 * src];
    syms[i].im = eoo_in[2 * src + 1];
  }

  /* noise variance from the known tail, every symbol should be (1, 0) */
  for (i = K; i < n_syms; i++) {
    float re = eoo_in[2 * i];
    float im = eoo_in[2 * i + 1];
    float amp = sqrtf(re * re + im * im);

    if (amp > 0.0f) {
      float dr = 1.0f - re / amp;
      float di = -im / amp;
      ss += dr * dr + di * di;
      ss_count += 2;
    }
  }

  noise_var = fmaxf(ss / (float)(ss_count - 1), 1e-6f);

  for (i = 0; i < K; i++) {
    float amp = sqrtf(syms[i].re * syms[i].re + syms[i].im * syms[i].im);

    if (amp > 0.0f) {
      syms[i].re /= amp;
      syms[i].im /= amp;
    } else {
      syms[i].re = 0.0f;
      syms[i].im = 0.0f;
    }

    amps[i] = amp;
  }

  if (!ldpc_decode(syms, amps, noise_var, bits)) {
    return 0;
  }

  unpack_info(bits, &rx_crc, ota);

  if (rx_crc != crc8(ota, MAX_CHARS)) {
    return 0;
  }

  return from_ota(ota, call_out) > 0;
}
