#ifndef RE0_PROCESS_H
#define RE0_PROCESS_H
/* Runs a null-terminated argument vector without invoking a command shell. */
int re0_process_run(const char *program, const char *const arguments[]);
#endif
