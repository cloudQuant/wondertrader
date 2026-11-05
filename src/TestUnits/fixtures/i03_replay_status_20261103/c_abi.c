#include "Includes/ReplayStatusV1.h"
#include <stddef.h>
_Static_assert(sizeof(((WtReplayStatusV1*)0)->generation) == 8, "generation width");
_Static_assert(sizeof(((WtReplayStatusV1*)0)->state) == 4, "state width");
_Static_assert(sizeof(((WtReplayStatusV1*)0)->flags) == 4, "flags width");
_Static_assert(sizeof(((WtReplayStatusV1*)0)->message) == 512, "message capacity");
_Static_assert(offsetof(WtReplayStatusV1, flags) == 24, "flags offset");
_Static_assert(offsetof(WtReplayStatusV1, message) == 28, "message offset");
_Static_assert(WT_REPLAY_STATUS_FINAL == 1u, "final flag");
int main(void) { WtReplayStatusV1 s = {0}; return (int)s.state; }
