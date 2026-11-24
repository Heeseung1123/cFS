/* Byte-offset file downlink prototype: a file is cut into segments, each one
 * carrying its byte offset, so the ground can put them back in any order.
 * The ground asks again for missing byte ranges with RESEND (offset, length). */
#ifndef _file_dl_my_h_
#define _file_dl_my_h_

#include "cfe.h"
#include "file_dl_msgids.h"

// #define FILE_DL_PIPE_DEPTH          16
/* Also the SB message limit of the command MID; uploads add DATA commands to the pipe. */
#define FILE_DL_PIPE_DEPTH          32
#define FILE_DL_SEGMENT_SIZE        1024 /* 16 + 16 + 1024 fits one TM data field (1780) */
/* One segment every 100 ms (about 10 KiB/s) instead of a burst per 1 Hz wakeup, so at
 * most one segment waits in TO_LAB's pipe per 100 ms TO cycle, even next to HK. */
// #define FILE_DL_SEGMENTS_PER_WAKEUP 8
#define FILE_DL_SEND_PERIOD_MSEC  100
#define FILE_DL_SEGMENTS_PER_TICK 1
// #define FILE_DL_MAX_RESENDS         16 /* pending RESEND ranges */
#define FILE_DL_MAX_RESENDS 64 /* pending RESEND ranges, 8 bytes each */
#define FILE_DL_MAX_NAME            64

/* Command codes */
#define FILE_DL_NOOP_CC  0
#define FILE_DL_START_CC  1
#define FILE_DL_RESEND_CC 2
#define FILE_DL_DONE_CC   3 /* ground has the whole file: compare CRC32 and close it */
// /* Upload command codes 4..8 are in file_ul_my.h. */
/* Upload and relay have their own MIDs and command codes (file_ul_my.h). */

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
/* Upload and relay event IDs 9..15 are in file_ul_my.h. */

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
    // uint8 Spare;
    uint8 DestTag; /* was Spare (0): ground station the file is for, 0 = any */
    uint8 DataLength[2];
    uint8 Offset[4];
    uint8 FileSize[4];
    uint8 Crc32[4];
} FILE_DL_SegHdr_t;

void FILE_DL_AppMain(void);

/* Shared with file_ul_my.c (upload and relay). */
void   FILE_DL_PutU16(uint8 *p, uint16 v);
uint16 FILE_DL_GetU16(const uint8 *p);
uint32 FILE_DL_GetU32(const uint8 *p);
void   FILE_DL_PutU32(uint8 *p, uint32 v);
uint32 FILE_DL_Crc32(uint32 crc, const uint8 *p, uint32 len);
// bool   FILE_DL_StartFile(const char *filename, uint8 destTag); /* false if busy or no file */
/* Sends a file with segments on segMid; false if busy or no file. */
bool FILE_DL_StartFile(const char *filename, uint8 destTag, uint16 segMid);
// bool FILE_DL_IsBusy(void); /* a file is open for sending */
bool FILE_DL_IsBusy(void); /* a file is in its first pass (START to END) */

#endif /* _file_dl_my_h_ */
