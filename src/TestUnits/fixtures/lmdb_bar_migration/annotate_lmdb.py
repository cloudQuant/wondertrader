"""TSan-only snapshot annotation for macOS named semaphores.

TSan on the tested macOS runtime does not recognize sem_open synchronization.
Annotate only the real LMDB semaphore acquire/release; do not alter production
sources. The unannotated result and independent semaphore control are retained.
"""
from pathlib import Path
import sys

source = Path(sys.argv[1]).read_text()
old = "#define LOCK_MUTEX0(mutex)\t\tmdb_sem_wait(mutex)\n#define UNLOCK_MUTEX(mutex)\t\tsem_post(mutex)"
new = """extern void __tsan_acquire(void *);
extern void __tsan_release(void *);
static int wt_tsan_sem_wait(sem_t *s);
static int wt_tsan_sem_post(sem_t *s);
#define LOCK_MUTEX0(mutex) wt_tsan_sem_wait(mutex)
#define UNLOCK_MUTEX(mutex) wt_tsan_sem_post(mutex)"""
body = """   while ((rc = sem_wait(sem)) && (rc = errno) == EINTR) ;
   return rc;
}
"""
replacement = body + """
static int wt_tsan_sem_wait(sem_t *s) {
   int rc = mdb_sem_wait(s);
   if (!rc) __tsan_acquire(s);
   return rc;
}
static int wt_tsan_sem_post(sem_t *s) {
   __tsan_release(s);
   return sem_post(s);
}
"""
if source.count(old) != 1 or source.count(body) != 1:
    raise SystemExit("LMDB semaphore source did not match expected version")
Path(sys.argv[2]).write_text(source.replace(old, new).replace(body, replacement))
