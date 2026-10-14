#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stddef.h>

#define TEAM_PREFIX "T9"
#define DEFAULT_PORT 5000
#define DEFAULT_K 1
#define MAX_JOBS 4096
#define MAX_CMD_LEN 8192
#define MAX_OUTPUT_SIZE (1024 * 1024) /* 1 MiB output capture (min required is 64 KiB) */
#define BUFFER_SIZE 16384
#define AGING_INTERVAL_SEC 5 /* Priority with aging: +1 priority per 5s queued */

/* Job States */
typedef enum {
    STATE_UNKNOWN = 0,
    STATE_QUEUED,
    STATE_RUNNING,
    STATE_DONE,
    STATE_KILLED_TIMEOUT,
    STATE_KILLED_SIGNAL,
    STATE_CANCELLED
} JobState;

/* Protocol Messages between Client and Server */
/*
   Client -> Server:
     SUBMIT <timeout> <priority> <cmd_length>\n<cmd_bytes>
     STATUS <id>\n
     CANCEL <id>\n
     FETCH <id>\n

   Server -> Client:
     JOB_ID <id>\n
     STATUS_REPLY <state_string>\n   (e.g., QUEUED, RUNNING, DONE 0, KILLED TIMEOUT, KILLED SIGNAL 9, CANCELLED)
     CANCEL_REPLY <OK|ALREADY_FINISHED|UNKNOWN>\n
     FETCH_REPLY <OK|NOT_FINISHED|UNKNOWN> [length]\n[binary_data]
     ERROR <message>\n
*/

/* Protocol Messages between Server and Worker */
/*
   Worker -> Server:
     WORKER_HELLO <K>\n
     REPORT <id> <STATE_NAME> <extra_int> <output_len>\n<output_bytes>
       e.g., REPORT T12-1 DONE 0 6\nhello\n
             REPORT T12-2 KILLED_TIMEOUT 0 0\n
             REPORT T12-3 KILLED_SIGNAL 9 0\n
             REPORT T12-4 CANCELLED 0 0\n

   Server -> Worker:
     WORKER_OK\n
     DISPATCH <id> <timeout> <cmd_len>\n<cmd_bytes>
     CANCEL_JOB <id>\n
*/

#endif /* PROTOCOL_H */
