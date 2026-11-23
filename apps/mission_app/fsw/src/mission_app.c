/*
** MSN_APP: satellite control app. Changes parameters, chooses the apps cFS starts,
** runs diagnostics, reports app status and sends the temperature read from a file.
*/

#include <stdio.h>
#include <string.h>

#include "msn_app_events.h"
#include "msn_app_version.h"
#include "msn_app.h"

/* Apps needed to command the satellite: they cannot be disabled and are checked by RUN_DIAG */
static const char *const MSN_APP_EssentialApps[] = {"CI_LAB_APP", "TO_LAB_APP", "SCH_LAB_APP", "MSN_APP"};

#define MSN_APP_ESSENTIAL_COUNT (int32)(sizeof(MSN_APP_EssentialApps) / sizeof(MSN_APP_EssentialApps[0]))

static const char *const MSN_APP_TempStatusName[] = {"NORMAL", "HIGH", "LOW"};

MSN_APP_Data_t MSN_APP_Data;

/* MSN_APP_Main() -- Application entry point and main process loop */
void MSN_APP_Main(void)
{
    int32            status;
    CFE_SB_Buffer_t *SBBufPtr;

    CFE_ES_PerfLogEntry(MSN_APP_PERF_ID);

    status = MSN_APP_Init();
    if (status != CFE_SUCCESS)
    {
        MSN_APP_Data.RunStatus = CFE_ES_RunStatus_APP_ERROR;
    }

    while (CFE_ES_RunLoop(&MSN_APP_Data.RunStatus) == true)
    {
        CFE_ES_PerfLogExit(MSN_APP_PERF_ID);

        status = CFE_SB_ReceiveBuffer(&SBBufPtr, MSN_APP_Data.CommandPipe, CFE_SB_PEND_FOREVER);

        CFE_ES_PerfLogEntry(MSN_APP_PERF_ID);

        if (status == CFE_SUCCESS)
        {
            MSN_APP_ProcessCommandPacket(SBBufPtr);
        }
        else
        {
            CFE_EVS_SendEvent(MSN_APP_PIPE_ERR_EID, CFE_EVS_EventType_ERROR, "MSN: SB Pipe Read Error, App Will Exit");
            MSN_APP_Data.RunStatus = CFE_ES_RunStatus_APP_ERROR;
        }
    }

    CFE_ES_PerfLogExit(MSN_APP_PERF_ID);
    CFE_ES_ExitApp(MSN_APP_Data.RunStatus);
}

/* MSN_APP_Init() -- initialization */
int32 MSN_APP_Init(void)
{
    int32 status;

    memset(&MSN_APP_Data, 0, sizeof(MSN_APP_Data));
    MSN_APP_Data.RunStatus  = CFE_ES_RunStatus_APP_RUN;
    MSN_APP_Data.TempStatus = MSN_APP_TEMP_NO_DATA;

    status = CFE_EVS_Register(NULL, 0, CFE_EVS_EventFilter_BINARY);
    if (status != CFE_SUCCESS)
    {
        CFE_ES_WriteToSysLog("MSN App: Error Registering Events, RC = 0x%08lX\n", (unsigned long)status);
        return status;
    }

    CFE_MSG_Init(CFE_MSG_PTR(MSN_APP_Data.HkTlm.TelemetryHeader), CFE_SB_ValueToMsgId(MSN_APP_HK_TLM_MID),
                 sizeof(MSN_APP_Data.HkTlm));
    CFE_MSG_Init(CFE_MSG_PTR(MSN_APP_Data.TempTlm.TelemetryHeader), CFE_SB_ValueToMsgId(MSN_APP_TEMP_TLM_MID),
                 sizeof(MSN_APP_Data.TempTlm));
    CFE_MSG_Init(CFE_MSG_PTR(MSN_APP_Data.DiagTlm.TelemetryHeader), CFE_SB_ValueToMsgId(MSN_APP_DIAG_TLM_MID),
                 sizeof(MSN_APP_Data.DiagTlm));
    CFE_MSG_Init(CFE_MSG_PTR(MSN_APP_Data.AppStatusTlm.TelemetryHeader),
                 CFE_SB_ValueToMsgId(MSN_APP_APP_STATUS_TLM_MID), sizeof(MSN_APP_Data.AppStatusTlm));

    status = CFE_SB_CreatePipe(&MSN_APP_Data.CommandPipe, MSN_APP_PIPE_DEPTH, "MSN_APP_CMD_PIPE");
    if (status != CFE_SUCCESS)
    {
        CFE_ES_WriteToSysLog("MSN App: Error creating pipe, RC = 0x%08lX\n", (unsigned long)status);
        return status;
    }

    status = CFE_SB_Subscribe(CFE_SB_ValueToMsgId(MSN_APP_SEND_HK_MID), MSN_APP_Data.CommandPipe);
    if (status != CFE_SUCCESS)
    {
        CFE_ES_WriteToSysLog("MSN App: Error Subscribing to HK request, RC = 0x%08lX\n", (unsigned long)status);
        return status;
    }

    status = CFE_SB_Subscribe(CFE_SB_ValueToMsgId(MSN_APP_CMD_MID), MSN_APP_Data.CommandPipe);
    if (status != CFE_SUCCESS)
    {
        CFE_ES_WriteToSysLog("MSN App: Error Subscribing to Command, RC = 0x%08lX\n", (unsigned long)status);
        return status;
    }

    /* Defaults when the parameter file is missing or bad: limits 60 / -20 C, TEMP tlm every HK */
    if (MSN_APP_ReadParamFile(MSN_APP_Data.Param) == false)
    {
        MSN_APP_Data.Param[MSN_APP_PARAM_TEMP_HIGH]   = 60;
        MSN_APP_Data.Param[MSN_APP_PARAM_TEMP_LOW]    = -20;
        MSN_APP_Data.Param[MSN_APP_PARAM_TEMP_PERIOD] = 1;
        MSN_APP_SaveParams();
    }

    CFE_EVS_SendEvent(MSN_APP_STARTUP_INF_EID, CFE_EVS_EventType_INFORMATION,
                      "MSN App Initialized.%s Temp limits %ld / %ld C, period %ld", MSN_APP_VERSION_STRING,
                      (long)MSN_APP_Data.Param[MSN_APP_PARAM_TEMP_HIGH], (long)MSN_APP_Data.Param[MSN_APP_PARAM_TEMP_LOW],
                      (long)MSN_APP_Data.Param[MSN_APP_PARAM_TEMP_PERIOD]);

    return CFE_SUCCESS;
}

/* MSN_APP_ProcessCommandPacket() -- route a packet from the command pipe */
void MSN_APP_ProcessCommandPacket(CFE_SB_Buffer_t *SBBufPtr)
{
    CFE_SB_MsgId_t MsgId = CFE_SB_INVALID_MSG_ID;

    CFE_MSG_GetMsgId(&SBBufPtr->Msg, &MsgId);

    switch (CFE_SB_MsgIdToValue(MsgId))
    {
        case MSN_APP_CMD_MID:
            MSN_APP_ProcessGroundCommand(SBBufPtr);
            break;

        case MSN_APP_SEND_HK_MID:
            MSN_APP_ReportHousekeeping((CFE_MSG_CommandHeader_t *)SBBufPtr);
            break;

        default:
            CFE_EVS_SendEvent(MSN_APP_INVALID_MSGID_ERR_EID, CFE_EVS_EventType_ERROR,
                              "MSN: invalid command packet,MID = 0x%x", (unsigned int)CFE_SB_MsgIdToValue(MsgId));
            break;
    }
}

/* MSN_APP_ProcessGroundCommand() -- MSN ground commands */
void MSN_APP_ProcessGroundCommand(CFE_SB_Buffer_t *SBBufPtr)
{
    CFE_MSG_FcnCode_t CommandCode = 0;

    CFE_MSG_GetFcnCode(&SBBufPtr->Msg, &CommandCode);

    switch (CommandCode)
    {
        case MSN_APP_NOOP_CC:
            if (MSN_APP_VerifyCmdLength(&SBBufPtr->Msg, sizeof(MSN_APP_NoopCmd_t)))
            {
                MSN_APP_Data.CmdCounter++;
                CFE_EVS_SendEvent(MSN_APP_COMMANDNOP_INF_EID, CFE_EVS_EventType_INFORMATION, "MSN: NOOP command%s",
                                  MSN_APP_VERSION_STRING);
            }
            break;

        case MSN_APP_RESET_COUNTERS_CC:
            if (MSN_APP_VerifyCmdLength(&SBBufPtr->Msg, sizeof(MSN_APP_ResetCountersCmd_t)))
            {
                MSN_APP_Data.CmdCounter = 0;
                MSN_APP_Data.ErrCounter = 0;
                CFE_EVS_SendEvent(MSN_APP_COMMANDRST_INF_EID, CFE_EVS_EventType_INFORMATION, "MSN: RESET command");
            }
            break;

        case MSN_APP_SET_PARAM_CC:
            if (MSN_APP_VerifyCmdLength(&SBBufPtr->Msg, sizeof(MSN_APP_SetParamCmd_t)))
            {
                MSN_APP_SetParam((MSN_APP_SetParamCmd_t *)SBBufPtr);
            }
            break;

        case MSN_APP_SET_STARTUP_CC:
            if (MSN_APP_VerifyCmdLength(&SBBufPtr->Msg, sizeof(MSN_APP_SetStartupCmd_t)))
            {
                MSN_APP_SetStartup((MSN_APP_SetStartupCmd_t *)SBBufPtr);
            }
            break;

        case MSN_APP_RUN_DIAG_CC:
            if (MSN_APP_VerifyCmdLength(&SBBufPtr->Msg, sizeof(MSN_APP_RunDiagCmd_t)))
            {
                MSN_APP_RunDiag((MSN_APP_RunDiagCmd_t *)SBBufPtr);
            }
            break;

        case MSN_APP_GET_APP_STATUS_CC:
            if (MSN_APP_VerifyCmdLength(&SBBufPtr->Msg, sizeof(MSN_APP_GetAppStatusCmd_t)))
            {
                MSN_APP_GetAppStatus((MSN_APP_GetAppStatusCmd_t *)SBBufPtr);
            }
            break;

        case MSN_APP_READ_TEMP_CC:
            if (MSN_APP_VerifyCmdLength(&SBBufPtr->Msg, sizeof(MSN_APP_ReadTempCmd_t)))
            {
                if (MSN_APP_SendTemperature() == CFE_SUCCESS)
                {
                    MSN_APP_Data.CmdCounter++;
                }
                else
                {
                    MSN_APP_Data.ErrCounter++;
                }
            }
            break;

        default:
            MSN_APP_Data.ErrCounter++;
            CFE_EVS_SendEvent(MSN_APP_COMMAND_ERR_EID, CFE_EVS_EventType_ERROR, "MSN: Invalid ground command code: CC = %d",
                              CommandCode);
            break;
    }
}

/*
** MSN_APP_ReportHousekeeping() -- send HK telemetry on request from SCH, and
** TEMP telemetry on every TEMP_PERIOD-th request
*/
int32 MSN_APP_ReportHousekeeping(const CFE_MSG_CommandHeader_t *Msg)
{
    MSN_APP_HkTlm_Payload_t *Payload = &MSN_APP_Data.HkTlm.Payload;

    MSN_APP_Data.HkCount++;
    if (MSN_APP_Data.Param[MSN_APP_PARAM_TEMP_PERIOD] > 0 &&
        MSN_APP_Data.HkCount % MSN_APP_Data.Param[MSN_APP_PARAM_TEMP_PERIOD] == 0)
    {
        MSN_APP_SendTemperature();
    }

    Payload->CommandCounter      = MSN_APP_Data.CmdCounter;
    Payload->CommandErrorCounter = MSN_APP_Data.ErrCounter;
    Payload->TempStatus          = MSN_APP_Data.TempStatus;
    Payload->Temperature         = MSN_APP_Data.Temperature;
    memcpy(Payload->Param, MSN_APP_Data.Param, sizeof(Payload->Param));

    CFE_SB_TimeStampMsg(CFE_MSG_PTR(MSN_APP_Data.HkTlm.TelemetryHeader));
    CFE_SB_TransmitMsg(CFE_MSG_PTR(MSN_APP_Data.HkTlm.TelemetryHeader), true);

    return CFE_SUCCESS;
}

/* MSN_APP_SetParam() -- change one parameter after a range check and save it to the file */
int32 MSN_APP_SetParam(const MSN_APP_SetParamCmd_t *Msg)
{
    uint8 Id    = Msg->Payload.ParamId;
    int32 Value = Msg->Payload.Value;
    int32 Old;
    bool  Valid;

    switch (Id)
    {
        case MSN_APP_PARAM_TEMP_HIGH:
            Valid = (Value > MSN_APP_Data.Param[MSN_APP_PARAM_TEMP_LOW] && Value <= 200);
            break;
        case MSN_APP_PARAM_TEMP_LOW:
            Valid = (Value < MSN_APP_Data.Param[MSN_APP_PARAM_TEMP_HIGH] && Value >= -100);
            break;
        case MSN_APP_PARAM_TEMP_PERIOD:
            Valid = (Value >= 0 && Value <= 100);
            break;
        default:
            Valid = false;
            break;
    }

    if (Valid == false)
    {
        MSN_APP_Data.ErrCounter++;
        CFE_EVS_SendEvent(MSN_APP_PARAM_ERR_EID, CFE_EVS_EventType_ERROR, "MSN: Invalid param %u or value %ld",
                          (unsigned int)Id, (long)Value);
        return MSN_APP_CMD_REJECTED_ERR_CODE;
    }

    Old                    = MSN_APP_Data.Param[Id];
    MSN_APP_Data.Param[Id] = Value;
    MSN_APP_SaveParams();

    MSN_APP_Data.CmdCounter++;
    CFE_EVS_SendEvent(MSN_APP_PARAM_INF_EID, CFE_EVS_EventType_INFORMATION, "MSN: Param %u changed %ld -> %ld",
                      (unsigned int)Id, (long)Old, (long)Value);

    return CFE_SUCCESS;
}

/*
** MSN_APP_SetStartup() -- enable or disable an app for the next cFS start. ES stops reading
** the startup script at the first '!', so the app's line is moved above or below that line.
*/
int32 MSN_APP_SetStartup(const MSN_APP_SetStartupCmd_t *Msg)
{
    char        AppName[CFE_MISSION_MAX_API_LEN];
    char       *Entry;
    char       *Marker;
    char       *EntryEnd;
    char       *MarkerEnd;
    char       *Dest;
    char       *End;
    const char *From[4];
    const char *To[4];
    osal_id_t   FileId;
    int32       State;
    int32       i;
    bool        Written = true;

    CFE_SB_MessageStringGet(AppName, Msg->Payload.AppName, NULL, sizeof(AppName), sizeof(Msg->Payload.AppName));

    if (Msg->Payload.Enable > 1)
    {
        MSN_APP_Data.ErrCounter++;
        CFE_EVS_SendEvent(MSN_APP_STARTUP_SCR_ERR_EID, CFE_EVS_EventType_ERROR, "MSN: Invalid Enable %u",
                          (unsigned int)Msg->Payload.Enable);
        return MSN_APP_CMD_REJECTED_ERR_CODE;
    }

    for (i = 0; i < MSN_APP_ESSENTIAL_COUNT && Msg->Payload.Enable == 0; i++)
    {
        if (strcmp(AppName, MSN_APP_EssentialApps[i]) == 0)
        {
            MSN_APP_Data.ErrCounter++;
            CFE_EVS_SendEvent(MSN_APP_STARTUP_SCR_ERR_EID, CFE_EVS_EventType_ERROR,
                              "MSN: %s is needed for commanding and cannot be disabled", AppName);
            return MSN_APP_CMD_REJECTED_ERR_CODE;
        }
    }

    State = MSN_APP_FindStartupEntry(AppName, &Entry, &Marker);
    if (State == MSN_APP_STARTUP_NOT_FOUND)
    {
        CFE_EVS_SendEvent(MSN_APP_STARTUP_SCR_ERR_EID, CFE_EVS_EventType_ERROR, "MSN: %s is not in %s", AppName,
                          MSN_APP_STARTUP_FILE);
    }
    if (State <= MSN_APP_STARTUP_NOT_FOUND)
    {
        MSN_APP_Data.ErrCounter++;
        return CFE_STATUS_EXTERNAL_RESOURCE_FAIL;
    }
    if (State == (Msg->Payload.Enable ? MSN_APP_STARTUP_ENABLED : MSN_APP_STARTUP_DISABLED))
    {
        MSN_APP_Data.CmdCounter++;
        CFE_EVS_SendEvent(MSN_APP_STARTUP_SCR_INF_EID, CFE_EVS_EventType_INFORMATION, "MSN: %s is already %s",
                          AppName, Msg->Payload.Enable ? "enabled" : "disabled");
        return CFE_SUCCESS;
    }

    /*
    ** Write the script in four pieces: a disabled entry goes just below the marker, an enabled
    ** one just above it, or to the top for a library so it still loads before the apps using it
    */
    EntryEnd  = strchr(Entry, '\n') + 1;
    MarkerEnd = strchr(Marker, '\n') + 1;
    End       = MSN_APP_Data.StartupBuf + MSN_APP_Data.StartupLength;
    if (Msg->Payload.Enable == 0)
    {
        From[0] = MSN_APP_Data.StartupBuf; To[0] = Entry;
        From[1] = EntryEnd;                To[1] = MarkerEnd;
        From[2] = Entry;                   To[2] = EntryEnd;
        From[3] = MarkerEnd;               To[3] = End;
    }
    else
    {
        Dest    = (strncmp(Entry, "CFE_LIB", 7) == 0) ? MSN_APP_Data.StartupBuf : Marker;
        From[0] = MSN_APP_Data.StartupBuf; To[0] = Dest;
        From[1] = Entry;                   To[1] = EntryEnd;
        From[2] = Dest;                    To[2] = Entry;
        From[3] = EntryEnd;                To[3] = End;
    }

    /* A temporary file and a rename keep the old script if writing fails */
    if (OS_OpenCreate(&FileId, MSN_APP_STARTUP_TEMP, OS_FILE_FLAG_CREATE | OS_FILE_FLAG_TRUNCATE, OS_WRITE_ONLY) !=
        OS_SUCCESS)
    {
        Written = false;
    }
    else
    {
        for (i = 0; i < 4; i++)
        {
            if (To[i] > From[i] && OS_write(FileId, From[i], To[i] - From[i]) != To[i] - From[i])
            {
                Written = false;
            }
        }
        OS_close(FileId);
    }

    if (Written == false || OS_rename(MSN_APP_STARTUP_TEMP, MSN_APP_STARTUP_FILE) != OS_SUCCESS)
    {
        OS_remove(MSN_APP_STARTUP_TEMP);
        MSN_APP_Data.ErrCounter++;
        CFE_EVS_SendEvent(MSN_APP_FILE_ERR_EID, CFE_EVS_EventType_ERROR, "MSN: Cannot write %s",
                          MSN_APP_STARTUP_FILE);
        return CFE_STATUS_EXTERNAL_RESOURCE_FAIL;
    }

    MSN_APP_Data.CmdCounter++;
    CFE_EVS_SendEvent(MSN_APP_STARTUP_SCR_INF_EID, CFE_EVS_EventType_INFORMATION,
                      "MSN: %s %s in %s from the next cFS start", AppName,
                      Msg->Payload.Enable ? "enabled" : "disabled", MSN_APP_STARTUP_FILE);

    return CFE_SUCCESS;
}

/*
** MSN_APP_RunDiag() -- run the checks below and send DIAG telemetry:
** 0 temperature file readable, 1 temperature within limits, 2 parameter file matches,
** 3 essential apps enabled in the startup script, 4 essential apps running
*/
int32 MSN_APP_RunDiag(const MSN_APP_RunDiagCmd_t *Msg)
{
    MSN_APP_DiagTlm_Payload_t *Payload = &MSN_APP_Data.DiagTlm.Payload;
    int32                      FileParam[MSN_APP_PARAM_COUNT];
    CFE_ES_AppId_t             AppId;
    char                      *Entry;
    char                      *Marker;
    int32                      i;

    memset(Payload, 0, sizeof(*Payload));

    Payload->Result[0] = (MSN_APP_ReadTemperature() != CFE_SUCCESS);
    Payload->Result[1] = (MSN_APP_Data.TempStatus != MSN_APP_TEMP_NORMAL);
    Payload->Result[2] = (MSN_APP_ReadParamFile(FileParam) == false ||
                          memcmp(FileParam, MSN_APP_Data.Param, sizeof(FileParam)) != 0);

    for (i = 0; i < MSN_APP_ESSENTIAL_COUNT; i++)
    {
        if (MSN_APP_FindStartupEntry(MSN_APP_EssentialApps[i], &Entry, &Marker) != MSN_APP_STARTUP_ENABLED)
        {
            Payload->Result[3] = 1;
        }
        if (CFE_ES_GetAppIDByName(&AppId, MSN_APP_EssentialApps[i]) != CFE_SUCCESS)
        {
            Payload->Result[4] = 1;
        }
    }

    for (i = 0; i < MSN_APP_DIAG_CHECK_COUNT; i++)
    {
        Payload->FailCount += Payload->Result[i];
    }
    MSN_APP_Data.DiagRunCount++;
    Payload->RunCount = MSN_APP_Data.DiagRunCount;

    CFE_SB_TimeStampMsg(CFE_MSG_PTR(MSN_APP_Data.DiagTlm.TelemetryHeader));
    CFE_SB_TransmitMsg(CFE_MSG_PTR(MSN_APP_Data.DiagTlm.TelemetryHeader), true);

    MSN_APP_Data.CmdCounter++;
    CFE_EVS_SendEvent(Payload->FailCount ? MSN_APP_DIAG_ERR_EID : MSN_APP_DIAG_INF_EID,
                      Payload->FailCount ? CFE_EVS_EventType_ERROR : CFE_EVS_EventType_INFORMATION,
                      "MSN: Diagnostics %u of %u failed (%u%u%u%u%u)", (unsigned int)Payload->FailCount,
                      (unsigned int)MSN_APP_DIAG_CHECK_COUNT, (unsigned int)Payload->Result[0],
                      (unsigned int)Payload->Result[1], (unsigned int)Payload->Result[2],
                      (unsigned int)Payload->Result[3], (unsigned int)Payload->Result[4]);

    return CFE_SUCCESS;
}

/* MSN_APP_GetAppStatus() -- send APP_STATUS telemetry from ES and the startup script */
int32 MSN_APP_GetAppStatus(const MSN_APP_GetAppStatusCmd_t *Msg)
{
    MSN_APP_AppStatusTlm_Payload_t *Payload = &MSN_APP_Data.AppStatusTlm.Payload;
    CFE_ES_AppId_t                  AppId;
    CFE_ES_AppInfo_t                AppInfo;
    char                           *Entry;
    char                           *Marker;
    int32                           State;

    memset(Payload, 0, sizeof(*Payload));
    CFE_SB_MessageStringGet(Payload->AppName, Msg->Payload.AppName, NULL, sizeof(Payload->AppName),
                            sizeof(Msg->Payload.AppName));

    if (CFE_ES_GetAppIDByName(&AppId, Payload->AppName) == CFE_SUCCESS &&
        CFE_ES_GetAppInfo(&AppInfo, AppId) == CFE_SUCCESS)
    {
        Payload->Running          = 1;
        Payload->Priority         = AppInfo.Priority;
        Payload->StackSize        = AppInfo.StackSize;
        Payload->ExecutionCounter = AppInfo.ExecutionCounter;
        Payload->ChildTasks       = AppInfo.NumOfChildTasks;
    }

    State            = MSN_APP_FindStartupEntry(Payload->AppName, &Entry, &Marker);
    Payload->Startup = (State < 0) ? MSN_APP_STARTUP_NOT_FOUND : State;

    CFE_SB_TimeStampMsg(CFE_MSG_PTR(MSN_APP_Data.AppStatusTlm.TelemetryHeader));
    CFE_SB_TransmitMsg(CFE_MSG_PTR(MSN_APP_Data.AppStatusTlm.TelemetryHeader), true);

    MSN_APP_Data.CmdCounter++;
    CFE_EVS_SendEvent(MSN_APP_APP_STATUS_INF_EID, CFE_EVS_EventType_INFORMATION,
                      "MSN: %s %s, startup %s, %lu runs", Payload->AppName,
                      Payload->Running ? "running" : "not running",
                      (Payload->Startup == MSN_APP_STARTUP_ENABLED)
                          ? "enabled"
                          : ((Payload->Startup == MSN_APP_STARTUP_DISABLED) ? "disabled" : "not found"),
                      (unsigned long)Payload->ExecutionCounter);

    return CFE_SUCCESS;
}

/* MSN_APP_SendTemperature() -- read the temperature file and send TEMP telemetry */
int32 MSN_APP_SendTemperature(void)
{
    MSN_APP_TempTlm_Payload_t *Payload = &MSN_APP_Data.TempTlm.Payload;
    int32                      status;

    status = MSN_APP_ReadTemperature();

    Payload->Temperature = MSN_APP_Data.Temperature;
    Payload->Status      = MSN_APP_Data.TempStatus;
    Payload->SampleCount = MSN_APP_Data.TempSampleCount;

    CFE_SB_TimeStampMsg(CFE_MSG_PTR(MSN_APP_Data.TempTlm.TelemetryHeader));
    CFE_SB_TransmitMsg(CFE_MSG_PTR(MSN_APP_Data.TempTlm.TelemetryHeader), true);

    return status;
}

/*
** MSN_APP_ReadTemperature() -- read one number (deg C) from the temperature file, compare it
** with the limits and send an event only when the status changes
*/
int32 MSN_APP_ReadTemperature(void)
{
    osal_id_t FileId;
    char      Text[32];
    int32     Length = 0;
    float     Value;
    uint8     Status = MSN_APP_TEMP_NO_DATA;

    if (OS_OpenCreate(&FileId, MSN_APP_TEMP_FILE, OS_FILE_FLAG_NONE, OS_READ_ONLY) == OS_SUCCESS)
    {
        Length = OS_read(FileId, Text, sizeof(Text) - 1);
        OS_close(FileId);
    }
    Text[(Length > 0) ? Length : 0] = '\0';

    if (sscanf(Text, "%f", &Value) == 1)
    {
        MSN_APP_Data.Temperature = Value;
        MSN_APP_Data.TempSampleCount++;
        Status = MSN_APP_TEMP_NORMAL;
        if (Value > MSN_APP_Data.Param[MSN_APP_PARAM_TEMP_HIGH])
        {
            Status = MSN_APP_TEMP_HIGH;
        }
        else if (Value < MSN_APP_Data.Param[MSN_APP_PARAM_TEMP_LOW])
        {
            Status = MSN_APP_TEMP_LOW;
        }
    }

    if (Status != MSN_APP_Data.TempStatus && Status == MSN_APP_TEMP_NO_DATA)
    {
        MSN_APP_Data.TempStatus = Status;
        CFE_EVS_SendEvent(MSN_APP_TEMP_ERR_EID, CFE_EVS_EventType_ERROR, "MSN: No temperature in %s",
                          MSN_APP_TEMP_FILE);
    }
    else if (Status != MSN_APP_Data.TempStatus)
    {
        MSN_APP_Data.TempStatus = Status;
        CFE_EVS_SendEvent((Status == MSN_APP_TEMP_NORMAL) ? MSN_APP_TEMP_INF_EID : MSN_APP_TEMP_ERR_EID,
                          (Status == MSN_APP_TEMP_NORMAL) ? CFE_EVS_EventType_INFORMATION : CFE_EVS_EventType_ERROR,
                          "MSN: Temperature %.2f C is %s (limits %ld / %ld)", MSN_APP_Data.Temperature,
                          MSN_APP_TempStatusName[Status], (long)MSN_APP_Data.Param[MSN_APP_PARAM_TEMP_HIGH],
                          (long)MSN_APP_Data.Param[MSN_APP_PARAM_TEMP_LOW]);
    }

    return (Status == MSN_APP_TEMP_NO_DATA) ? CFE_STATUS_EXTERNAL_RESOURCE_FAIL : CFE_SUCCESS;
}

/* MSN_APP_ReadParamFile() -- read "high low period" from the parameter file */
bool MSN_APP_ReadParamFile(int32 *Param)
{
    osal_id_t FileId;
    char      Line[64];
    int32     Length = 0;
    long      Value[MSN_APP_PARAM_COUNT];
    int32     i;

    if (OS_OpenCreate(&FileId, MSN_APP_PARAM_FILE, OS_FILE_FLAG_NONE, OS_READ_ONLY) == OS_SUCCESS)
    {
        Length = OS_read(FileId, Line, sizeof(Line) - 1);
        OS_close(FileId);
    }
    Line[(Length > 0) ? Length : 0] = '\0';

    if (sscanf(Line, "%ld %ld %ld", &Value[0], &Value[1], &Value[2]) != MSN_APP_PARAM_COUNT)
    {
        return false;
    }
    for (i = 0; i < MSN_APP_PARAM_COUNT; i++)
    {
        Param[i] = Value[i];
    }
    return true;
}

/* MSN_APP_SaveParams() -- rewrite the parameter file with the current values */
void MSN_APP_SaveParams(void)
{
    osal_id_t FileId;
    char      Line[64];

    if (OS_OpenCreate(&FileId, MSN_APP_PARAM_FILE, OS_FILE_FLAG_CREATE | OS_FILE_FLAG_TRUNCATE, OS_WRITE_ONLY) !=
        OS_SUCCESS)
    {
        CFE_EVS_SendEvent(MSN_APP_FILE_ERR_EID, CFE_EVS_EventType_ERROR, "MSN: Cannot write %s", MSN_APP_PARAM_FILE);
        return;
    }

    snprintf(Line, sizeof(Line), "%ld %ld %ld\n", (long)MSN_APP_Data.Param[0], (long)MSN_APP_Data.Param[1],
             (long)MSN_APP_Data.Param[2]);
    OS_write(FileId, Line, strlen(Line));
    OS_close(FileId);
}

/*
** MSN_APP_FindStartupEntry() -- load the startup script into StartupBuf and find the line whose
** 4th field is AppName and the first '!' line. Returns MSN_APP_STARTUP_* or a negative error.
*/
int32 MSN_APP_FindStartupEntry(const char *AppName, char **Entry, char **Marker)
{
    char     *Buf        = MSN_APP_Data.StartupBuf;
    size_t    NameLength = strlen(AppName);
    osal_id_t FileId;
    int32     Length = 0;
    int32     Commas;
    char     *Line;
    char     *Field;
    bool      Active = true;
    int32     State  = MSN_APP_STARTUP_NOT_FOUND;

    *Entry  = NULL;
    *Marker = NULL;

    /* Leave room for a final newline, a "!\n" marker and the terminator */
    if (OS_OpenCreate(&FileId, MSN_APP_STARTUP_FILE, OS_FILE_FLAG_NONE, OS_READ_ONLY) == OS_SUCCESS)
    {
        Length = OS_read(FileId, Buf, MSN_APP_STARTUP_MAX_SIZE - 4);
        OS_close(FileId);
    }
    if (Length <= 0 || Length >= MSN_APP_STARTUP_MAX_SIZE - 4)
    {
        CFE_EVS_SendEvent(MSN_APP_FILE_ERR_EID, CFE_EVS_EventType_ERROR, "MSN: Cannot read %s or it is too large",
                          MSN_APP_STARTUP_FILE);
        return CFE_STATUS_EXTERNAL_RESOURCE_FAIL;
    }
    if (Buf[Length - 1] != '\n')
    {
        Buf[Length++] = '\n';
    }
    Buf[Length] = '\0';

    for (Line = Buf; *Line != '\0'; Line = strchr(Line, '\n') + 1)
    {
        if (Line[0] == '!')
        {
            if (Active)
            {
                *Marker = Line;
                Active  = false;
            }
            continue;
        }

        /* Skip three commas to reach the cFE name, staying inside this line */
        Commas = 0;
        for (Field = Line; *Field != '\n' && Commas < 3; Field++)
        {
            if (*Field == ',')
            {
                Commas++;
            }
        }
        while (*Field == ' ' || *Field == '\t')
        {
            Field++;
        }

        if (*Entry == NULL && Commas == 3 && NameLength > 0 && strncmp(Field, AppName, NameLength) == 0 &&
            (Field[NameLength] == ',' || Field[NameLength] == ' ' || Field[NameLength] == '\t'))
        {
            *Entry = Line;
            State  = Active ? MSN_APP_STARTUP_ENABLED : MSN_APP_STARTUP_DISABLED;
        }
    }

    if (*Marker == NULL)
    {
        *Marker = Buf + Length;
        strcpy(*Marker, "!\n");
        Length += 2;
    }
    MSN_APP_Data.StartupLength = Length;

    return State;
}

/* MSN_APP_VerifyCmdLength() -- Verify command packet length */
bool MSN_APP_VerifyCmdLength(CFE_MSG_Message_t *MsgPtr, size_t ExpectedLength)
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

        CFE_EVS_SendEvent(MSN_APP_LEN_ERR_EID, CFE_EVS_EventType_ERROR,
                          "MSN: Invalid Msg length: ID = 0x%X,  CC = %u, Len = %u, Expected = %u",
                          (unsigned int)CFE_SB_MsgIdToValue(MsgId), (unsigned int)FcnCode, (unsigned int)ActualLength,
                          (unsigned int)ExpectedLength);

        result = false;
        MSN_APP_Data.ErrCounter++;
    }

    return result;
}
