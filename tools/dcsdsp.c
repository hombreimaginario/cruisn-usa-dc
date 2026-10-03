/*
 * dcsdsp.c - Ayudante de tools/dcs_bank.py para el trabajo pesado de audio.
 *
 *   dcsdsp resample <entrada.raw> <hz_entrada> <salida.raw> <hz_salida>
 *       Remuestrea PCM de 16 bits mono con un filtro sinc con ventana
 *       (corte en 0.45 * la frecuencia menor).
 *   dcsdsp adpcm <entrada.raw> <salida.adp> [<muestra_bucle>]
 *       Codifica a ADPCM de Yamaha (AICA), el mismo algoritmo que
 *       wav2adpcm de KallistiOS. Con <muestra_bucle> codifica entrada + la
 *       parte desde el bucle otra vez y guarda [0, bucle) de la primera pasada
 *       y [bucle, fin) de la segunda: asi el estado del decodificador al
 *       llegar al final coincide con el del inicio del bucle y no hay saltos.
 *   dcsdsp findloop <entrada.raw> <hz> <inicio_s> <ventana_s> <lag_min_s>
 *       Busca a partir de que retardo se repite la senal (musica en bucle):
 *       compara una ventana que empieza en <inicio_s> con el resto de la
 *       grabacion y escribe los mejores retardos (muestras) y su error
 *       relativo.
 *
 * Compilar: cc -O2 -o dcsdsp dcsdsp.c -lm
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int16_t *load(const char *path, long *n)
{
    FILE *f = fopen(path, "rb");
    int16_t *d;
    long sz;
    if (!f) {
        perror(path);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    d = malloc(sz + 2);
    if (fread(d, 1, sz, f) != (size_t)sz) {
        perror(path);
        exit(1);
    }
    fclose(f);
    *n = sz / 2;
    return d;
}

static void save(const char *path, const void *d, long bytes)
{
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(d, 1, bytes, f) != (size_t)bytes) {
        perror(path);
        exit(1);
    }
    fclose(f);
}

static int resample(int argc, char **argv)
{
    long n, m, i;
    int16_t *in, *out;
    double rin, rout, ratio, fc, width;
    int taps = 24;

    if (argc != 6)
        return 2;
    in = load(argv[2], &n);
    rin = atof(argv[3]);
    rout = atof(argv[5]);
    ratio = rin / rout;
    m = (long)(n / ratio);
    out = malloc(m * 2 + 2);
    fc = 0.45 * (rout < rin ? rout : rin) / rin;      /* ciclos por muestra de entrada */
    width = taps / (2.0 * fc);                         /* semiancho en muestras de entrada */
    for (i = 0; i < m; i++) {
        double c = i * ratio, acc = 0, wsum = 0;
        long k0 = (long)ceil(c - width), k1 = (long)floor(c + width), k;
        for (k = k0; k <= k1; k++) {
            double x = k - c, s, w;
            if (k < 0 || k >= n)
                continue;
            s = x == 0 ? 2 * fc : sin(2 * M_PI * fc * x) / (M_PI * x);
            w = 0.42 + 0.5 * cos(M_PI * x / width) + 0.08 * cos(2 * M_PI * x / width);
            acc += in[k] * s * w;
            wsum += s * w;
        }
        if (wsum != 0)
            acc /= wsum;
        if (acc > 32767)
            acc = 32767;
        if (acc < -32768)
            acc = -32768;
        out[i] = (int16_t)lrint(acc);
    }
    save(argv[4], out, m * 2);
    return 0;
}

#define CLAMP(x, lo, hi) ((x) > (hi) ? (hi) : (x) < (lo) ? (lo) : (x))

static int16_t ymz_step(int step, int16_t *history, int16_t *step_size)
{
    static const int step_table[8] = { 230, 230, 230, 230, 307, 409, 512, 614 };
    int sign = step & 8, delta = step & 7;
    int diff = ((1 + (delta << 1)) * *step_size) >> 3;
    int newval = *history;
    int nstep = (step_table[delta] * *step_size) >> 8;

    diff = CLAMP(diff, 0, 32767);
    if (sign)
        newval -= diff;
    else
        newval += diff;
    *step_size = (int16_t)CLAMP(nstep, 127, 24576);
    *history = (int16_t)(newval = CLAMP(newval, -32768, 32767));
    return (int16_t)newval;
}

/* Codifica n muestras; los nibbles se guardan en out[] (uno por muestra) */
static void encode(const int16_t *in, long n, uint8_t *nib, int16_t *hist, int16_t *ss)
{
    long i;
    for (i = 0; i < n; i++) {
        int step = (in[i] & -8) - *hist;
        int s = (int)(((long)abs(step) << 16) / ((long)*ss << 14));
        s = CLAMP(s, 0, 7);
        if (step < 0)
            s |= 8;
        nib[i] = (uint8_t)s;
        ymz_step(s, hist, ss);
    }
}

static int adpcm(int argc, char **argv)
{
    long n, loop = -1, i, total;
    int16_t *in, hist = 0, ss = 127;
    uint8_t *nib, *out;

    if (argc != 4 && argc != 5)
        return 2;
    in = load(argv[2], &n);
    if (argc == 5)
        loop = atol(argv[4]);
    nib = malloc(n + 64);
    encode(in, n, nib, &hist, &ss);
    if (loop >= 0 && loop < n)
        encode(in + loop, n - loop, nib + loop, &hist, &ss);
    total = (n + 1) & ~1L;
    if (total > n)
        nib[n] = 0;
    out = malloc(total / 2);
    for (i = 0; i < total; i += 2)
        out[i / 2] = (uint8_t)(nib[i] | (nib[i + 1] << 4));
    save(argv[3], out, total / 2);
    return 0;
}

static int findloop(int argc, char **argv)
{
    long n, i, lag, m, dn, start, win, minlag, best[5];
    double rate, energy = 0, berr[5];
    int16_t *in;
    float *dec;
    int k, j, D = 12;

    if (argc != 7)
        return 2;
    in = load(argv[2], &n);
    rate = atof(argv[3]);
    start = (long)(atof(argv[4]) * rate);
    win = (long)(atof(argv[5]) * rate);
    minlag = (long)(atof(argv[6]) * rate);
    if (start + win > n)
        return 1;
    /* busqueda gruesa a 1/D de la frecuencia (media de D muestras) */
    dn = n / D;
    dec = malloc(dn * sizeof(float));
    for (i = 0; i < dn; i++) {
        float acc = 0;
        for (j = 0; j < D; j++)
            acc += in[i * D + j];
        dec[i] = acc / D;
    }
    m = win / D;
    for (i = 0; i < m; i++)
        energy += (double)dec[start / D + i] * dec[start / D + i];
    for (k = 0; k < 5; k++) {
        best[k] = -1;
        berr[k] = 1e30;
    }
    for (lag = minlag / D; start / D + lag + m <= dn; lag++) {
        double err = 0;
        const float *a = dec + start / D, *b = a + lag;
        for (i = 0; i < m && err < berr[4] * energy; i++) {
            double d = a[i] - b[i];
            err += d * d;
        }
        err /= energy + 1;
        if (err < berr[4]) {
            /* evitar minimos casi iguales en retardos vecinos */
            for (k = 0; k < 5; k++)
                if (best[k] >= 0 && labs(best[k] - lag * D) < rate / 4)
                    break;
            if (k < 5) {
                if (err < berr[k]) {
                    berr[k] = err;
                    best[k] = lag * D;
                }
            } else {
                berr[4] = err;
                best[4] = lag * D;
            }
            for (k = 4; k > 0 && berr[k] < berr[k - 1]; k--) {
                double te = berr[k]; long tl = best[k];
                berr[k] = berr[k - 1]; best[k] = best[k - 1];
                berr[k - 1] = te; best[k - 1] = tl;
            }
        }
    }
    /* afinado a resolucion completa alrededor de cada candidato */
    for (k = 0; k < 5; k++) {
        long l0 = best[k], bl = l0;
        double be = 1e30, e2 = 0;
        if (l0 < 0)
            continue;
        for (i = 0; i < win; i++)
            e2 += (double)in[start + i] * in[start + i];
        for (lag = l0 - D; lag <= l0 + D; lag++) {
            double err = 0;
            if (start + lag + win > n)
                break;
            for (i = 0; i < win; i++) {
                double d = in[start + i] - in[start + lag + i];
                err += d * d;
            }
            err /= e2 + 1;
            if (err < be) {
                be = err;
                bl = lag;
            }
        }
        printf("%ld %.5f\n", bl, be);
    }
    return 0;
}

int main(int argc, char **argv)
{
    int r = 2;
    if (argc > 1 && !strcmp(argv[1], "resample"))
        r = resample(argc, argv);
    else if (argc > 1 && !strcmp(argv[1], "adpcm"))
        r = adpcm(argc, argv);
    else if (argc > 1 && !strcmp(argv[1], "findloop"))
        r = findloop(argc, argv);
    if (r == 2)
        fprintf(stderr, "uso: dcsdsp resample|adpcm ...\n");
    return r;
}
