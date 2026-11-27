/* Data tables (defined in tables.c). These structs are the only shape the data has; the rest of the core reads them through these types. */
#ifndef FEZ_TABLES_H
#define FEZ_TABLES_H

#define PATCH_MAX_PRESETS 64   /* register presets one patch may set (the generator would fail to compile if exceeded) */
#define PATCH_MAX_ROUTES  16   /* same as the matrix size */

typedef struct { const char *name; int addr; int def; double tau; } ParamDef;  /* tau: smoothing time in seconds */
typedef struct { const char *name; int addr; } NameDef;
typedef struct { const char *name; const char *text; } HelpDef;
typedef struct { int addr; int value; } Preset;
typedef struct { int on; int rate; int block; } PulserDef;   /* rate: pulses per minute; block 0..15, 16 = sequencer, 17 = grain */
typedef struct { int src; int dst; int w; } RouteDef;       /* a live route: src is a row of SRC_TABLE */
typedef struct { const char *src; int dst; int w; } RouteSpec;   /* a route in an example patch: the source by NAME, so moving a source cannot change it */
typedef struct { int rule; int row; int tap; int trig; } CaDef;
typedef struct { int rate; int rho; int trig; } LzDef;

typedef struct {
  const char *name;
  const char *desc;
  const char *listen;       /* what to listen for */
  Preset presets[PATCH_MAX_PRESETS];   /* the list ends at the first unused entry (address 0, which no preset uses) */
  const char *blocks[16];   /* source text of each block ("" = empty) */
  const char *words[16];
  PulserDef pulsers[4];
  int nroutes;   RouteSpec routes[PATCH_MAX_ROUTES];
  int nsetup;    int setup[8];   /* blocks to run once at load */
  CaDef ca;
  LzDef lz;
  int level;     /* master volume, 0..32767 */
} PatchDef;

extern const ParamDef PARAM_TABLE[];  extern const int PARAM_COUNT;
extern const NameDef  NAME_TABLE[];   extern const int NAME_COUNT;
extern const HelpDef  HELP_TABLE[];   extern const int HELP_COUNT;
typedef struct { const char *name; int addr; const char *group, *help; } SrcDef;
extern const SrcDef   SRC_TABLE[];    extern const int SRC_COUNT;   /* matrix sources by number (0 is "", none): name and register address */
extern const PatchDef PATCH_TABLE[];  extern const int PATCH_COUNT;

extern const int HZ_TAB[257];
extern const int HZ_K;
extern const int JUST88[13];
extern const unsigned char FONT5[95][8];   /* 5x8 font, ASCII 32..126; row bits, bit 4 = leftmost pixel */

#endif
