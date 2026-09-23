#include "exec/process.h"
#include "platform.h"
#include <errno.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define RE0_PROCESS_MAX_ARGUMENTS 128
#define RE0_PROCESS_MAX_COMMAND 32767

#if defined(RE0_PLATFORM_WINDOWS)
#include <windows.h>

static bool append_char(char *buffer, size_t *length, char value) {
    if (*length >= RE0_PROCESS_MAX_COMMAND - 1) return false;
    buffer[(*length)++] = value;
    return true;
}

static bool append_argument(char *buffer, size_t *length, const char *argument) {
    if (!append_char(buffer, length, '"')) return false;
    const char *p = argument;
    for (;;) {
        size_t slashes = 0;
        while (*p == '\\') { slashes++; p++; }
        size_t count = (*p == '"' || !*p) ? slashes * 2 : slashes;
        for (size_t i = 0; i < count; i++)
            if (!append_char(buffer, length, '\\')) return false;
        if (!*p) break;
        if (*p == '"' && !append_char(buffer, length, '\\')) return false;
        if (!append_char(buffer, length, *p++)) return false;
    }
    return append_char(buffer, length, '"');
}
#else
#include <unistd.h>
#include <sys/wait.h>
#endif

int re0_process_run(const char *program, const char *const arguments[]) {
    if (!program || !*program || !arguments || !arguments[0]) return -1;
    size_t count = 0;
    while (count < RE0_PROCESS_MAX_ARGUMENTS && arguments[count]) count++;
    if (count == RE0_PROCESS_MAX_ARGUMENTS) return -1;
#if defined(RE0_PLATFORM_WINDOWS)
    char *command = malloc(RE0_PROCESS_MAX_COMMAND);
    if (!command) return -1;
    size_t length = 0;
    bool ok = true;
    for (size_t i = 0; i < count && ok; i++) {
        if (i) ok = append_char(command, &length, ' ');
        if (ok) ok = append_argument(command, &length, arguments[i]);
    }
    command[length] = 0;
    STARTUPINFOA startup = {0};
    PROCESS_INFORMATION process = {0};
    startup.cb = sizeof(startup);
    BOOL launched = ok && CreateProcessA(NULL, command, NULL, NULL, FALSE, 0,
                                         NULL, NULL, &startup, &process);
    free(command);
    if (!launched) return -1;
    DWORD exit_code = 0;
    DWORD waited = WaitForSingleObject(process.hProcess, INFINITE);
    BOOL obtained = waited == WAIT_OBJECT_0 && GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return obtained && exit_code <= 2147483647UL ? (int)exit_code : -1;
#else
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execvp(program, (char *const *)arguments);
        _exit(127);
    }
    int status = 0;
    pid_t waited;
    do { waited = waitpid(pid, &status, 0); } while (waited < 0 && errno == EINTR);
    if (waited < 0) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
#endif
}
