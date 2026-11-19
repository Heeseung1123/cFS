/* Byte-offset upload and relay, part of the file_dl app. One engine, two instances:
 * upload (/cf/inbox) and relay (/cf/relay). START opens <name>.part -> DATA writes chunks
 * -> END reports gaps or starts the CRC check -> rename; a relay file then goes down. */
#include <stdio.h>
#include <string.h>
#include "file_ul_my.h"

typedef struct
{
    const char *Name;      /* event prefix: "FILE_UL" or "FILE_RELAY" */
    const char *Dir;       /* files may only be written here */
    uint16      StatusMid; /* FILE_UL_STATUS_TLM_MID or FILE_RELAY_STATUS_TLM_MID */
    bool        Relay;     /* a verified file is queued for the downlink */
    uint8       State;
    bool        Open;
    osal_id_t   Fd;
    char        Filename[FILE_DL_MAX_NAME];
    char        PartName[FILE_DL_MAX_NAME + 8];
    uint32      FileSize;
    uint32      ExpectedCrc;
    uint32      FileCrc; /* CRC32 of the bytes read back so far */
    uint32      ChunkCount;
    uint32      ReceivedChunks;
    uint32      VerifyChunk; /* next chunk to read back */
    uint8       DestTag;
    uint8       Bitmap[(FILE_UL_MAX_CHUNKS + 7) / 8]; /* one bit per received chunk */
    uint8       Chunk[FILE_UL_CHUNK_SIZE];
    FILE_UL_StatusTlm_t Status;
} FILE_UL_Ctx_t;

static FILE_UL_Ctx_t FILE_UL_Upload;
static FILE_UL_Ctx_t FILE_UL_Relay;

/* Verified relay files waiting until the downlink engine is free. */
static struct
{
    char   Name[FILE_UL_RELAY_QUEUE][FILE_DL_MAX_NAME];
    uint8  Tag[FILE_UL_RELAY_QUEUE];
    uint32 Count;
} FILE_UL_RelayQueue;

static bool FILE_UL_IsSet(const FILE_UL_Ctx_t *c, uint32 i)
{
    return (c->Bitmap[i / 8] & (1u << (i % 8))) != 0;
}

/* Bytes in chunk i; only the last chunk may be shorter than FILE_UL_CHUNK_SIZE. */
static uint32 FILE_UL_ChunkLength(const FILE_UL_Ctx_t *c, uint32 i)
{
    uint32 left = c->FileSize - i * FILE_UL_CHUNK_SIZE;

    return (left < FILE_UL_CHUNK_SIZE) ? left : FILE_UL_CHUNK_SIZE;
}

/* Sends the status packet: state, CRCs and the first FILE_UL_MAX_GAPS missing byte ranges. */
static void FILE_UL_SendStatus(FILE_UL_Ctx_t *c)
{
    FILE_UL_StatusTlm_t *pTlm   = &c->Status;
    uint32               total  = 0;
    uint32               listed = 0;
    uint32               i      = 0;
    uint32               first;
    uint32               end;

    memset(pTlm, 0, sizeof(*pTlm));
    CFE_MSG_Init(CFE_MSG_PTR(pTlm->TelemetryHeader), CFE_SB_ValueToMsgId(c->StatusMid), sizeof(*pTlm));
    pTlm->State   = c->State;
    pTlm->Relay   = c->Relay ? 1 : 0;
    pTlm->DestTag = c->DestTag;
    FILE_DL_PutU32(pTlm->FileSize, c->FileSize);
    FILE_DL_PutU32(pTlm->ExpectedCrc, c->ExpectedCrc);
    FILE_DL_PutU32(pTlm->FileCrc, c->FileCrc);
    memcpy(pTlm->Filename, c->Filename, sizeof(pTlm->Filename));

    /* A run of 0 bits is one missing range. */
    while (i < c->ChunkCount)
    {
        if (FILE_UL_IsSet(c, i))
        {
            i++;
            continue;
        }
        first = i;
        while ((i < c->ChunkCount) && !FILE_UL_IsSet(c, i))
        {
            i++;
        }
        if (listed < FILE_UL_MAX_GAPS)
        {
            end = i * FILE_UL_CHUNK_SIZE;
            if (end > c->FileSize)
            {
                end = c->FileSize;
            }
            FILE_DL_PutU32(pTlm->Gaps[listed].Offset, first * FILE_UL_CHUNK_SIZE);
            FILE_DL_PutU32(pTlm->Gaps[listed].Length, end - first * FILE_UL_CHUNK_SIZE);
            listed++;
        }
        total++;
    }

    /* Received bytes: every chunk is full except maybe the last one. */
    end = c->ReceivedChunks * FILE_UL_CHUNK_SIZE;
    if ((c->ChunkCount > 0) && FILE_UL_IsSet(c, c->ChunkCount - 1))
    {
        end -= FILE_UL_CHUNK_SIZE - FILE_UL_ChunkLength(c, c->ChunkCount - 1);
    }
    FILE_DL_PutU32(pTlm->ReceivedBytes, end);
    pTlm->GapCount = (uint8)listed;
    FILE_DL_PutU32(pTlm->TotalGaps, total);

    CFE_SB_TimeStampMsg(CFE_MSG_PTR(pTlm->TelemetryHeader));
    CFE_SB_TransmitMsg(CFE_MSG_PTR(pTlm->TelemetryHeader), true);
}

static void FILE_UL_CloseFile(FILE_UL_Ctx_t *c)
{
    if (c->Open)
    {
        OS_close(c->Fd);
    }
    c->Open = false;
}

static void FILE_UL_Fail(FILE_UL_Ctx_t *c)
{
    FILE_UL_CloseFile(c);
    c->State = FILE_UL_STATE_FAILED;
    FILE_UL_SendStatus(c);
}

/* Only plain names directly under the instance's directory, so other files are safe. */
static bool FILE_UL_IsValidName(const FILE_UL_Ctx_t *c, const char *name)
{
    size_t dirLength = strlen(c->Dir);

    return (strncmp(name, c->Dir, dirLength) == 0) && (name[dirLength] == '/') && (name[dirLength + 1] != '\0') &&
           (strstr(name, "..") == NULL) && (strchr(name + dirLength + 1, '/') == NULL);
}

/* START: checks the name and size, then creates (or empties) <name>.part. */
static void FILE_UL_Start(FILE_UL_Ctx_t *c, const char *cmdName, uint32 size, uint32 crc, uint8 destTag)
{
    char name[FILE_DL_MAX_NAME];

    if ((c->State == FILE_UL_STATE_RECEIVING) || (c->State == FILE_UL_STATE_VERIFYING))
    {
        CFE_EVS_SendEvent(FILE_UL_CMD_ERR_EID, CFE_EVS_EventType_ERROR, "%s: %s is still being received", c->Name,
                          c->Filename);
        return;
    }

    CFE_PSP_MemCpy(name, cmdName, FILE_DL_MAX_NAME);
    name[FILE_DL_MAX_NAME - 1] = '\0';
    if (!FILE_UL_IsValidName(c, name) || (size == 0) || (size > FILE_UL_MAX_FILE_SIZE))
    {
        CFE_EVS_SendEvent(FILE_UL_CMD_ERR_EID, CFE_EVS_EventType_ERROR, "%s: START %s, %lu bytes rejected", c->Name,
                          name, (unsigned long)size);
        return;
    }

    snprintf(c->PartName, sizeof(c->PartName), "%s.part", name);
    if (OS_OpenCreate(&c->Fd, c->PartName, OS_FILE_FLAG_CREATE | OS_FILE_FLAG_TRUNCATE, OS_READ_WRITE) !=
        OS_SUCCESS)
    {
        CFE_EVS_SendEvent(FILE_UL_FILE_ERR_EID, CFE_EVS_EventType_ERROR, "%s: cannot create %s", c->Name,
                          c->PartName);
        return;
    }

    memcpy(c->Filename, name, sizeof(c->Filename));
    memset(c->Bitmap, 0, sizeof(c->Bitmap));
    c->Open           = true;
    c->FileSize       = size;
    c->ExpectedCrc    = crc;
    c->FileCrc        = 0;
    c->ChunkCount     = (size + FILE_UL_CHUNK_SIZE - 1) / FILE_UL_CHUNK_SIZE;
    c->ReceivedChunks = 0;
    c->DestTag        = destTag;
    c->State          = FILE_UL_STATE_RECEIVING;

    CFE_EVS_SendEvent(FILE_UL_START_INF_EID, CFE_EVS_EventType_INFORMATION, "%s: receiving %s, %lu bytes, tag %u",
                      c->Name, name, (unsigned long)size, (unsigned int)destTag);
}

/* DATA: writes one chunk at its offset; a chunk received twice is ignored. */
static void FILE_UL_WriteChunk(FILE_UL_Ctx_t *c, const FILE_UL_DataCmd_t *pCmd, size_t size)
{
    uint32 offset = FILE_DL_GetU32(pCmd->Offset);
    uint32 length = FILE_DL_GetU16(pCmd->DataLength);
    uint32 i      = offset / FILE_UL_CHUNK_SIZE;

    if ((c->State != FILE_UL_STATE_RECEIVING) || (size != FILE_UL_DATA_HDR_SIZE + length) ||
        ((offset % FILE_UL_CHUNK_SIZE) != 0) || (i >= c->ChunkCount) || (length != FILE_UL_ChunkLength(c, i)))
    {
        CFE_EVS_SendEvent(FILE_UL_CMD_ERR_EID, CFE_EVS_EventType_ERROR, "%s: DATA %lu+%lu rejected", c->Name,
                          (unsigned long)offset, (unsigned long)length);
        return;
    }
    if (FILE_UL_IsSet(c, i))
    {
        return;
    }

    if ((OS_lseek(c->Fd, (int32)offset, OS_SEEK_SET) != (int32)offset) ||
        (OS_write(c->Fd, pCmd->Data, length) != (int32)length))
    {
        CFE_EVS_SendEvent(FILE_UL_FILE_ERR_EID, CFE_EVS_EventType_ERROR, "%s: write error at offset %lu", c->Name,
                          (unsigned long)offset);
        FILE_UL_Fail(c);
        return;
    }
    c->Bitmap[i / 8] |= (uint8)(1u << (i % 8));
    c->ReceivedChunks++;
}

/* END: reports the gaps; with none left the CRC check starts on the next ticks. */
static void FILE_UL_End(FILE_UL_Ctx_t *c)
{
    if ((c->State == FILE_UL_STATE_RECEIVING) && (c->ReceivedChunks == c->ChunkCount))
    {
        c->State       = FILE_UL_STATE_VERIFYING;
        c->VerifyChunk = 0;
        c->FileCrc     = 0;
    }
    FILE_UL_SendStatus(c);
}

/* CANCEL: drops the transfer and its .part file. */
static void FILE_UL_Cancel(FILE_UL_Ctx_t *c)
{
    FILE_UL_CloseFile(c);
    if ((c->State != FILE_UL_STATE_IDLE) && (c->State != FILE_UL_STATE_COMPLETE))
    {
        OS_remove(c->PartName);
    }
    c->State = FILE_UL_STATE_IDLE;
    CFE_EVS_SendEvent(FILE_UL_CANCEL_INF_EID, CFE_EVS_EventType_INFORMATION, "%s: %s cancelled", c->Name,
                      c->Filename);
    FILE_UL_SendStatus(c);
}

/* All bytes read back: compare the CRC, rename <name>.part to <name>; a relay file
 * is queued for the downlink. */
static void FILE_UL_Finish(FILE_UL_Ctx_t *c)
{
    FILE_UL_CloseFile(c);
    if (c->FileCrc != c->ExpectedCrc)
    {
        CFE_EVS_SendEvent(FILE_UL_CRC_ERR_EID, CFE_EVS_EventType_ERROR, "%s: %s crc32 mismatch, file 0x%08lx, ground 0x%08lx",
                          c->Name, c->Filename, (unsigned long)c->FileCrc, (unsigned long)c->ExpectedCrc);
        FILE_UL_Fail(c);
        return;
    }

    OS_remove(c->Filename); /* an older file of the same name is replaced */
    if (OS_rename(c->PartName, c->Filename) != OS_SUCCESS)
    {
        CFE_EVS_SendEvent(FILE_UL_FILE_ERR_EID, CFE_EVS_EventType_ERROR, "%s: cannot rename %s", c->Name,
                          c->PartName);
        FILE_UL_Fail(c);
        return;
    }

    c->State = FILE_UL_STATE_COMPLETE;
    CFE_EVS_SendEvent(FILE_UL_COMPLETE_INF_EID, CFE_EVS_EventType_INFORMATION,
                      "%s: %s received and verified, crc32 0x%08lx", c->Name, c->Filename,
                      (unsigned long)c->FileCrc);

    if (c->Relay)
    {
        if (FILE_UL_RelayQueue.Count < FILE_UL_RELAY_QUEUE)
        {
            memcpy(FILE_UL_RelayQueue.Name[FILE_UL_RelayQueue.Count], c->Filename, FILE_DL_MAX_NAME);
            FILE_UL_RelayQueue.Tag[FILE_UL_RelayQueue.Count] = c->DestTag;
            FILE_UL_RelayQueue.Count++;
        }
        else
        {
            CFE_EVS_SendEvent(FILE_UL_CMD_ERR_EID, CFE_EVS_EventType_ERROR, "%s: queue full, %s kept", c->Name,
                              c->Filename);
        }
    }
    FILE_UL_SendStatus(c);
}

/* Reads back FILE_UL_VERIFY_CHUNKS chunks for the CRC; in steps, so the pipe is
 * still served while a large file is checked. */
static void FILE_UL_Verify(FILE_UL_Ctx_t *c)
{
    uint32 k;
    uint32 length;
    uint32 offset;

    for (k = 0; (c->State == FILE_UL_STATE_VERIFYING) && (k < FILE_UL_VERIFY_CHUNKS); k++)
    {
        if (c->VerifyChunk >= c->ChunkCount)
        {
            FILE_UL_Finish(c);
            break;
        }
        offset = c->VerifyChunk * FILE_UL_CHUNK_SIZE;
        length = FILE_UL_ChunkLength(c, c->VerifyChunk);
        if ((OS_lseek(c->Fd, (int32)offset, OS_SEEK_SET) != (int32)offset) ||
            (OS_read(c->Fd, c->Chunk, length) != (int32)length))
        {
            CFE_EVS_SendEvent(FILE_UL_FILE_ERR_EID, CFE_EVS_EventType_ERROR, "%s: read error at offset %lu", c->Name,
                              (unsigned long)offset);
            FILE_UL_Fail(c);
            break;
        }
        c->FileCrc = FILE_DL_Crc32(c->FileCrc, c->Chunk, length);
        c->VerifyChunk++;
    }
}

/* The next verified relay file goes down on FILE_RELAY_SEG_TLM_MID once no
 * first pass is running; a file still waiting for DONE is closed. */
static void FILE_UL_RelayNext(void)
{
    uint32 i;

    if ((FILE_UL_RelayQueue.Count == 0) || FILE_DL_IsBusy())
    {
        return;
    }
    if (FILE_DL_StartFile(FILE_UL_RelayQueue.Name[0], FILE_UL_RelayQueue.Tag[0], FILE_RELAY_SEG_TLM_MID))
    {
        CFE_EVS_SendEvent(FILE_UL_RELAY_INF_EID, CFE_EVS_EventType_INFORMATION, "FILE_RELAY: sending %s to tag %u",
                          FILE_UL_RelayQueue.Name[0], (unsigned int)FILE_UL_RelayQueue.Tag[0]);
    }
    for (i = 1; i < FILE_UL_RelayQueue.Count; i++)
    {
        memcpy(FILE_UL_RelayQueue.Name[i - 1], FILE_UL_RelayQueue.Name[i], FILE_DL_MAX_NAME);
        FILE_UL_RelayQueue.Tag[i - 1] = FILE_UL_RelayQueue.Tag[i];
    }
    FILE_UL_RelayQueue.Count--;
}

static void FILE_UL_InitCtx(FILE_UL_Ctx_t *c, const char *name, const char *dir, uint16 statusMid, bool relay)
{
    memset(c, 0, sizeof(*c));
    c->Name      = name;
    c->Dir       = dir;
    c->StatusMid = statusMid;
    c->Relay     = relay;
    c->State     = FILE_UL_STATE_IDLE;
    OS_mkdir(dir, OS_READ_WRITE); /* fails harmlessly if it already exists */
}

void FILE_UL_Init(void)
{
    FILE_UL_InitCtx(&FILE_UL_Upload, "FILE_UL", FILE_UL_DIR, FILE_UL_STATUS_TLM_MID, false);
    FILE_UL_InitCtx(&FILE_UL_Relay, "FILE_RELAY", FILE_RELAY_DIR, FILE_RELAY_STATUS_TLM_MID, true);
    memset(&FILE_UL_RelayQueue, 0, sizeof(FILE_UL_RelayQueue));
}

/* Every 100 ms: CRC check steps of both instances, then a waiting relay. */
void FILE_UL_Tick(void)
{
    FILE_UL_Verify(&FILE_UL_Upload);
    FILE_UL_Verify(&FILE_UL_Relay);
    FILE_UL_RelayNext();
}

/* Same command codes on both MIDs; only START differs (relay START has DestTag). */
static void FILE_UL_Dispatch(FILE_UL_Ctx_t *c, const CFE_SB_Buffer_t *pBuf)
{
    CFE_MSG_FcnCode_t cc   = 0;
    size_t            size = 0;

    CFE_MSG_GetFcnCode(&pBuf->Msg, &cc);
    CFE_MSG_GetSize(&pBuf->Msg, &size);

    if ((cc == FILE_UL_NOOP_CC) && (size == sizeof(CFE_MSG_CommandHeader_t)))
    {
        CFE_EVS_SendEvent(FILE_UL_START_INF_EID, CFE_EVS_EventType_INFORMATION, "%s: NOOP", c->Name);
    }
    else if ((cc == FILE_UL_START_CC) && !c->Relay && (size == sizeof(FILE_UL_StartCmd_t)))
    {
        const FILE_UL_StartCmd_t *pCmd = (const FILE_UL_StartCmd_t *)pBuf;
        FILE_UL_Start(c, pCmd->Filename, FILE_DL_GetU32(pCmd->FileSize), FILE_DL_GetU32(pCmd->Crc32), 0);
    }
    else if ((cc == FILE_UL_START_CC) && c->Relay && (size == sizeof(FILE_RELAY_StartCmd_t)))
    {
        const FILE_RELAY_StartCmd_t *pCmd = (const FILE_RELAY_StartCmd_t *)pBuf;
        FILE_UL_Start(c, pCmd->Filename, FILE_DL_GetU32(pCmd->FileSize), FILE_DL_GetU32(pCmd->Crc32),
                      pCmd->DestTag);
    }
    else if ((cc == FILE_UL_DATA_CC) && (size > FILE_UL_DATA_HDR_SIZE) && (size <= sizeof(FILE_UL_DataCmd_t)))
    {
        FILE_UL_WriteChunk(c, (const FILE_UL_DataCmd_t *)pBuf, size);
    }
    else if ((cc == FILE_UL_END_CC) && (size == sizeof(CFE_MSG_CommandHeader_t)))
    {
        FILE_UL_End(c);
    }
    else if ((cc == FILE_UL_STATUS_CC) && (size == sizeof(CFE_MSG_CommandHeader_t)))
    {
        FILE_UL_SendStatus(c);
    }
    else if ((cc == FILE_UL_CANCEL_CC) && (size == sizeof(CFE_MSG_CommandHeader_t)))
    {
        FILE_UL_Cancel(c);
    }
    else
    {
        CFE_EVS_SendEvent(FILE_UL_CMD_ERR_EID, CFE_EVS_EventType_ERROR, "%s: bad command cc %u, size %lu", c->Name,
                          (unsigned int)cc, (unsigned long)size);
    }
}

void FILE_UL_ProcessCommand(const CFE_SB_Buffer_t *pBuf)
{
    FILE_UL_Dispatch(&FILE_UL_Upload, pBuf);
}

void FILE_RELAY_ProcessCommand(const CFE_SB_Buffer_t *pBuf)
{
    FILE_UL_Dispatch(&FILE_UL_Relay, pBuf);
}
