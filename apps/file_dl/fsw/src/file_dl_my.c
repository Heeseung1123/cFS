/* Byte-offset file downlink prototype.
 * START -> START segment; every 100 ms -> one DATA segment (RESEND ranges first,
 * then the next bytes in order); after the last byte -> END segment with CRC32. */
#include <string.h>
#include "file_dl_my.h"
#include "file_ul_my.h"

typedef struct
{
    CFE_SB_PipeId_t Pipe;
    osal_id_t       Fd;
    bool            Active; /* first pass over the file is running */
    bool            Open;   /* file stays open after END for RESEND */
    uint32          FileSize;
    uint32          Offset; /* next byte to send */
    uint32          Crc32;  /* running CRC32 of the bytes sent so far */
    uint8           DestTag; /* copied into every segment; 0 = any ground station */
    char            Filename[FILE_DL_MAX_NAME];
    uint32          ResendOffset[FILE_DL_MAX_RESENDS];
    uint32          ResendLength[FILE_DL_MAX_RESENDS];
    uint32          ResendCount;
} FILE_DL_Data_t;

static FILE_DL_Data_t FILE_DL_Data;

// static void FILE_DL_PutU16(uint8 *p, uint16 v)
void FILE_DL_PutU16(uint8 *p, uint16 v)
{
    p[0] = (uint8)(v >> 8);
    p[1] = (uint8)v;
}

uint16 FILE_DL_GetU16(const uint8 *p)
{
    return (uint16)(((uint16)p[0] << 8) | p[1]);
}

// static uint32 FILE_DL_GetU32(const uint8 *p)
uint32 FILE_DL_GetU32(const uint8 *p)
{
    return ((uint32)p[0] << 24) | ((uint32)p[1] << 16) | ((uint32)p[2] << 8) | p[3];
}

// static void FILE_DL_PutU32(uint8 *p, uint32 v)
void FILE_DL_PutU32(uint8 *p, uint32 v)
{
    p[0] = (uint8)(v >> 24);
    p[1] = (uint8)(v >> 16);
    p[2] = (uint8)(v >> 8);
    p[3] = (uint8)v;
}

/* Standard CRC-32 (zlib, reflected 0xEDB88320), updated per segment. */
// static uint32 FILE_DL_Crc32(uint32 crc, const uint8 *p, uint32 len)
uint32 FILE_DL_Crc32(uint32 crc, const uint8 *p, uint32 len)
{
    uint32 i;
    int    k;

    crc = ~crc;
    for (i = 0; i < len; i++)
    {
        crc ^= p[i];
        for (k = 0; k < 8; k++)
        {
            crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
        }
    }
    return ~crc;
}

/* Allocates one segment packet and fills its header; the caller writes the data. */
static CFE_SB_Buffer_t *FILE_DL_NewSegment(uint8 type, uint32 offset, uint16 dataLength, uint8 **ppData)
{
    CFE_SB_Buffer_t  *pBuf;
    FILE_DL_SegHdr_t *pHdr;
    size_t            size = sizeof(CFE_MSG_TelemetryHeader_t) + sizeof(FILE_DL_SegHdr_t) + dataLength;

    pBuf = CFE_SB_AllocateMessageBuffer(size);
    if (pBuf == NULL)
    {
        return NULL;
    }
    CFE_MSG_Init(&pBuf->Msg, CFE_SB_ValueToMsgId(FILE_DL_SEG_TLM_MID), size);

    pHdr       = (FILE_DL_SegHdr_t *)((uint8 *)pBuf + sizeof(CFE_MSG_TelemetryHeader_t));
    pHdr->Type = type;
    // pHdr->Spare = 0;
    pHdr->DestTag = FILE_DL_Data.DestTag;
    FILE_DL_PutU16(pHdr->DataLength, dataLength);
    FILE_DL_PutU32(pHdr->Offset, offset);
    FILE_DL_PutU32(pHdr->FileSize, FILE_DL_Data.FileSize);
    FILE_DL_PutU32(pHdr->Crc32, FILE_DL_Data.Crc32);

    *ppData = (uint8 *)pHdr + sizeof(FILE_DL_SegHdr_t);
    return pBuf;
}

static void FILE_DL_Send(CFE_SB_Buffer_t *pBuf)
{
    CFE_SB_TimeStampMsg(&pBuf->Msg);
    if (CFE_SB_TransmitBuffer(pBuf, true) != CFE_SUCCESS)
    {
        CFE_SB_ReleaseMessageBuffer(pBuf);
    }
}

static void FILE_DL_Close(void)
{
    if (FILE_DL_Data.Open)
    {
        OS_close(FILE_DL_Data.Fd);
    }
    FILE_DL_Data.Open        = false;
    FILE_DL_Data.Active      = false;
    FILE_DL_Data.ResendCount = 0;
}

/* Reads [offset, offset+length) and sends it as one DATA segment; firstPass adds it to the CRC.
 * Returns the bytes sent, 0 if no SB buffer is free now, or -1 on a file error. */
static int32 FILE_DL_SendData(uint32 offset, uint32 length, bool firstPass)
{
    CFE_SB_Buffer_t *pBuf;
    uint8           *pData;

    if (length > FILE_DL_SEGMENT_SIZE)
    {
        length = FILE_DL_SEGMENT_SIZE;
    }

    pBuf = FILE_DL_NewSegment(FILE_DL_TYPE_DATA, offset, (uint16)length, &pData);
    if (pBuf == NULL)
    {
        return 0;
    }

    if ((OS_lseek(FILE_DL_Data.Fd, (int32)offset, OS_SEEK_SET) != (int32)offset) ||
        (OS_read(FILE_DL_Data.Fd, pData, length) != (int32)length))
    {
        CFE_SB_ReleaseMessageBuffer(pBuf);
        CFE_EVS_SendEvent(FILE_DL_FILE_ERR_EID, CFE_EVS_EventType_ERROR, "FILE_DL: read error at offset %lu",
                          (unsigned long)offset);
        return -1;
    }

    if (firstPass)
    {
        FILE_DL_Data.Crc32 = FILE_DL_Crc32(FILE_DL_Data.Crc32, pData, length);
    }
    FILE_DL_Send(pBuf);
    return (int32)length;
}

/* Opens the file and sends the START segment (file size and name).
 * Called by the START command (destTag 0) and by a finished relay upload. */
// static void FILE_DL_Start(const FILE_DL_StartCmd_t *pCmd)
bool FILE_DL_StartFile(const char *filename, uint8 destTag)
{
    os_fstat_t       st;
    CFE_SB_Buffer_t *pBuf;
    uint8           *pData;
    uint16           nameLength;

    if (FILE_DL_Data.Active)
    {
        CFE_EVS_SendEvent(FILE_DL_CMD_ERR_EID, CFE_EVS_EventType_ERROR, "FILE_DL: %s is still being sent",
                          FILE_DL_Data.Filename);
        return false;
    }

    FILE_DL_Close(); /* a finished file is no longer kept for RESEND */
    // CFE_PSP_MemCpy(FILE_DL_Data.Filename, pCmd->Filename, FILE_DL_MAX_NAME);
    strncpy(FILE_DL_Data.Filename, filename, FILE_DL_MAX_NAME - 1);
    FILE_DL_Data.Filename[FILE_DL_MAX_NAME - 1] = '\0';

    if ((OS_stat(FILE_DL_Data.Filename, &st) != OS_SUCCESS) ||
        (OS_OpenCreate(&FILE_DL_Data.Fd, FILE_DL_Data.Filename, OS_FILE_FLAG_NONE, OS_READ_ONLY) != OS_SUCCESS))
    {
        CFE_EVS_SendEvent(FILE_DL_FILE_ERR_EID, CFE_EVS_EventType_ERROR, "FILE_DL: cannot open %s",
                          FILE_DL_Data.Filename);
        return false;
    }

    FILE_DL_Data.FileSize = (uint32)OS_FILESTAT_SIZE(st);
    FILE_DL_Data.Offset   = 0;
    FILE_DL_Data.Crc32    = 0;
    FILE_DL_Data.Active   = true;
    FILE_DL_Data.Open     = true;
    FILE_DL_Data.DestTag  = destTag;

    nameLength = (uint16)strlen(FILE_DL_Data.Filename);
    pBuf       = FILE_DL_NewSegment(FILE_DL_TYPE_START, 0, nameLength, &pData);
    if (pBuf != NULL)
    {
        CFE_PSP_MemCpy(pData, FILE_DL_Data.Filename, nameLength);
        FILE_DL_Send(pBuf);
    }

    CFE_EVS_SendEvent(FILE_DL_START_INF_EID, CFE_EVS_EventType_INFORMATION, "FILE_DL: sending %s, %lu bytes",
                      FILE_DL_Data.Filename, (unsigned long)FILE_DL_Data.FileSize);
    return true;
}

/* START command from the ground: segments are for any ground station. */
static void FILE_DL_Start(const FILE_DL_StartCmd_t *pCmd)
{
    char filename[FILE_DL_MAX_NAME];

    CFE_PSP_MemCpy(filename, pCmd->Filename, FILE_DL_MAX_NAME);
    filename[FILE_DL_MAX_NAME - 1] = '\0';
    FILE_DL_StartFile(filename, 0);
}

/* A file stays open from START until DONE (or the next START); a relay waits for it. */
bool FILE_DL_IsBusy(void)
{
    return FILE_DL_Data.Open;
}

/* Queues a byte range of the open file; FILE_DL_Wakeup sends it before new data. */
static void FILE_DL_Resend(const FILE_DL_ResendCmd_t *pCmd)
{
    uint32 offset = FILE_DL_GetU32(pCmd->Offset);
    uint32 length = FILE_DL_GetU32(pCmd->Length);

    if (!FILE_DL_Data.Open || (offset >= FILE_DL_Data.FileSize) || (length == 0) ||
        (FILE_DL_Data.ResendCount >= FILE_DL_MAX_RESENDS))
    {
        CFE_EVS_SendEvent(FILE_DL_CMD_ERR_EID, CFE_EVS_EventType_ERROR, "FILE_DL: RESEND %lu+%lu rejected",
                          (unsigned long)offset, (unsigned long)length);
        return;
    }
    if (length > FILE_DL_Data.FileSize - offset)
    {
        length = FILE_DL_Data.FileSize - offset;
    }
    FILE_DL_Data.ResendOffset[FILE_DL_Data.ResendCount] = offset;
    FILE_DL_Data.ResendLength[FILE_DL_Data.ResendCount] = length;
    FILE_DL_Data.ResendCount++;
}

/* Each tick (every FILE_DL_SEND_PERIOD_MSEC) sends up to FILE_DL_SEGMENTS_PER_TICK segments:
 * queued RESEND ranges first, then the next bytes of the first pass, then END. */
static void FILE_DL_Wakeup(void)
{
    CFE_SB_Buffer_t *pBuf;
    uint8           *pData;
    int32            sent;
    uint32           i;
    int              budget;

    for (budget = FILE_DL_SEGMENTS_PER_TICK; FILE_DL_Data.Open && (budget > 0); budget--)
    {
        if (FILE_DL_Data.ResendCount > 0)
        {
            sent = FILE_DL_SendData(FILE_DL_Data.ResendOffset[0], FILE_DL_Data.ResendLength[0], false);
            if (sent <= 0)
            {
                break;
            }
            FILE_DL_Data.ResendOffset[0] += (uint32)sent;
            FILE_DL_Data.ResendLength[0] -= (uint32)sent;
            if (FILE_DL_Data.ResendLength[0] == 0)
            {
                /* Range done: shift the queue down by one. */
                for (i = 1; i < FILE_DL_Data.ResendCount; i++)
                {
                    FILE_DL_Data.ResendOffset[i - 1] = FILE_DL_Data.ResendOffset[i];
                    FILE_DL_Data.ResendLength[i - 1] = FILE_DL_Data.ResendLength[i];
                }
                FILE_DL_Data.ResendCount--;
            }
        }
        else if (!FILE_DL_Data.Active)
        {
            break; /* first pass done and nothing to resend */
        }
        else if (FILE_DL_Data.Offset < FILE_DL_Data.FileSize)
        {
            sent = FILE_DL_SendData(FILE_DL_Data.Offset, FILE_DL_Data.FileSize - FILE_DL_Data.Offset, true);
            if (sent < 0)
            {
                FILE_DL_Close();
                break;
            }
            if (sent == 0)
            {
                break; /* no buffer now: same offset next wakeup */
            }
            FILE_DL_Data.Offset += (uint32)sent;
        }
        else
        {
            pBuf = FILE_DL_NewSegment(FILE_DL_TYPE_END, FILE_DL_Data.FileSize, 0, &pData);
            if (pBuf != NULL)
            {
                FILE_DL_Send(pBuf);
            }
            CFE_EVS_SendEvent(FILE_DL_END_INF_EID, CFE_EVS_EventType_INFORMATION,
                              "FILE_DL: %s sent, crc32 0x%08lx", FILE_DL_Data.Filename,
                              (unsigned long)FILE_DL_Data.Crc32);
            FILE_DL_Data.Active = false; /* file stays open for RESEND */
        }
    }
}

/* The ground confirms the whole file: report whether its CRC32 matches, then close
 * the file so it is no longer kept for RESEND. */
static void FILE_DL_Done(const FILE_DL_DoneCmd_t *pCmd)
{
    uint32 groundCrc = FILE_DL_GetU32(pCmd->Crc32);

    if (!FILE_DL_Data.Open || FILE_DL_Data.Active)
    {
        CFE_EVS_SendEvent(FILE_DL_CMD_ERR_EID, CFE_EVS_EventType_ERROR, "FILE_DL: DONE before END rejected");
        return;
    }

    if (groundCrc == FILE_DL_Data.Crc32)
    {
        CFE_EVS_SendEvent(FILE_DL_DONE_INF_EID, CFE_EVS_EventType_INFORMATION,
                          "FILE_DL: %s confirmed by ground, crc32 0x%08lx", FILE_DL_Data.Filename,
                          (unsigned long)groundCrc);
    }
    else
    {
        CFE_EVS_SendEvent(FILE_DL_DONE_ERR_EID, CFE_EVS_EventType_ERROR,
                          "FILE_DL: %s crc32 mismatch, ground 0x%08lx, flight 0x%08lx", FILE_DL_Data.Filename,
                          (unsigned long)groundCrc, (unsigned long)FILE_DL_Data.Crc32);
    }
    FILE_DL_Close();
}

static void FILE_DL_ProcessCommand(const CFE_SB_Buffer_t *pBuf)
{
    CFE_MSG_FcnCode_t cc   = 0;
    size_t            size = 0;

    CFE_MSG_GetFcnCode(&pBuf->Msg, &cc);
    CFE_MSG_GetSize(&pBuf->Msg, &size);

    if ((cc == FILE_DL_NOOP_CC) && (size == sizeof(CFE_MSG_CommandHeader_t)))
    {
        CFE_EVS_SendEvent(FILE_DL_NOOP_INF_EID, CFE_EVS_EventType_INFORMATION, "FILE_DL: NOOP");
    }
    else if ((cc == FILE_DL_START_CC) && (size == sizeof(FILE_DL_StartCmd_t)))
    {
        FILE_DL_Start((const FILE_DL_StartCmd_t *)pBuf);
    }
    else if ((cc == FILE_DL_RESEND_CC) && (size == sizeof(FILE_DL_ResendCmd_t)))
    {
        FILE_DL_Resend((const FILE_DL_ResendCmd_t *)pBuf);
    }
    else if ((cc == FILE_DL_DONE_CC) && (size == sizeof(FILE_DL_DoneCmd_t)))
    {
        FILE_DL_Done((const FILE_DL_DoneCmd_t *)pBuf);
    }
    else if ((cc >= FILE_UL_START_CC) && (cc <= FILE_UL_CANCEL_CC))
    {
        FILE_UL_ProcessCommand(pBuf, cc, size); /* upload commands share the CMD MID */
    }
    else
    {
        CFE_EVS_SendEvent(FILE_DL_CMD_ERR_EID, CFE_EVS_EventType_ERROR, "FILE_DL: bad command cc %u, size %lu",
                          (unsigned int)cc, (unsigned long)size);
    }
}

void FILE_DL_AppMain(void)
{
    uint32           runStatus = CFE_ES_RunStatus_APP_RUN;
    CFE_SB_Buffer_t *pBuf;
    CFE_SB_MsgId_t   msgId;
    int32            status;
    OS_time_t        now;
    OS_time_t        lastTick;
    int64            waitMsec;

    CFE_EVS_Register(NULL, 0, CFE_EVS_EventFilter_BINARY);
    CFE_SB_CreatePipe(&FILE_DL_Data.Pipe, FILE_DL_PIPE_DEPTH, "FILE_DL_PIPE");
    /* CFE_SB_Subscribe allows 4 messages per MID in the pipe, too few for a round of RESENDs. */
    // CFE_SB_Subscribe(CFE_SB_ValueToMsgId(FILE_DL_CMD_MID), FILE_DL_Data.Pipe);
    CFE_SB_SubscribeEx(CFE_SB_ValueToMsgId(FILE_DL_CMD_MID), FILE_DL_Data.Pipe, CFE_SB_DEFAULT_QOS,
                       FILE_DL_PIPE_DEPTH);
    CFE_SB_Subscribe(CFE_SB_ValueToMsgId(FILE_DL_WAKEUP_MID), FILE_DL_Data.Pipe);
    FILE_DL_Data.Active = false;
    FILE_DL_Data.Open   = false;
    FILE_UL_Init();
    CFE_PSP_GetTime(&lastTick);

    CFE_EVS_SendEvent(FILE_DL_INIT_INF_EID, CFE_EVS_EventType_INFORMATION, "FILE_DL Initialized");

    while (CFE_ES_RunLoop(&runStatus))
    {
        /* The tick runs on the clock, not on a receive timeout: upload DATA every 20 ms
         * would otherwise never let the wait time out and the downlink would stop. */
        CFE_PSP_GetTime(&now);
        waitMsec = FILE_DL_SEND_PERIOD_MSEC - OS_TimeGetTotalMilliseconds(OS_TimeSubtract(now, lastTick));
        if (waitMsec <= 0)
        {
            FILE_DL_Wakeup(); /* downlink: one segment */
            FILE_UL_Tick();   /* upload: CRC check, then relay */
            lastTick = now;
            waitMsec = FILE_DL_SEND_PERIOD_MSEC;
        }

        // status = CFE_SB_ReceiveBuffer(&pBuf, FILE_DL_Data.Pipe, FILE_DL_SEND_PERIOD_MSEC);
        // if (status == CFE_SB_TIME_OUT)
        // {
        //     FILE_DL_Wakeup();
        //     continue;
        // }
        status = CFE_SB_ReceiveBuffer(&pBuf, FILE_DL_Data.Pipe, (int32)waitMsec);
        if (status != CFE_SUCCESS)
        {
            continue;
        }

        CFE_MSG_GetMsgId(&pBuf->Msg, &msgId);
        if (CFE_SB_MsgIdToValue(msgId) == FILE_DL_WAKEUP_MID)
        {
            FILE_DL_Wakeup(); /* still accepted if sch_lab sends it again */
        }
        else
        {
            FILE_DL_ProcessCommand(pBuf);
        }
    }

    CFE_ES_ExitApp(runStatus);
}
