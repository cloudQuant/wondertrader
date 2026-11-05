// Diagnostic probe (no product change): fork-child open of an
// UnresolvedReportJournal, printing the child's view of the guard file.
#include <WtCore/UnresolvedReportJournal.h>
#include <sys/acl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <string>
namespace fs = std::filesystem;
static void describe(const char* who, const std::string& path)
{
    int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW);
    struct stat st; memset(&st, 0, sizeof(st));
    int r = fstat(fd, &st);
    errno = 0;
    acl_t acl = acl_get_fd_np(fd, ACL_TYPE_EXTENDED);
    int aclErr = errno;
    if (acl) acl_free(acl);
    printf("%s %s fd=%d fstat=%d nlink=%u mode=%o uid=%u acl=%p errno=%d(%s)\n", who, path.c_str(), fd, r,
        (unsigned)st.st_nlink, (unsigned)(st.st_mode & 07777), (unsigned)st.st_uid, (void*)acl, aclErr, strerror(aclErr));
    if (fd >= 0) close(fd);
}
int main(int argc, char** argv)
{
    int rounds = argc > 1 ? atoi(argv[1]) : 5;
    int fails = 0;
    for (int i = 0; i < rounds; ++i)
    {
        fs::path dir = fs::temp_directory_path() / ("wt_urj_probe_" + std::to_string(getpid()) + "_" + std::to_string(i));
        fs::create_directory(dir);
        fs::permissions(dir, fs::perms::owner_all, fs::perm_options::replace);
        std::string wal = (dir / "reports.wal").string();
        {
            wtp::UnresolvedReportJournal seed;
            if (seed.createNew(wal, "probe") != wtp::UnresolvedReportJournal::Status::Ok) { printf("createNew failed\n"); return 2; }
            uint64_t seq = 0;
            seed.append("x", seq);
            seed.close();
        }
        describe("parent", wal + ".guard");
        fflush(stdout);
        pid_t c = fork();
        if (c == 0)
        {
            describe("child ", wal + ".guard");
            wtp::UnresolvedReportJournal w;
            std::string err;
            auto st = w.open(wal, &err);
            printf("child open status=%d err=%s\n", (int)st, err.c_str());
            fflush(stdout);
            _exit(st == wtp::UnresolvedReportJournal::Status::Ok ? 0 : 10);
        }
        int status = 0; waitpid(c, &status, 0);
        if (WEXITSTATUS(status) != 0) ++fails;
        wtp::UnresolvedReportJournal p; std::string perr;
        auto ps = p.open(wal, &perr);
        printf("parent reopen status=%d err=%s\n", (int)ps, perr.c_str());
        p.close();
        std::error_code ec; fs::remove_all(dir, ec);
    }
    printf("fails=%d/%d\n", fails, rounds);
    return 0;
}
