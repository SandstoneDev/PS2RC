/* hle_aw_psp.h - automatic test results + cpu_clock_mhz (PSP only,
   src/hle/psp/hle_aw_psp.c; called from hle_psp.c) */
#ifndef HLE_AW_PSP_H
#define HLE_AW_PSP_H
#include <stddef.h>

/* A. results block / results_<seq>.txt / shot_<seq>_<tag>.bmp */
void hle_aw_result_add(const char *s, size_t n);
void hle_aw_result_tee(const char *s, size_t n);          /* hle_plat_log_write: selected log lines */
const char *hle_aw_results(unsigned *len, unsigned *lines, unsigned *dropped);
void hle_aw_results_freeze(void);
int hle_aw_cap_files(const char *dir, const char *prefix, const char *suffix, int keep);
int hle_aw_shot(const char *dir, unsigned seq, const char *tag, int keep, char *out, unsigned outn);

/* B. cpu_clock_mhz (boot-time ramp, periodic check, restore at exit) */
void hle_aw_clock_cfg(const char *k, const char *v);
int hle_aw_clock_on(void);
int hle_aw_clock_early(void);                 /* cpu_clock_early=1 */
/* cpu_clock_at=main|thread - ARK's call at the first line of main() (stage 0) / of the game thread
   (stage 1); reads poc6.cfg itself; the report goes to session.log once it is open */
void hle_aw_preclock(const char *dir, int stage, unsigned t0_us);
void hle_aw_preclock_report(void (*logf)(const char *line));
/* cpu_auto=0|342|365 (+ cpu_auto_reset=N) - crash-safe automatic overclock, see hle_aw_psp.c */
void hle_aw_auto_boot(const char *dir, unsigned t0_us);    /* main(), first line (reads poc6.cfg itself) */
void hle_aw_auto_report(void (*logf)(const char *line));   /* after the log is open */
int hle_aw_auto_tick(char *out, unsigned n);               /* watchdog, once per second */
int hle_aw_auto_exit(int clean, char *out, unsigned n);    /* exit sequence */
int hle_aw_auto_active(void);                              /* the active MHz, 0 = not overclocked */
void hle_aw_clock_boot(void (*logf)(const char *line));
int hle_aw_clock_tick(char *out, unsigned n, int *emergency);
int hle_aw_clock_down(const char *who, char *out, unsigned n);
void hle_aw_clock_report(void);
#endif
