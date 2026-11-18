/* Upload and relay of the file_dl app. Both write a file by byte offset, report the
 * missing ranges and check the CRC32 the same way; they differ in MIDs and directory,
 * and a verified relay file is sent down by the downlink engine. */
#ifndef _file_ul_my_h_
#define _file_ul_my_h_

#include <stddef.h>
#include "file_dl_my.h"

#define FILE_UL_DIR           "/cf/inbox" /* uploads (FILE_UL_CMD_MID) are written here */
#define FILE_RELAY_DIR        "/cf/relay" /* relay files (FILE_RELAY_CMD_MID) are written here */
#define FILE_UL_CHUNK_SIZE    960 /* TC frame: 5 + 8 cmd hdr + 8 DATA hdr + 960 = 981 <= 1024 */
#define FILE_UL_MAX_FILE_SIZE (4u * 1024u * 1024u)
#define FILE_UL_MAX_CHUNKS    ((FILE_UL_MAX_FILE_SIZE + FILE_UL_CHUNK_SIZE - 1) / FILE_UL_CHUNK_SIZE)
#define FILE_UL_MAX_GAPS      32 /* missing ranges per status packet */
#define FILE_UL_VERIFY_CHUNKS 16 /* chunks read back per 100 ms tick for the CRC check */
#define FILE_UL_RELAY_QUEUE   4  /* verified relay files waiting for the downlink */

/* Command codes, the same on FILE_UL_CMD_MID and FILE_RELAY_CMD_MID */
// #define FILE_UL_START_CC  4
// #define FILE_UL_DATA_CC   5
// #define FILE_UL_END_CC    6
// #define FILE_UL_STATUS_CC 7
// #define FILE_UL_CANCEL_CC 8
#define FILE_UL_NOOP_CC   0
#define FILE_UL_START_CC  1
#define FILE_UL_DATA_CC   2
#define FILE_UL_END_CC    3
#define FILE_UL_STATUS_CC 4
#define FILE_UL_CANCEL_CC 5

/* States in the status packet */
#define FILE_UL_STATE_IDLE      0
#define FILE_UL_STATE_RECEIVING 1
#define FILE_UL_STATE_VERIFYING 2
#define FILE_UL_STATE_COMPLETE  3
#define FILE_UL_STATE_FAILED    4

/* Event IDs, after FILE_DL's 1..8; the text starts with FILE_UL or FILE_RELAY */
#define FILE_UL_START_INF_EID    9
#define FILE_UL_CMD_ERR_EID      10
#define FILE_UL_FILE_ERR_EID     11
#define FILE_UL_COMPLETE_INF_EID 12
#define FILE_UL_CRC_ERR_EID      13
#define FILE_UL_RELAY_INF_EID    14
#define FILE_UL_CANCEL_INF_EID   15

/* Upload START: where to store the file (under /cf/inbox/), its size and CRC32. */
typedef struct
{
    CFE_MSG_CommandHeader_t CommandHeader;
    char                    Filename[FILE_DL_MAX_NAME];
    uint8                   FileSize[4];
    uint8                   Crc32[4];
} FILE_UL_StartCmd_t;

/* Relay START: as upload START (under /cf/relay/), plus the ground station tag
 * written into every downlink segment of this file. */
typedef struct
{
    CFE_MSG_CommandHeader_t CommandHeader;
    char                    Filename[FILE_DL_MAX_NAME];
    uint8                   FileSize[4];
    uint8                   Crc32[4];
    uint8                   DestTag;
    uint8                   Spare[3];
} FILE_RELAY_StartCmd_t;

/* DATA (upload and relay): one chunk at Offset, a multiple of FILE_UL_CHUNK_SIZE.
 * Only DataLength bytes of Data are sent; the last chunk may be shorter. */
typedef struct
{
    CFE_MSG_CommandHeader_t CommandHeader;
    uint8                   Offset[4];
    uint8                   DataLength[2];
    uint8                   Spare[2];
    uint8                   Data[FILE_UL_CHUNK_SIZE];
} FILE_UL_DataCmd_t;

#define FILE_UL_DATA_HDR_SIZE offsetof(FILE_UL_DataCmd_t, Data)

typedef struct
{
    uint8 Offset[4];
    uint8 Length[4];
} FILE_UL_Gap_t;

/* Status telemetry (FILE_UL_STATUS_TLM_MID or FILE_RELAY_STATUS_TLM_MID): sent on END,
 * STATUS, CANCEL and after the CRC check. TotalGaps may exceed the GapCount listed. */
typedef struct
{
    CFE_MSG_TelemetryHeader_t TelemetryHeader;
    uint8                     State;
    uint8                     Relay; /* 1 on the relay MID */
    uint8                     DestTag;
    uint8                     GapCount;
    uint8                     FileSize[4];
    uint8                     ReceivedBytes[4];
    uint8                     ExpectedCrc[4]; /* from START */
    uint8                     FileCrc[4];     /* read back from the stored file */
    uint8                     TotalGaps[4];
    char                      Filename[FILE_DL_MAX_NAME];
    FILE_UL_Gap_t             Gaps[FILE_UL_MAX_GAPS];
} FILE_UL_StatusTlm_t;

void FILE_UL_Init(void);
void FILE_UL_Tick(void);
// void FILE_UL_ProcessCommand(const CFE_SB_Buffer_t *pBuf, CFE_MSG_FcnCode_t cc, size_t size);
void FILE_UL_ProcessCommand(const CFE_SB_Buffer_t *pBuf);    /* FILE_UL_CMD_MID */
void FILE_RELAY_ProcessCommand(const CFE_SB_Buffer_t *pBuf); /* FILE_RELAY_CMD_MID */

#endif /* _file_ul_my_h_ */
