/* Byte-offset file upload and relay, part of the file_dl app.
 * UL_START opens <name>.part -> UL_DATA writes chunks at their offsets -> UL_END reports
 * gaps or starts the CRC check -> rename to <name> -> optional relay to the downlink. */
#include <stdio.h>
#include <string.h>
#include "file_ul_my.h"

typedef struct
{
    uint8     State;
    bool      Open;
    osal_id_t Fd;
    char      Filename[FILE_DL_MAX_NAME];
    char      PartName[FILE_DL_MAX_NAME + 8];
    uint32    FileSize;
    uint32    ExpectedCrc;
    uint32    FileCrc; /* CRC32 of the bytes read back so far */
    uint32    ChunkCount;
    uint32    ReceivedChunks;
    uint32    VerifyChunk; /* next chunk to read back */
    uint8     Relay;
    uint8     DestTag;
    uint8     Bitmap[(FILE_UL_MAX_CHUNKS + 7) / 8]; /* one bit per received chunk */
    uint8     Chunk[FILE_UL_CHUNK_SIZE];
    char      RelayName[FILE_UL_RELAY_QUEUE][FILE_DL_MAX_NAME];
    uint8     RelayTag[FILE_UL_RELAY_QUEUE];
    uint32    RelayCount;
    FILE_UL_StatusTlm_t Status;
} FILE_UL_Data_t;

static FILE_UL_Data_t FILE_UL_Data;

static bool FILE_UL_IsSet(uint32 i)
{
    return (FILE_UL_Data.Bitmap[i / 8] & (1u << (i % 8))) != 0;
}

/* Bytes in chunk i; only the last chunk may be shorter than FILE_UL_CHUNK_SIZE. */
static uint32 FILE_UL_ChunkLength(uint32 i)
{
    uint32 offset = i * FILE_UL_CHUNK_SIZE;
    uint32 left   = FILE_UL_Data.FileSize - offset;

    return (left < FILE_UL_CHUNK_SIZE) ? left : FILE_UL_CHUNK_SIZE;
}

/* Sends UL_STATUS: state, CRCs and the first FILE_UL_MAX_GAPS missing byte ranges. */
static void FILE_UL_SendStatus(void)
{
    FILE_UL_StatusTlm_t *pTlm  = &FILE_UL_Data.Status;
    uint32               total = 0;
    uint32               listed = 0;
    uint32               i     = 0;
    uint32               first;
    uint32               end;

    memset(pTlm, 0, sizeof(*pTlm));
    CFE_MSG_Init(CFE_MSG_PTR(pTlm->TelemetryHeader), CFE_SB_ValueToMsgId(FILE_DL_UL_STATUS_TLM_MID), sizeof(*pTlm));
    pTlm->State   = FILE_UL_Data.State;
    pTlm->Relay   = FILE_UL_Data.Relay;
    pTlm->DestTag = FILE_UL_Data.DestTag;
    FILE_DL_PutU32(pTlm->FileSize, FILE_UL_Data.FileSize);
    FILE_DL_PutU32(pTlm->ExpectedCrc, FILE_UL_Data.ExpectedCrc);
    FILE_DL_PutU32(pTlm->FileCrc, FILE_UL_Data.FileCrc);
    memcpy(pTlm->Filename, FILE_UL_Data.Filename, sizeof(pTlm->Filename));

    while (i < FILE_UL_Data.ChunkCount)
    {
        if (FILE_UL_IsSet(i))
        {
            i++;
            continue;
        }
        first = i;
        while ((i < FILE_UL_Data.ChunkCount) && !FILE_UL_IsSet(i))
        {
            i++;
        }
        if (listed < FILE_UL_MAX_GAPS)
        {
            end = i * FILE_UL_CHUNK_SIZE;
            if (end > FILE_UL_Data.FileSize)
            {
                end = FILE_UL_Data.FileSize;
            }
            FILE_DL_PutU32(pTlm->Gaps[listed].Offset, first * FILE_UL_CHUNK_SIZE);
            FILE_DL_PutU32(pTlm->Gaps[listed].Length, end - first * FILE_UL_CHUNK_SIZE);
            listed++;
        }
        total++;
    }

    /* Received bytes: every chunk is full except maybe the last one. */
    end = FILE_UL_Data.ReceivedChunks * FILE_UL_CHUNK_SIZE;
    if ((FILE_UL_Data.ChunkCount > 0) && FILE_UL_IsSet(FILE_UL_Data.ChunkCount - 1))
    {
        end -= FILE_UL_CHUNK_SIZE - FILE_UL_ChunkLength(FILE_UL_Data.ChunkCount - 1);
    }
    FILE_DL_PutU32(pTlm->ReceivedBytes, end);
    pTlm->GapCount = (uint8)listed;
    FILE_DL_PutU32(pTlm->TotalGaps, total);

    CFE_SB_TimeStampMsg(CFE_MSG_PTR(pTlm->TelemetryHeader));
    CFE_SB_TransmitMsg(CFE_MSG_PTR(pTlm->TelemetryHeader), true);
}

static void FILE_UL_CloseFile(void)
{
    if (FILE_UL_Data.Open)
    {
        OS_close(FILE_UL_Data.Fd);
    }
    FILE_UL_Data.Open = false;
}

static void FILE_UL_Fail(void)
{
    FILE_UL_CloseFile();
    FILE_UL_Data.State = FILE_UL_STATE_FAILED;
    FILE_UL_SendStatus();
}

/* Only plain names under FILE_UL_DIR, so an upload cannot overwrite other files. */
static bool FILE_UL_IsValidName(const char *name)
{
    size_t dirLength = strlen(FILE_UL_DIR);

    return (strncmp(name, FILE_UL_DIR "/", dirLength + 1) == 0) && (name[dirLength + 1] != '\0') &&
           (strstr(name, "..") == NULL) && (strchr(name + dirLength + 1, '/') == NULL);
}

/* UL_START: checks the name and size, then creates (or empties) <name>.part. */
static void FILE_UL_Start(const FILE_UL_StartCmd_t *pCmd)
{
    char   name[FILE_DL_MAX_NAME];
    uint32 size = FILE_DL_GetU32(pCmd->FileSize);

    if ((FILE_UL_Data.State == FILE_UL_STATE_RECEIVING) || (FILE_UL_Data.State == FILE_UL_STATE_VERIFYING))
    {
        CFE_EVS_SendEvent(FILE_UL_CMD_ERR_EID, CFE_EVS_EventType_ERROR, "FILE_UL: %s is still being uploaded",
                          FILE_UL_Data.Filename);
        return;
    }

    CFE_PSP_MemCpy(name, pCmd->Filename, FILE_DL_MAX_NAME);
    name[FILE_DL_MAX_NAME - 1] = '\0';
    if (!FILE_UL_IsValidName(name) || (size == 0) || (size > FILE_UL_MAX_FILE_SIZE))
    {
        CFE_EVS_SendEvent(FILE_UL_CMD_ERR_EID, CFE_EVS_EventType_ERROR, "FILE_UL: START %s, %lu bytes rejected",
                          name, (unsigned long)size);
        return;
    }

    snprintf(FILE_UL_Data.PartName, sizeof(FILE_UL_Data.PartName), "%s.part", name);
    if (OS_OpenCreate(&FILE_UL_Data.Fd, FILE_UL_Data.PartName, OS_FILE_FLAG_CREATE | OS_FILE_FLAG_TRUNCATE,
                      OS_READ_WRITE) != OS_SUCCESS)
    {
        CFE_EVS_SendEvent(FILE_UL_FILE_ERR_EID, CFE_EVS_EventType_ERROR, "FILE_UL: cannot create %s",
                          FILE_UL_Data.PartName);
        return;
    }

    memcpy(FILE_UL_Data.Filename, name, sizeof(FILE_UL_Data.Filename));
    memset(FILE_UL_Data.Bitmap, 0, sizeof(FILE_UL_Data.Bitmap));
    FILE_UL_Data.Open           = true;
    FILE_UL_Data.FileSize       = size;
    FILE_UL_Data.ExpectedCrc    = FILE_DL_GetU32(pCmd->Crc32);
    FILE_UL_Data.FileCrc        = 0;
    FILE_UL_Data.ChunkCount     = (size + FILE_UL_CHUNK_SIZE - 1) / FILE_UL_CHUNK_SIZE;
    FILE_UL_Data.ReceivedChunks = 0;
    FILE_UL_Data.Relay          = pCmd->Relay;
    FILE_UL_Data.DestTag        = pCmd->DestTag;
    FILE_UL_Data.State          = FILE_UL_STATE_RECEIVING;

    CFE_EVS_SendEvent(FILE_UL_START_INF_EID, CFE_EVS_EventType_INFORMATION,
                      "FILE_UL: receiving %s, %lu bytes, relay %u tag %u", name, (unsigned long)size,
                      (unsigned int)pCmd->Relay, (unsigned int)pCmd->DestTag);
}

/* UL_DATA: writes one chunk at its offset; a chunk received twice is ignored. */
static void FILE_UL_WriteChunk(const FILE_UL_DataCmd_t *pCmd, size_t size)
{
    uint32 offset = FILE_DL_GetU32(pCmd->Offset);
    uint32 length = FILE_DL_GetU16(pCmd->DataLength);
    uint32 i      = offset / FILE_UL_CHUNK_SIZE;

    if ((FILE_UL_Data.State != FILE_UL_STATE_RECEIVING) || (size != FILE_UL_DATA_HDR_SIZE + length) ||
        ((offset % FILE_UL_CHUNK_SIZE) != 0) || (i >= FILE_UL_Data.ChunkCount) || (length != FILE_UL_ChunkLength(i)))
    {
        CFE_EVS_SendEvent(FILE_UL_CMD_ERR_EID, CFE_EVS_EventType_ERROR, "FILE_UL: DATA %lu+%lu rejected",
                          (unsigned long)offset, (unsigned long)length);
        return;
    }
    if (FILE_UL_IsSet(i))
    {
        return;
    }

    if ((OS_lseek(FILE_UL_Data.Fd, (int32)offset, OS_SEEK_SET) != (int32)offset) ||
        (OS_write(FILE_UL_Data.Fd, pCmd->Data, length) != (int32)length))
    {
        CFE_EVS_SendEvent(FILE_UL_FILE_ERR_EID, CFE_EVS_EventType_ERROR, "FILE_UL: write error at offset %lu",
                          (unsigned long)offset);
        FILE_UL_Fail();
        return;
    }
    FILE_UL_Data.Bitmap[i / 8] |= (uint8)(1u << (i % 8));
    FILE_UL_Data.ReceivedChunks++;
}

/* UL_END: reports the gaps; with none left the CRC check starts on the next ticks. */
static void FILE_UL_End(void)
{
    if ((FILE_UL_Data.State == FILE_UL_STATE_RECEIVING) &&
        (FILE_UL_Data.ReceivedChunks == FILE_UL_Data.ChunkCount))
    {
        FILE_UL_Data.State       = FILE_UL_STATE_VERIFYING;
        FILE_UL_Data.VerifyChunk = 0;
        FILE_UL_Data.FileCrc     = 0;
    }
    FILE_UL_SendStatus();
}

/* UL_CANCEL: drops the upload and its .part file. */
static void FILE_UL_Cancel(void)
{
    FILE_UL_CloseFile();
    if (FILE_UL_Data.State != FILE_UL_STATE_IDLE && FILE_UL_Data.State != FILE_UL_STATE_COMPLETE)
    {
        OS_remove(FILE_UL_Data.PartName);
    }
    FILE_UL_Data.State = FILE_UL_STATE_IDLE;
    CFE_EVS_SendEvent(FILE_UL_CANCEL_INF_EID, CFE_EVS_EventType_INFORMATION, "FILE_UL: %s cancelled",
                      FILE_UL_Data.Filename);
    FILE_UL_SendStatus();
}

/* All bytes read back: compare the CRC, rename <name>.part to <name>, queue a relay. */
static void FILE_UL_Finish(void)
{
    FILE_UL_CloseFile();
    if (FILE_UL_Data.FileCrc != FILE_UL_Data.ExpectedCrc)
    {
        CFE_EVS_SendEvent(FILE_UL_CRC_ERR_EID, CFE_EVS_EventType_ERROR,
                          "FILE_UL: %s crc32 mismatch, file 0x%08lx, ground 0x%08lx", FILE_UL_Data.Filename,
                          (unsigned long)FILE_UL_Data.FileCrc, (unsigned long)FILE_UL_Data.ExpectedCrc);
        FILE_UL_Fail();
        return;
    }

    OS_remove(FILE_UL_Data.Filename); /* an older file of the same name is replaced */
    if (OS_rename(FILE_UL_Data.PartName, FILE_UL_Data.Filename) != OS_SUCCESS)
    {
        CFE_EVS_SendEvent(FILE_UL_FILE_ERR_EID, CFE_EVS_EventType_ERROR, "FILE_UL: cannot rename %s",
                          FILE_UL_Data.PartName);
        FILE_UL_Fail();
        return;
    }

    FILE_UL_Data.State = FILE_UL_STATE_COMPLETE;
    CFE_EVS_SendEvent(FILE_UL_COMPLETE_INF_EID, CFE_EVS_EventType_INFORMATION,
                      "FILE_UL: %s received and verified, crc32 0x%08lx", FILE_UL_Data.Filename,
                      (unsigned long)FILE_UL_Data.FileCrc);

    if (FILE_UL_Data.Relay != 0)
    {
        if (FILE_UL_Data.RelayCount < FILE_UL_RELAY_QUEUE)
        {
            memcpy(FILE_UL_Data.RelayName[FILE_UL_Data.RelayCount], FILE_UL_Data.Filename, FILE_DL_MAX_NAME);
            FILE_UL_Data.RelayTag[FILE_UL_Data.RelayCount] = FILE_UL_Data.DestTag;
            FILE_UL_Data.RelayCount++;
        }
        else
        {
            CFE_EVS_SendEvent(FILE_UL_CMD_ERR_EID, CFE_EVS_EventType_ERROR, "FILE_UL: relay queue full, %s kept",
                              FILE_UL_Data.Filename);
        }
    }
    FILE_UL_SendStatus();
}

/* Relay: the next verified file goes down once the downlink has no open file. */
static void FILE_UL_RelayNext(void)
{
    uint32 i;

    if ((FILE_UL_Data.RelayCount == 0) || FILE_DL_IsBusy())
    {
        return;
    }
    if (FILE_DL_StartFile(FILE_UL_Data.RelayName[0], FILE_UL_Data.RelayTag[0]))
    {
        CFE_EVS_SendEvent(FILE_UL_RELAY_INF_EID, CFE_EVS_EventType_INFORMATION, "FILE_UL: relaying %s to tag %u",
                          FILE_UL_Data.RelayName[0], (unsigned int)FILE_UL_Data.RelayTag[0]);
    }
    for (i = 1; i < FILE_UL_Data.RelayCount; i++)
    {
        memcpy(FILE_UL_Data.RelayName[i - 1], FILE_UL_Data.RelayName[i], FILE_DL_MAX_NAME);
        FILE_UL_Data.RelayTag[i - 1] = FILE_UL_Data.RelayTag[i];
    }
    FILE_UL_Data.RelayCount--;
}

void FILE_UL_Init(void)
{
    memset(&FILE_UL_Data, 0, sizeof(FILE_UL_Data));
    FILE_UL_Data.State = FILE_UL_STATE_IDLE;
    OS_mkdir(FILE_UL_DIR, OS_READ_WRITE); /* fails harmlessly if it already exists */
}

/* Every 100 ms: read back FILE_UL_VERIFY_CHUNKS chunks for the CRC, then start a relay.
 * Reading in steps keeps the pipe served while a large file is checked. */
void FILE_UL_Tick(void)
{
    uint32 k;
    uint32 length;
    uint32 offset;

    for (k = 0; (FILE_UL_Data.State == FILE_UL_STATE_VERIFYING) && (k < FILE_UL_VERIFY_CHUNKS); k++)
    {
        if (FILE_UL_Data.VerifyChunk >= FILE_UL_Data.ChunkCount)
        {
            FILE_UL_Finish();
            break;
        }
        offset = FILE_UL_Data.VerifyChunk * FILE_UL_CHUNK_SIZE;
        length = FILE_UL_ChunkLength(FILE_UL_Data.VerifyChunk);
        if ((OS_lseek(FILE_UL_Data.Fd, (int32)offset, OS_SEEK_SET) != (int32)offset) ||
            (OS_read(FILE_UL_Data.Fd, FILE_UL_Data.Chunk, length) != (int32)length))
        {
            CFE_EVS_SendEvent(FILE_UL_FILE_ERR_EID, CFE_EVS_EventType_ERROR, "FILE_UL: read error at offset %lu",
                              (unsigned long)offset);
            FILE_UL_Fail();
            break;
        }
        FILE_UL_Data.FileCrc = FILE_DL_Crc32(FILE_UL_Data.FileCrc, FILE_UL_Data.Chunk, length);
        FILE_UL_Data.VerifyChunk++;
    }
    FILE_UL_RelayNext();
}

void FILE_UL_ProcessCommand(const CFE_SB_Buffer_t *pBuf, CFE_MSG_FcnCode_t cc, size_t size)
{
    if ((cc == FILE_UL_START_CC) && (size == sizeof(FILE_UL_StartCmd_t)))
    {
        FILE_UL_Start((const FILE_UL_StartCmd_t *)pBuf);
    }
    else if ((cc == FILE_UL_DATA_CC) && (size > FILE_UL_DATA_HDR_SIZE) && (size <= sizeof(FILE_UL_DataCmd_t)))
    {
        FILE_UL_WriteChunk((const FILE_UL_DataCmd_t *)pBuf, size);
    }
    else if ((cc == FILE_UL_END_CC) && (size == sizeof(CFE_MSG_CommandHeader_t)))
    {
        FILE_UL_End();
    }
    else if ((cc == FILE_UL_STATUS_CC) && (size == sizeof(CFE_MSG_CommandHeader_t)))
    {
        FILE_UL_SendStatus();
    }
    else if ((cc == FILE_UL_CANCEL_CC) && (size == sizeof(CFE_MSG_CommandHeader_t)))
    {
        FILE_UL_Cancel();
    }
    else
    {
        CFE_EVS_SendEvent(FILE_UL_CMD_ERR_EID, CFE_EVS_EventType_ERROR, "FILE_UL: bad command cc %u, size %lu",
                          (unsigned int)cc, (unsigned long)size);
    }
}
