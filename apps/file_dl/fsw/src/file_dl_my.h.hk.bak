/* Byte-offset file downlink prototype: a file is cut into segments, each one
 * carrying its byte offset, so the ground can put them back in any order.
 * The ground asks again for missing byte ranges with RESEND (offset, length). */
#ifndef _file_dl_my_h_
#define _file_dl_my_h_

#include "cfe.h"
#include "file_dl_msgids.h"

#define FILE_DL_PIPE_DEPTH          16
#define FILE_DL_SEGMENT_SIZE        1024 /* 16 + 16 + 1024 fits one TM data field (1766) */
/* Segments per 1 Hz wakeup. On native Linux every SB pipe holds only 10 messages
 * (/proc/sys/fs/mqueue/msg_max), so some segments can be lost; RESEND covers them. */
#define FILE_DL_SEGMENTS_PER_WAKEUP 8
#define FILE_DL_MAX_RESENDS         16 /* pending RESEND ranges */
#define FILE_DL_MAX_NAME            64

/* Command codes */
#define FILE_DL_NOOP_CC  0
#define FILE_DL_START_CC  1
#define FILE_DL_RESEND_CC 2
#define FILE_DL_DONE_CC   3 /* ground has the whole file: compare CRC32 and close it */

/* Segment types */
#define FILE_DL_TYPE_START 0 /* FileSize, Data = file name */
#define FILE_DL_TYPE_DATA  1 /* Offset, DataLength, Data = file bytes */
#define FILE_DL_TYPE_END   2 /* FileSize, Crc32 of the whole file */

/* Event IDs */
#define FILE_DL_INIT_INF_EID  1
#define FILE_DL_CMD_ERR_EID   2
#define FILE_DL_START_INF_EID 3
#define FILE_DL_FILE_ERR_EID  4
#define FILE_DL_END_INF_EID   5
#define FILE_DL_NOOP_INF_EID  6
#define FILE_DL_DONE_INF_EID  7
#define FILE_DL_DONE_ERR_EID  8

/* START command: the file to send, e.g. "/cf/data.bin". */
typedef struct
{
    CFE_MSG_CommandHeader_t CommandHeader;
    char                    Filename[FILE_DL_MAX_NAME];
} FILE_DL_StartCmd_t;

/* RESEND command: big-endian byte range of the current file to send again. */
typedef struct
{
    CFE_MSG_CommandHeader_t CommandHeader;
    uint8                   Offset[4];
    uint8                   Length[4];
} FILE_DL_ResendCmd_t;

/* DONE command: CRC32 of the file the ground rebuilt, big-endian. */
typedef struct
{
    CFE_MSG_CommandHeader_t CommandHeader;
    uint8                   Crc32[4];
} FILE_DL_DoneCmd_t;

/* Segment header after the telemetry header. Multi-byte fields are big-endian
 * byte arrays, so the layout is the same on every CPU. */
typedef struct
{
    uint8 Type;
    uint8 Spare;
    uint8 DataLength[2];
    uint8 Offset[4];
    uint8 FileSize[4];
    uint8 Crc32[4];
} FILE_DL_SegHdr_t;

void FILE_DL_AppMain(void);

#endif /* _file_dl_my_h_ */
