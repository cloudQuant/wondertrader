#pragma once
#include <stdint.h>

/* Fixed numeric values. Fields in the V1 snapshot have fixed widths. */
enum WtReplayStateV1 {
    WT_REPLAY_IDLE = 0,
    WT_REPLAY_PREPARING = 1,
    WT_REPLAY_PREPARED = 2,
    WT_REPLAY_RUNNING = 3,
    WT_REPLAY_SUCCEEDED = 4,
    WT_REPLAY_STOPPED = 5,
    WT_REPLAY_FAILED = 6
};
enum WtReplayErrorV1 {
    WT_REPLAY_ERROR_NONE = 0,
    WT_REPLAY_ERROR_INVALID_RANGE = 1,
    WT_REPLAY_ERROR_NO_LISTENER = 2,
    WT_REPLAY_ERROR_FEE = 3,
    WT_REPLAY_ERROR_NO_SOURCE = 4,
    WT_REPLAY_ERROR_DATE_MAPPING = 5,
    WT_REPLAY_ERROR_INTERNAL = 6,
    WT_REPLAY_ERROR_CALLBACK_EXCEPTION = 7
};
/* FINAL is set only after completion hook and terminal notification return,
 * and running admission has reopened. Terminal state alone can change if a
 * completion callback throws. report_replay_error ignores finalizing runs. */
enum { WT_REPLAY_STATUS_FINAL = 1u };
typedef struct WtReplayStatusV1 {
    uint32_t struct_size;
    uint32_t abi_version;
    uint64_t generation;
    uint32_t state;
    uint32_t error_code;
    uint32_t flags;
    char message[512];
} WtReplayStatusV1;
