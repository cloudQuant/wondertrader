#include <fcntl.h>
#include <semaphore.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <thread>

extern "C" void __tsan_acquire(void*);
extern "C" void __tsan_release(void*);

int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    const bool annotated = std::strcmp(argv[1], "annotated") == 0;
    const bool unprotected = std::strcmp(argv[1], "unprotected") == 0;
    char name[80];
    std::snprintf(name, sizeof(name), "/wt-sem-tsan-%d", getpid());
    sem_t* s = sem_open(name, O_CREAT | O_EXCL, 0600, 1);
    if (s == SEM_FAILED) return 3;
    sem_unlink(name);
    int counter = 0;
    auto work = [&] {
        for (int i=0; i<1000; ++i) {
            if (!unprotected) { if (sem_wait(s)) return; if (annotated) __tsan_acquire(s); }
            ++counter;
            if (!unprotected) { if (annotated) __tsan_release(s); sem_post(s); }
        }
    };
    std::thread one(work), two(work); one.join(); two.join(); sem_close(s);
    return counter == 2000 ? 0 : 4;
}
