/* rss_wrap OUTFILE CMD [ARGS...]: runs CMD with its stdout discarded and writes its peak resident set
 * size in KiB (ru_maxrss of the child) to OUTFILE. The exit status is the child's.
 *
 * Why this exists (J5n2): Linux counts the resident set that the parent had when it forked in the
 * child's ru_maxrss, through exec. A python3 launcher (about 11 MB) therefore put a floor of about
 * 11 MB under every figure, and every small peak read as exactly the floor. This launcher is a few
 * hundred KiB, so the figures are the child's own. compare.sh builds it with the first C compiler it
 * finds. Linux only (fork, wait4); compare.sh itself needs a Linux machine. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: rss_wrap OUTFILE CMD [ARGS...]\n");
        return 2;
    }
    pid_t p = fork();
    if (p < 0) {
        return 2;
    }
    if (p == 0) {
        if (freopen("/dev/null", "w", stdout) == NULL) {
            _exit(126);
        }
        execvp(argv[2], argv + 2);
        _exit(127);
    }
    int st = 0;
    struct rusage ru;
    if (wait4(p, &st, 0, &ru) < 0) {
        return 2;
    }
    FILE *f = fopen(argv[1], "w");
    if (f == NULL) {
        return 2;
    }
    fprintf(f, "%ld\n", ru.ru_maxrss);
    fclose(f);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
}
