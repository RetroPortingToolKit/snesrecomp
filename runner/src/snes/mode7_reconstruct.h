#ifndef SNESRECOMP_MODE7_RECONSTRUCT_H
#define SNESRECOMP_MODE7_RECONSTRUCT_H
#include "hd_jobs_internal.h"
#include "mode7_hd.h"
#include <string.h>

/* Original reconstruction: fit inverse matrix coefficients to a line in
 * raster Y. Quantized perspective tables then describe a continuous plane.
 * A failed fit retains local affine geometry, rather than inventing a plane
 * for rotating rooms, waves or deliberate raster distortions. */
static bool hd_same_camera(const SnesHdLine *a, const SnesHdLine *b) {
  for (unsigned c = 0; c < 4; ++c)
    if (fabs(a->matrix[c] - b->matrix[c]) >
        fmax(64, .5*fmax(fabs(a->matrix[c]), fabs(b->matrix[c])))) return false;
  return a->kind == kSnesHdReconstructed && b->kind == a->kind &&
      a->camera == b->camera && a->control == b->control &&
      a->composition == b->composition && b->raster_line == a->raster_line + 1 &&
      !memcmp(a->matrix + 4, b->matrix + 4, 4 * sizeof(double));
}
static void hd_reconstruct(SnesHdFrame *f) {
  for (unsigned y = 0; y < f->height; ++y) f->lines[y].perspective = 0;
  for (unsigned first = 0; first < f->height;) {
    unsigned end = first + 1;
    while (end < f->height && hd_same_camera(&f->lines[end-1], &f->lines[end])) ++end;
    unsigned inset = (end - first) / 8, lo = first + inset, hi = end - inset;
    bool valid = hi - lo >= 4, varying = false;
    unsigned mask = 0;
    double fit[8] = {0};
    for (unsigned c = 0; c < 4 && valid; ++c) {
      double min = f->lines[lo].matrix[c], max = min;
      for (unsigned y = lo; y < hi; ++y) {
        double v = f->lines[y].matrix[c];
        if (v < min) min = v;
        if (v > max) max = v;
      }
      if (max == min) { fit[c] = min; continue; }
      varying = true;
      mask |= 1u << c;
      if (min <= 0 && max >= 0) { valid = false; break; }
      /* Least squares reduces quantization noise without depending on a
       * single potentially anomalous endpoint. Center Y for stability. */
      double mid = (f->lines[lo].raster_line + f->lines[hi-1].raster_line) * .5;
      double sum = 0, cross = 0, square = 0;
      for (unsigned y = lo; y < hi; ++y) {
        double t = f->lines[y].raster_line - mid;
        double inverse = 1.0 / f->lines[y].matrix[c];
        sum += inverse; cross += t * inverse; square += t * t;
      }
      fit[c+4] = cross / square;
      fit[c] = sum / (hi-lo) - fit[c+4] * mid;
      /* DSP tables may have horizon artifacts outside the fit interval.
       * Judge residuals inside it, but reject poles/unbounded extrapolation
       * throughout the complete band, including every subpixel row. */
      for (unsigned y = first; y < end && valid; ++y) {
        double denominator = fit[c] + fit[c+4] * f->lines[y].raster_line;
        double original = f->lines[y].matrix[c];
        if (!isfinite(denominator) || denominator * original <= 0 ||
            (denominator - .5*fit[c+4]) * original <= 0 ||
            (denominator + .5*fit[c+4]) * original <= 0 ||
            fabs(denominator - .5*fit[c+4]) < 1.0/65536 ||
            fabs(denominator + .5*fit[c+4]) < 1.0/65536 ||
            (y >= lo && y < hi &&
             fabs(1.0 / denominator - original) > fmax(2.0, fabs(original) * .025))) valid = false;
      }
    }
    if (valid && varying) for (unsigned y = first; y < end; ++y) {
      memcpy(f->lines[y].reciprocal, fit, sizeof(fit));
      f->lines[y].perspective = (uint8_t)mask;
    }
    first = end;
  }
}
#endif
