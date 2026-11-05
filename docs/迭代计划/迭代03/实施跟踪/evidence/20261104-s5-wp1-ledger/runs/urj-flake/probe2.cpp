#include <sys/acl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <string>
int main()
{
    std::string base = std::string(getenv("TMPDIR")) + "wt_acl_probe_" + std::to_string(getpid());
    int bad = 0, total = 0, recovered = 0;
    for (int i = 0; i < 40; ++i)
    {
        std::string f = base + "_" + std::to_string(i);
        int fd = ::open(f.c_str(), O_CREAT | O_RDWR, 0600);
        { errno = 0; acl_t a = acl_get_fd_np(fd, ACL_TYPE_EXTENDED); if (a) acl_free(a); }
        close(fd);
        fflush(stdout);
        pid_t c = fork();
        if (c == 0)
        {
            int fd2 = ::open(f.c_str(), O_RDONLY);
            int e[5];
            for (int k = 0; k < 5; ++k) { errno = 0; acl_t a = acl_get_fd_np(fd2, ACL_TYPE_EXTENDED); e[k] = errno; if (a) acl_free(a); }
            int firstBad = e[0] != ENOENT;
            int anyOkLater = 0; for (int k = 1; k < 5; ++k) if (e[k] == ENOENT) anyOkLater = 1;
            printf("%d %d %d %d %d\n", e[0], e[1], e[2], e[3], e[4]); fflush(stdout);
            _exit(firstBad ? (anyOkLater ? 2 : 1) : 0);
        }
        int st = 0; waitpid(c, &st, 0); ++total;
        if (WEXITSTATUS(st) == 1) ++bad; else if (WEXITSTATUS(st) == 2) { ++bad; ++recovered; }
        unlink(f.c_str());
    }
    fprintf(stderr, "children with first-call errno!=ENOENT: %d/%d, recovered by retry: %d\n", bad, total, recovered);
}
