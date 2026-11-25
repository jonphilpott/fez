/* See scales.h. Each function is one formula that maps a 0..32767 register value to a real quantity. */
#include "scales.h"
#include <math.h>

#define FULL 32767.0            /* register full scale */

double atk_sec(double v)   { return 0.001 * pow(500.0, v / FULL); }
double rel_sec(double v)   { return 0.01 * pow(400.0, v / FULL); }
double slew_sec(double v)  { return 0.001 * pow(5000.0, v / FULL); }
double lfo_hz(double v)    { return 0.05 * pow(400.0, v / FULL); }
double pitch_hz(double p)  { return 440.0 * pow(2.0, (p / 256.0 - 69.0) / 12.0); }
double cut_hz(double v)    { return 40.0 * pow(350.0, v / FULL); }
double fm_hz(double v)     { return 0.5 * pow(4000.0, v / FULL); }
double nz_hz(double v)     { return 100.0 * pow(200.0, v / FULL); }
double fm_depth_hz(double v) { double x = v / FULL; return x * x * 2000.0; }
double lz_units_per_sec(double v) { return 0.2 * pow(1000.0, v / FULL); }
double lz_rho(double v)    { return (60.0 * v) / FULL; }
double dly_sec(double v)   { return 12.0 * v / FULL; }
double dly_lp_hz(double v)  { return 300.0 * pow(40.0, v / FULL); }
double dly_fb(double v)    { return v / FULL; }
double spring_fb(double v) { return 0.3 + 0.7 * (v / FULL); }
int cx_shape(int v)     { int s = (v * 3) / 32768; return s > 2 ? 2 : s; }
int md_shape(int v)     { int s = (v * 4) / 32768; return s > 3 ? 3 : s; }
