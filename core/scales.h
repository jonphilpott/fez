/* Conversions from register values (0..32767) to physical units. One definition of each curve, used by the
   control side (slew, LFO, Lorenz) and the audio side (voice, effects). Most are exponential: equal register
   steps give equal ratios, which is how pitch and time feel to the ear. */
#ifndef FEZ_SCALES_H
#define FEZ_SCALES_H

double atk_sec(double v);          /* envelope attack: 1 ms to 500 ms */
double rel_sec(double v);          /* envelope release: 10 ms to 4 s */
double slew_sec(double v);         /* slew time constant: 1 ms to 5 s */
double lfo_hz(double v);           /* 0.05 Hz to 20 Hz */
double pitch_hz(double p);         /* 8.8 semitones (MIDI note x 256) to hertz */
double cut_hz(double v);           /* filter cutoff: 40 Hz to 14 kHz */
double fm_hz(double v);            /* modulation oscillator: 0.5 Hz to 2 kHz */
double nz_hz(double v);            /* noise tone lowpass: 100 Hz to 20 kHz */
double fm_depth_hz(double v);      /* FM deviation in hertz, squared curve */
double lz_units_per_sec(double v); /* Lorenz simulation speed */
double lz_rho(double v);           /* Lorenz rho: 0 to 60 */
double dly_sec(double v);          /* delay time: 0 to 12 s, linear */
double dly_lp_hz(double v);        /* delay feedback lowpass: 300 Hz to 12 kHz */
double dly_fb(double v);           /* delay feedback: 0 to 1.0 (JS capped it at 0.85; the C core allows 100 percent) */
double spring_fb(double v);        /* spring feedback: 0.3 to 1.0 (JS capped it at 0.76) */
int    cx_shape(int v);         /* complex oscillator wave: 0 triangle, 1 square, 2 saw */
int    md_shape(int v);         /* modulation oscillator wave: 0 sine, 1 triangle, 2 square, 3 saw */

#endif
