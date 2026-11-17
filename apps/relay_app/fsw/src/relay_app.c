/*
** RELAY_APP: FORWARD asks FILE_DL to send an on-board file, then copies each
** FILE_DL segment of that file to the selected ground station over UDP.
*/

#include <string.h>

#include "relay_app_events.h"
#include "relay_app.h"

/* Pre-registered ground stations: FORWARD picks one by index, never by a free IP */
static const struct
{
    const char *Name;
    const char *Address;
    uint16      Port;
} RELAY_APP_Stations[] = {
    {"TV_STATION", "127.0.0.1", 1240},
    {"BACKUP_STATION", "127.0.0.1", 1241},
};

#define RELAY_APP_STATION_COUNT (sizeof(RELAY_APP_Stations) / sizeof(RELAY_APP_Stations[0]))

RELAY_APP_Data_t RELAY_APP_Data;

/* RELAY_APP_Main() -- Application entry point and main process loop */
void RELAY_APP_Main(void)
{
    int32            status;
    CFE_SB_Buffer_t *SBBufPtr;

    CFE_ES_PerfLogEntry(RELAY_APP_PERF_ID);

    status = RELAY_APP_Init();
    if (status != CFE_SUCCESS)
    {
        RELAY_APP_Data.RunStatus = CFE_ES_RunStatus_APP_ERROR;
    }

    while (CFE_ES_RunLoop(&RELAY_APP_Data.RunStatus) == true)
    {
        CFE_ES_PerfLogExit(RELAY_APP_PERF_ID);

        status = CFE_SB_ReceiveBuffer(&SBBufPtr, RELAY_APP_Data.CommandPipe, CFE_SB_PEND_FOREVER);

        CFE_ES_PerfLogEntry(RELAY_APP_PERF_ID);

        if (status == CFE_SUCCESS)
        {
            RELAY_APP_ProcessCommandPacket(SBBufPtr);
        }
        else
        {
            CFE_EVS_SendEvent(RELAY_APP_PIPE_ERR_EID, CFE_EVS_EventType_ERROR,
                              "RELAY: SB Pipe Read Error, App Will Exit");
            RELAY_APP_Data.RunStatus = CFE_ES_RunStatus_APP_ERROR;
        }
    }

    CFE_ES_PerfLogExit(RELAY_APP_PERF_ID);
    CFE_ES_ExitApp(RELAY_APP_Data.RunStatus);
}

/* RELAY_APP_Init() -- initialization */
int32 RELAY_APP_Init(void)
{
    int32 status;

    memset(&RELAY_APP_Data, 0, sizeof(RELAY_APP_Data));
    RELAY_APP_Data.RunStatus = CFE_ES_RunStatus_APP_RUN;
    RELAY_APP_Data.State     = RELAY_APP_STATE_IDLE;

    status = CFE_EVS_Register(NULL, 0, CFE_EVS_EventFilter_BINARY);
    if (status != CFE_SUCCESS)
    {
        CFE_ES_WriteToSysLog("Relay App: Error Registering Events, RC = 0x%08lX\n", (unsigned long)status);
        return status;
    }

    CFE_MSG_Init(CFE_MSG_PTR(RELAY_APP_Data.HkTlm.TelemetryHeader), CFE_SB_ValueToMsgId(RELAY_APP_HK_TLM_MID),
                 sizeof(RELAY_APP_Data.HkTlm));

    /* The FILE_DL START command is built once; FORWARD only fills in the file name */
    CFE_MSG_Init(CFE_MSG_PTR(RELAY_APP_Data.FileDlStartCmd.CommandHeader), CFE_SB_ValueToMsgId(FILE_DL_CMD_MID),
                 sizeof(RELAY_APP_Data.FileDlStartCmd));
    CFE_MSG_SetFcnCode(CFE_MSG_PTR(RELAY_APP_Data.FileDlStartCmd.CommandHeader), FILE_DL_START_CC);

    status = CFE_SB_CreatePipe(&RELAY_APP_Data.CommandPipe, RELAY_APP_PIPE_DEPTH, "RELAY_APP_CMD_PIPE");
    if (status != CFE_SUCCESS)
    {
        CFE_ES_WriteToSysLog("Relay App: Error creating pipe, RC = 0x%08lX\n", (unsigned long)status);
        return status;
    }

    status = CFE_SB_Subscribe(CFE_SB_ValueToMsgId(RELAY_APP_SEND_HK_MID), RELAY_APP_Data.CommandPipe);
    if (status == CFE_SUCCESS)
    {
        status = CFE_SB_Subscribe(CFE_SB_ValueToMsgId(RELAY_APP_CMD_MID), RELAY_APP_Data.CommandPipe);
    }
    if (status == CFE_SUCCESS)
    {
        /* FILE_DL segments may queue up while a segment is being sent, so allow the whole pipe */
        status = CFE_SB_SubscribeEx(CFE_SB_ValueToMsgId(FILE_DL_SEG_TLM_MID), RELAY_APP_Data.CommandPipe,
                                    CFE_SB_DEFAULT_QOS, RELAY_APP_PIPE_DEPTH);
    }
    if (status != CFE_SUCCESS)
    {
        CFE_ES_WriteToSysLog("Relay App: Error subscribing, RC = 0x%08lX\n", (unsigned long)status);
        return status;
    }

    status = OS_SocketOpen(&RELAY_APP_Data.Socket, OS_SocketDomain_INET, OS_SocketType_DATAGRAM);
    if (status != OS_SUCCESS)
    {
        CFE_ES_WriteToSysLog("Relay App: Error opening socket, RC = 0x%08lX\n", (unsigned long)status);
        return status;
    }

    CFE_EVS_SendEvent(RELAY_APP_STARTUP_INF_EID, CFE_EVS_EventType_INFORMATION, "RELAY App Initialized, %u stations",
                      (unsigned int)RELAY_APP_STATION_COUNT);

    return CFE_SUCCESS;
}

/* RELAY_APP_ProcessCommandPacket() -- route a packet from the pipe */
void RELAY_APP_ProcessCommandPacket(CFE_SB_Buffer_t *SBBufPtr)
{
    CFE_SB_MsgId_t MsgId = CFE_SB_INVALID_MSG_ID;

    CFE_MSG_GetMsgId(&SBBufPtr->Msg, &MsgId);

    switch (CFE_SB_MsgIdToValue(MsgId))
    {
        case RELAY_APP_CMD_MID:
            RELAY_APP_ProcessGroundCommand(SBBufPtr);
            break;

        case RELAY_APP_SEND_HK_MID:
            RELAY_APP_ReportHousekeeping((CFE_MSG_CommandHeader_t *)SBBufPtr);
            break;

        case FILE_DL_SEG_TLM_MID:
            RELAY_APP_ForwardSegment(SBBufPtr);
            break;

        default:
            CFE_EVS_SendEvent(RELAY_APP_INVALID_MSGID_ERR_EID, CFE_EVS_EventType_ERROR,
                              "RELAY: invalid command packet,MID = 0x%x", (unsigned int)CFE_SB_MsgIdToValue(MsgId));
            break;
    }
}

/* RELAY_APP_ProcessGroundCommand() -- RELAY ground commands */
void RELAY_APP_ProcessGroundCommand(CFE_SB_Buffer_t *SBBufPtr)
{
    CFE_MSG_FcnCode_t CommandCode = 0;

    CFE_MSG_GetFcnCode(&SBBufPtr->Msg, &CommandCode);

    switch (CommandCode)
    {
        case RELAY_APP_NOOP_CC:
            if (RELAY_APP_VerifyCmdLength(&SBBufPtr->Msg, sizeof(RELAY_APP_NoopCmd_t)))
            {
                RELAY_APP_Data.CmdCounter++;
                CFE_EVS_SendEvent(RELAY_APP_COMMANDNOP_INF_EID, CFE_EVS_EventType_INFORMATION, "RELAY: NOOP command");
            }
            break;

        case RELAY_APP_RESET_COUNTERS_CC:
            if (RELAY_APP_VerifyCmdLength(&SBBufPtr->Msg, sizeof(RELAY_APP_ResetCountersCmd_t)))
            {
                RELAY_APP_Data.CmdCounter = 0;
                RELAY_APP_Data.ErrCounter = 0;
                CFE_EVS_SendEvent(RELAY_APP_COMMANDRST_INF_EID, CFE_EVS_EventType_INFORMATION, "RELAY: RESET command");
            }
            break;

        case RELAY_APP_FORWARD_CC:
            if (RELAY_APP_VerifyCmdLength(&SBBufPtr->Msg, sizeof(RELAY_APP_ForwardCmd_t)))
            {
                RELAY_APP_Forward((RELAY_APP_ForwardCmd_t *)SBBufPtr);
            }
            break;

        case RELAY_APP_STOP_CC:
            if (RELAY_APP_VerifyCmdLength(&SBBufPtr->Msg, sizeof(RELAY_APP_StopCmd_t)))
            {
                RELAY_APP_Stop((RELAY_APP_StopCmd_t *)SBBufPtr);
            }
            break;

        default:
            RELAY_APP_Data.ErrCounter++;
            CFE_EVS_SendEvent(RELAY_APP_COMMAND_ERR_EID, CFE_EVS_EventType_ERROR,
                              "RELAY: Invalid ground command code: CC = %d", CommandCode);
            break;
    }
}

/* RELAY_APP_ReportHousekeeping() -- send HK telemetry on request from SCH */
int32 RELAY_APP_ReportHousekeeping(const CFE_MSG_CommandHeader_t *Msg)
{
    RELAY_APP_HkTlm_Payload_t *Payload = &RELAY_APP_Data.HkTlm.Payload;

    Payload->CommandCounter      = RELAY_APP_Data.CmdCounter;
    Payload->CommandErrorCounter = RELAY_APP_Data.ErrCounter;
    Payload->State               = RELAY_APP_Data.State;
    Payload->StationId           = RELAY_APP_Data.StationId;
    Payload->SegmentsForwarded   = RELAY_APP_Data.SegmentsForwarded;
    Payload->BytesForwarded      = RELAY_APP_Data.BytesForwarded;

    CFE_SB_TimeStampMsg(CFE_MSG_PTR(RELAY_APP_Data.HkTlm.TelemetryHeader));
    CFE_SB_TransmitMsg(CFE_MSG_PTR(RELAY_APP_Data.HkTlm.TelemetryHeader), true);

    return CFE_SUCCESS;
}

/*
** RELAY_APP_Forward() -- check the file and the station, then send FILE_DL a START
** command for the file; relaying begins when FILE_DL's START segment comes back.
*/
int32 RELAY_APP_Forward(const RELAY_APP_ForwardCmd_t *Msg)
{
    os_fstat_t FileStat;
    uint8      StationId = Msg->Payload.StationId;

    if (RELAY_APP_Data.State != RELAY_APP_STATE_IDLE)
    {
        RELAY_APP_Data.ErrCounter++;
        CFE_EVS_SendEvent(RELAY_APP_FORWARD_ERR_EID, CFE_EVS_EventType_ERROR,
                          "RELAY: %s is still being relayed, send STOP first", RELAY_APP_Data.Filename);
        return CFE_STATUS_REQUEST_ALREADY_PENDING;
    }

    if (StationId >= RELAY_APP_STATION_COUNT)
    {
        RELAY_APP_Data.ErrCounter++;
        CFE_EVS_SendEvent(RELAY_APP_FORWARD_ERR_EID, CFE_EVS_EventType_ERROR, "RELAY: Unknown station %u",
                          (unsigned int)StationId);
        return CFE_STATUS_EXTERNAL_RESOURCE_FAIL;
    }

    CFE_SB_MessageStringGet(RELAY_APP_Data.Filename, Msg->Payload.Filename, NULL, sizeof(RELAY_APP_Data.Filename),
                            sizeof(Msg->Payload.Filename));
    if (OS_stat(RELAY_APP_Data.Filename, &FileStat) != OS_SUCCESS)
    {
        RELAY_APP_Data.ErrCounter++;
        CFE_EVS_SendEvent(RELAY_APP_FORWARD_ERR_EID, CFE_EVS_EventType_ERROR, "RELAY: Cannot find %s",
                          RELAY_APP_Data.Filename);
        return CFE_STATUS_EXTERNAL_RESOURCE_FAIL;
    }

    OS_SocketAddrInit(&RELAY_APP_Data.StationAddr, OS_SocketDomain_INET);
    OS_SocketAddrFromString(&RELAY_APP_Data.StationAddr, RELAY_APP_Stations[StationId].Address);
    OS_SocketAddrSetPort(&RELAY_APP_Data.StationAddr, RELAY_APP_Stations[StationId].Port);

    RELAY_APP_Data.StationId         = StationId;
    RELAY_APP_Data.SegmentsForwarded = 0;
    RELAY_APP_Data.BytesForwarded    = 0;
    RELAY_APP_Data.State             = RELAY_APP_STATE_WAITING;

    /* Ask FILE_DL to send the file; it rejects the command if another file is still going */
    memset(RELAY_APP_Data.FileDlStartCmd.Filename, 0, sizeof(RELAY_APP_Data.FileDlStartCmd.Filename));
    strncpy(RELAY_APP_Data.FileDlStartCmd.Filename, RELAY_APP_Data.Filename,
            sizeof(RELAY_APP_Data.FileDlStartCmd.Filename) - 1);
    CFE_SB_TransmitMsg(CFE_MSG_PTR(RELAY_APP_Data.FileDlStartCmd.CommandHeader), true);

    RELAY_APP_Data.CmdCounter++;
    CFE_EVS_SendEvent(RELAY_APP_FORWARD_INF_EID, CFE_EVS_EventType_INFORMATION,
                      "RELAY: Forwarding %s (%lu bytes) to %s %s:%u", RELAY_APP_Data.Filename,
                      (unsigned long)OS_FILESTAT_SIZE(FileStat), RELAY_APP_Stations[StationId].Name,
                      RELAY_APP_Stations[StationId].Address, (unsigned int)RELAY_APP_Stations[StationId].Port);

    return CFE_SUCCESS;
}

/* RELAY_APP_Stop() -- end the relay; FILE_DL itself is not stopped */
int32 RELAY_APP_Stop(const RELAY_APP_StopCmd_t *Msg)
{
    RELAY_APP_Data.State = RELAY_APP_STATE_IDLE;

    RELAY_APP_Data.CmdCounter++;
    CFE_EVS_SendEvent(RELAY_APP_STOP_INF_EID, CFE_EVS_EventType_INFORMATION,
                      "RELAY: Stopped, %lu segments (%lu bytes) forwarded", (unsigned long)RELAY_APP_Data.SegmentsForwarded,
                      (unsigned long)RELAY_APP_Data.BytesForwarded);

    return CFE_SUCCESS;
}

/*
** RELAY_APP_ForwardSegment() -- a START segment naming our file starts the relay;
** while relaying, every segment (DATA, END and later RESEND data) goes to the station
** unchanged, so the station rebuilds the file the same way the main ground does.
*/
void RELAY_APP_ForwardSegment(const CFE_SB_Buffer_t *SBBufPtr)
{
    const FILE_DL_SegHdr_t *SegHdr = (const FILE_DL_SegHdr_t *)((const uint8 *)SBBufPtr + sizeof(CFE_MSG_TelemetryHeader_t));
    const uint8            *Data   = (const uint8 *)SegHdr + sizeof(FILE_DL_SegHdr_t);
    size_t                  Size   = 0;
    uint16                  DataLength;
    int32                   status;

    if (RELAY_APP_Data.State == RELAY_APP_STATE_IDLE)
    {
        return;
    }

    CFE_MSG_GetSize(&SBBufPtr->Msg, &Size);
    if (Size < sizeof(CFE_MSG_TelemetryHeader_t) + sizeof(FILE_DL_SegHdr_t))
    {
        return;
    }
    DataLength = (uint16)((SegHdr->DataLength[0] << 8) | SegHdr->DataLength[1]); /* big-endian */

    if (SegHdr->Type == FILE_DL_TYPE_START)
    {
        if (DataLength == strlen(RELAY_APP_Data.Filename) && memcmp(Data, RELAY_APP_Data.Filename, DataLength) == 0)
        {
            RELAY_APP_Data.State = RELAY_APP_STATE_FORWARDING;
            CFE_EVS_SendEvent(RELAY_APP_RELAY_INF_EID, CFE_EVS_EventType_INFORMATION, "RELAY: FILE_DL started %s",
                              RELAY_APP_Data.Filename);
        }
        else if (RELAY_APP_Data.State == RELAY_APP_STATE_FORWARDING)
        {
            /* FILE_DL moved on to another file, which must not go to this station */
            RELAY_APP_Data.State = RELAY_APP_STATE_IDLE;
            CFE_EVS_SendEvent(RELAY_APP_RELAY_INF_EID, CFE_EVS_EventType_INFORMATION,
                              "RELAY: FILE_DL started another file, relay of %s ended", RELAY_APP_Data.Filename);
            return;
        }
    }

    if (RELAY_APP_Data.State != RELAY_APP_STATE_FORWARDING)
    {
        return;
    }

    status = OS_SocketSendTo(RELAY_APP_Data.Socket, SBBufPtr, Size, &RELAY_APP_Data.StationAddr);
    if (status < 0)
    {
        RELAY_APP_Data.ErrCounter++;
        CFE_EVS_SendEvent(RELAY_APP_SOCKET_ERR_EID, CFE_EVS_EventType_ERROR, "RELAY: sendto error %d",
                          (int)status);
        return;
    }
    RELAY_APP_Data.SegmentsForwarded++;
    RELAY_APP_Data.BytesForwarded += Size;

    if (SegHdr->Type == FILE_DL_TYPE_END)
    {
        CFE_EVS_SendEvent(RELAY_APP_RELAY_INF_EID, CFE_EVS_EventType_INFORMATION,
                          "RELAY: %s forwarded to %s, %lu segments", RELAY_APP_Data.Filename,
                          RELAY_APP_Stations[RELAY_APP_Data.StationId].Name,
                          (unsigned long)RELAY_APP_Data.SegmentsForwarded);
    }
}

/* RELAY_APP_VerifyCmdLength() -- Verify command packet length */
bool RELAY_APP_VerifyCmdLength(CFE_MSG_Message_t *MsgPtr, size_t ExpectedLength)
{
    bool              result       = true;
    size_t            ActualLength = 0;
    CFE_SB_MsgId_t    MsgId        = CFE_SB_INVALID_MSG_ID;
    CFE_MSG_FcnCode_t FcnCode      = 0;

    CFE_MSG_GetSize(MsgPtr, &ActualLength);

    if (ExpectedLength != ActualLength)
    {
        CFE_MSG_GetMsgId(MsgPtr, &MsgId);
        CFE_MSG_GetFcnCode(MsgPtr, &FcnCode);

        CFE_EVS_SendEvent(RELAY_APP_LEN_ERR_EID, CFE_EVS_EventType_ERROR,
                          "RELAY: Invalid Msg length: ID = 0x%X,  CC = %u, Len = %u, Expected = %u",
                          (unsigned int)CFE_SB_MsgIdToValue(MsgId), (unsigned int)FcnCode, (unsigned int)ActualLength,
                          (unsigned int)ExpectedLength);

        result = false;
        RELAY_APP_Data.ErrCounter++;
    }

    return result;
}
