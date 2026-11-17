/*
** MSN_APP: mission app for scenario A (sensor read, device and thruster control)
** and scenario C (log and analysis). Files stand in for the real hardware.
*/

#include <stdio.h>
#include <string.h>

#include "msn_app_events.h"
#include "msn_app_version.h"
#include "msn_app.h"

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
    MSN_APP_Data.RunStatus = CFE_ES_RunStatus_APP_RUN;

    status = CFE_EVS_Register(NULL, 0, CFE_EVS_EventFilter_BINARY);
    if (status != CFE_SUCCESS)
    {
        CFE_ES_WriteToSysLog("MSN App: Error Registering Events, RC = 0x%08lX\n", (unsigned long)status);
        return status;
    }

    CFE_MSG_Init(CFE_MSG_PTR(MSN_APP_Data.HkTlm.TelemetryHeader), CFE_SB_ValueToMsgId(MSN_APP_HK_TLM_MID),
                 sizeof(MSN_APP_Data.HkTlm));
    CFE_MSG_Init(CFE_MSG_PTR(MSN_APP_Data.SensorTlm.TelemetryHeader), CFE_SB_ValueToMsgId(MSN_APP_SENSOR_TLM_MID),
                 sizeof(MSN_APP_Data.SensorTlm));
    CFE_MSG_Init(CFE_MSG_PTR(MSN_APP_Data.AnalysisTlm.TelemetryHeader),
                 CFE_SB_ValueToMsgId(MSN_APP_ANALYSIS_TLM_MID), sizeof(MSN_APP_Data.AnalysisTlm));
    CFE_MSG_Init(CFE_MSG_PTR(MSN_APP_Data.LogTlm.TelemetryHeader), CFE_SB_ValueToMsgId(MSN_APP_LOG_TLM_MID),
                 sizeof(MSN_APP_Data.LogTlm));

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

    MSN_APP_LoadState();
    MSN_APP_WriteLog("MSN App started");

    CFE_EVS_SendEvent(MSN_APP_STARTUP_INF_EID, CFE_EVS_EventType_INFORMATION,
                      "MSN App Initialized.%s Altitude %.2f km, Fuel %.3f kg", MSN_APP_VERSION_STRING,
                      MSN_APP_Data.Altitude, MSN_APP_Data.Fuel);

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

    /* ARM is valid only for the very next command, so any other command disarms */
    if (CommandCode != MSN_APP_FIRE_THRUSTER_CC)
    {
        MSN_APP_Data.ThrusterArmed = false;
    }

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

        case MSN_APP_READ_SENSOR_CC:
            if (MSN_APP_VerifyCmdLength(&SBBufPtr->Msg, sizeof(MSN_APP_ReadSensorCmd_t)))
            {
                MSN_APP_ReadSensor((MSN_APP_ReadSensorCmd_t *)SBBufPtr);
            }
            break;

        case MSN_APP_SET_DEVICE_CC:
            if (MSN_APP_VerifyCmdLength(&SBBufPtr->Msg, sizeof(MSN_APP_SetDeviceCmd_t)))
            {
                MSN_APP_SetDevice((MSN_APP_SetDeviceCmd_t *)SBBufPtr);
            }
            break;

        case MSN_APP_ARM_THRUSTER_CC:
            if (MSN_APP_VerifyCmdLength(&SBBufPtr->Msg, sizeof(MSN_APP_ArmThrusterCmd_t)))
            {
                MSN_APP_Data.ThrusterArmed = true;
                MSN_APP_Data.CmdCounter++;
                CFE_EVS_SendEvent(MSN_APP_ARM_INF_EID, CFE_EVS_EventType_INFORMATION,
                                  "MSN: Thruster armed, FIRE must be the next command");
            }
            break;

        case MSN_APP_FIRE_THRUSTER_CC:
            if (MSN_APP_VerifyCmdLength(&SBBufPtr->Msg, sizeof(MSN_APP_FireThrusterCmd_t)))
            {
                MSN_APP_FireThruster((MSN_APP_FireThrusterCmd_t *)SBBufPtr);
            }
            MSN_APP_Data.ThrusterArmed = false;
            break;

        case MSN_APP_GET_ANALYSIS_CC:
            if (MSN_APP_VerifyCmdLength(&SBBufPtr->Msg, sizeof(MSN_APP_GetAnalysisCmd_t)))
            {
                MSN_APP_GetAnalysis((MSN_APP_GetAnalysisCmd_t *)SBBufPtr);
            }
            break;

        case MSN_APP_GET_LOG_CC:
            if (MSN_APP_VerifyCmdLength(&SBBufPtr->Msg, sizeof(MSN_APP_GetLogCmd_t)))
            {
                MSN_APP_GetLog((MSN_APP_GetLogCmd_t *)SBBufPtr);
            }
            break;

        case MSN_APP_RESET_ANALYSIS_CC:
            if (MSN_APP_VerifyCmdLength(&SBBufPtr->Msg, sizeof(MSN_APP_ResetAnalysisCmd_t)))
            {
                MSN_APP_ResetAnalysis((MSN_APP_ResetAnalysisCmd_t *)SBBufPtr);
            }
            break;

        default:
            MSN_APP_Data.ErrCounter++;
            CFE_EVS_SendEvent(MSN_APP_COMMAND_ERR_EID, CFE_EVS_EventType_ERROR, "MSN: Invalid ground command code: CC = %d",
                              CommandCode);
            break;
    }
}

/* MSN_APP_ReportHousekeeping() -- send HK telemetry on request from SCH */
int32 MSN_APP_ReportHousekeeping(const CFE_MSG_CommandHeader_t *Msg)
{
    MSN_APP_HkTlm_Payload_t *Payload = &MSN_APP_Data.HkTlm.Payload;

    Payload->CommandCounter      = MSN_APP_Data.CmdCounter;
    Payload->CommandErrorCounter = MSN_APP_Data.ErrCounter;
    Payload->ThrusterArmed       = MSN_APP_Data.ThrusterArmed;
    memcpy(Payload->DeviceState, MSN_APP_Data.DeviceState, sizeof(Payload->DeviceState));
    Payload->Altitude = MSN_APP_Data.Altitude;
    Payload->Fuel     = MSN_APP_Data.Fuel;

    CFE_SB_TimeStampMsg(CFE_MSG_PTR(MSN_APP_Data.HkTlm.TelemetryHeader));
    CFE_SB_TransmitMsg(CFE_MSG_PTR(MSN_APP_Data.HkTlm.TelemetryHeader), true);

    return CFE_SUCCESS;
}

/*
** MSN_APP_ReadSensor() -- read the next "temp,volt,curr" line of the sensor
** file (wrapping at its end), update statistics and send SENSOR telemetry
*/
int32 MSN_APP_ReadSensor(const MSN_APP_ReadSensorCmd_t *Msg)
{
    osal_id_t FileId;
    char      Line[80];
    int32     Length;
    int32     i;
    float     Value[MSN_APP_SENSOR_COUNT];

    if (OS_OpenCreate(&FileId, MSN_APP_SENSOR_FILE, OS_FILE_FLAG_NONE, OS_READ_ONLY) != OS_SUCCESS)
    {
        MSN_APP_Data.ErrCounter++;
        CFE_EVS_SendEvent(MSN_APP_FILE_ERR_EID, CFE_EVS_EventType_ERROR, "MSN: Cannot open %s",
                          MSN_APP_SENSOR_FILE);
        return CFE_STATUS_EXTERNAL_RESOURCE_FAIL;
    }

    OS_lseek(FileId, MSN_APP_Data.SensorOffset, OS_SEEK_SET);
    Length = OS_read(FileId, Line, sizeof(Line) - 1);
    if (Length <= 0)
    {
        MSN_APP_Data.SensorOffset = 0;
        OS_lseek(FileId, 0, OS_SEEK_SET);
        Length = OS_read(FileId, Line, sizeof(Line) - 1);
    }
    OS_close(FileId);

    /* Keep only the first line and move the offset past it */
    if (Length < 0)
    {
        Length = 0;
    }
    Line[Length] = '\0';
    for (i = 0; i < Length && Line[i] != '\n'; i++)
    {
    }
    Line[i] = '\0';
    MSN_APP_Data.SensorOffset += i + 1;

    if (sscanf(Line, "%f,%f,%f", &Value[0], &Value[1], &Value[2]) != MSN_APP_SENSOR_COUNT)
    {
        MSN_APP_Data.ErrCounter++;
        CFE_EVS_SendEvent(MSN_APP_SENSOR_ERR_EID, CFE_EVS_EventType_ERROR, "MSN: Bad sensor line '%s'", Line);
        return CFE_STATUS_EXTERNAL_RESOURCE_FAIL;
    }

    for (i = 0; i < MSN_APP_SENSOR_COUNT; i++)
    {
        if (MSN_APP_Data.Stats.SampleCount == 0 || Value[i] < MSN_APP_Data.Stats.Min[i])
        {
            MSN_APP_Data.Stats.Min[i] = Value[i];
        }
        if (MSN_APP_Data.Stats.SampleCount == 0 || Value[i] > MSN_APP_Data.Stats.Max[i])
        {
            MSN_APP_Data.Stats.Max[i] = Value[i];
        }
        MSN_APP_Data.SensorSum[i] += Value[i];
        MSN_APP_Data.SensorTlm.Payload.Value[i] = Value[i];
    }
    MSN_APP_Data.Stats.SampleCount++;
    MSN_APP_Data.SensorTlm.Payload.SampleCount = MSN_APP_Data.Stats.SampleCount;

    CFE_SB_TimeStampMsg(CFE_MSG_PTR(MSN_APP_Data.SensorTlm.TelemetryHeader));
    CFE_SB_TransmitMsg(CFE_MSG_PTR(MSN_APP_Data.SensorTlm.TelemetryHeader), true);

    MSN_APP_Data.CmdCounter++;
    CFE_EVS_SendEvent(MSN_APP_SENSOR_INF_EID, CFE_EVS_EventType_INFORMATION, "MSN: Sensor %.2f C, %.2f V, %.3f A",
                      Value[0], Value[1], Value[2]);

    return CFE_SUCCESS;
}

/* MSN_APP_SetDevice() -- switch a device on or off and save it to the file */
int32 MSN_APP_SetDevice(const MSN_APP_SetDeviceCmd_t *Msg)
{
    char Text[64];

    if (Msg->Payload.DeviceId >= MSN_APP_DEVICE_COUNT || Msg->Payload.State > 1)
    {
        MSN_APP_Data.ErrCounter++;
        CFE_EVS_SendEvent(MSN_APP_DEVICE_ERR_EID, CFE_EVS_EventType_ERROR, "MSN: Invalid device %u or state %u",
                          (unsigned int)Msg->Payload.DeviceId, (unsigned int)Msg->Payload.State);
        return MSN_APP_CMD_REJECTED_ERR_CODE;
    }

    MSN_APP_Data.DeviceState[Msg->Payload.DeviceId] = Msg->Payload.State;
    MSN_APP_Data.Stats.DeviceSwitchCount++;
    MSN_APP_SaveState();

    snprintf(Text, sizeof(Text), "SET_DEVICE %u %s", (unsigned int)Msg->Payload.DeviceId,
             Msg->Payload.State ? "ON" : "OFF");
    MSN_APP_WriteLog(Text);

    MSN_APP_Data.CmdCounter++;
    CFE_EVS_SendEvent(MSN_APP_DEVICE_INF_EID, CFE_EVS_EventType_INFORMATION, "MSN: %s", Text);

    return CFE_SUCCESS;
}

/*
** MSN_APP_FireThruster() -- orbit maneuver: needs ARM first, then checks the
** dV range and the fuel, and updates altitude and fuel in the state file
*/
int32 MSN_APP_FireThruster(const MSN_APP_FireThrusterCmd_t *Msg)
{
    char  Text[80];
    float DeltaV    = Msg->Payload.DeltaV / 100.0f;
    float Magnitude = (DeltaV < 0) ? -DeltaV : DeltaV;
    float FuelUsed  = Magnitude * MSN_APP_FUEL_PER_MPS;

    Text[0] = '\0';
    if (MSN_APP_Data.ThrusterArmed == false)
    {
        snprintf(Text, sizeof(Text), "FIRE rejected: thruster not armed");
    }
    else if (Msg->Payload.DeltaV > MSN_APP_MAX_DELTAV || Msg->Payload.DeltaV < -MSN_APP_MAX_DELTAV)
    {
        snprintf(Text, sizeof(Text), "FIRE rejected: dV %d cm/s out of range", (int)Msg->Payload.DeltaV);
    }
    else if (FuelUsed > MSN_APP_Data.Fuel)
    {
        snprintf(Text, sizeof(Text), "FIRE rejected: needs %.3f kg fuel, %.3f kg left", FuelUsed, MSN_APP_Data.Fuel);
    }

    if (Text[0] != '\0')
    {
        MSN_APP_WriteLog(Text);
        MSN_APP_Data.ErrCounter++;
        CFE_EVS_SendEvent(MSN_APP_FIRE_ERR_EID, CFE_EVS_EventType_ERROR, "MSN: %s", Text);
        return MSN_APP_CMD_REJECTED_ERR_CODE;
    }

    MSN_APP_Data.Altitude += DeltaV * MSN_APP_KM_PER_MPS;
    MSN_APP_Data.Fuel -= FuelUsed;
    MSN_APP_Data.Stats.ManeuverCount++;
    MSN_APP_Data.Stats.TotalDeltaV += Magnitude;
    MSN_APP_SaveState();

    snprintf(Text, sizeof(Text), "FIRE dV %.2f m/s, altitude %.2f km, fuel %.3f kg", DeltaV, MSN_APP_Data.Altitude,
             MSN_APP_Data.Fuel);
    MSN_APP_WriteLog(Text);

    MSN_APP_Data.CmdCounter++;
    CFE_EVS_SendEvent(MSN_APP_FIRE_INF_EID, CFE_EVS_EventType_INFORMATION, "MSN: %s", Text);

    return CFE_SUCCESS;
}

/* MSN_APP_GetAnalysis() -- send the collected statistics as ANALYSIS tlm */
int32 MSN_APP_GetAnalysis(const MSN_APP_GetAnalysisCmd_t *Msg)
{
    MSN_APP_AnalysisTlm_Payload_t *Payload = &MSN_APP_Data.AnalysisTlm.Payload;
    int32                          i;

    *Payload = MSN_APP_Data.Stats;
    for (i = 0; i < MSN_APP_SENSOR_COUNT; i++)
    {
        if (Payload->SampleCount > 0)
        {
            Payload->Avg[i] = MSN_APP_Data.SensorSum[i] / Payload->SampleCount;
        }
    }
    Payload->Uptime = CFE_TIME_GetMETseconds();

    CFE_SB_TimeStampMsg(CFE_MSG_PTR(MSN_APP_Data.AnalysisTlm.TelemetryHeader));
    CFE_SB_TransmitMsg(CFE_MSG_PTR(MSN_APP_Data.AnalysisTlm.TelemetryHeader), true);

    MSN_APP_Data.CmdCounter++;
    CFE_EVS_SendEvent(MSN_APP_ANALYSIS_INF_EID, CFE_EVS_EventType_INFORMATION,
                      "MSN: Analysis sent, %lu samples, %lu maneuvers", (unsigned long)Payload->SampleCount,
                      (unsigned long)Payload->ManeuverCount);

    return CFE_SUCCESS;
}

/*
** MSN_APP_GetLog() -- send one chunk of the log file from the given offset;
** the ground repeats with Offset += Length until Length is 0
*/
int32 MSN_APP_GetLog(const MSN_APP_GetLogCmd_t *Msg)
{
    MSN_APP_LogTlm_Payload_t *Payload = &MSN_APP_Data.LogTlm.Payload;
    osal_id_t                 FileId;
    int32                     FileSize = 0;
    int32                     Length   = 0;

    memset(Payload, 0, sizeof(*Payload));

    if (OS_OpenCreate(&FileId, MSN_APP_LOG_FILE, OS_FILE_FLAG_NONE, OS_READ_ONLY) == OS_SUCCESS)
    {
        FileSize = OS_lseek(FileId, 0, OS_SEEK_END);
        if (FileSize > 0 && Msg->Payload.Offset < (uint32)FileSize)
        {
            OS_lseek(FileId, Msg->Payload.Offset, OS_SEEK_SET);
            Length = OS_read(FileId, Payload->Text, sizeof(Payload->Text));
        }
        OS_close(FileId);
    }

    Payload->FileSize = (FileSize > 0) ? FileSize : 0;
    Payload->Offset   = Msg->Payload.Offset;
    Payload->Length   = (Length > 0) ? Length : 0;

    CFE_SB_TimeStampMsg(CFE_MSG_PTR(MSN_APP_Data.LogTlm.TelemetryHeader));
    CFE_SB_TransmitMsg(CFE_MSG_PTR(MSN_APP_Data.LogTlm.TelemetryHeader), true);

    MSN_APP_Data.CmdCounter++;
    CFE_EVS_SendEvent(MSN_APP_LOG_INF_EID, CFE_EVS_EventType_INFORMATION, "MSN: Log chunk at %lu, %u of %lu bytes",
                      (unsigned long)Payload->Offset, (unsigned int)Payload->Length,
                      (unsigned long)Payload->FileSize);

    return CFE_SUCCESS;
}

/* MSN_APP_ResetAnalysis() -- clear the statistics and empty the log file */
int32 MSN_APP_ResetAnalysis(const MSN_APP_ResetAnalysisCmd_t *Msg)
{
    osal_id_t FileId;

    memset(&MSN_APP_Data.Stats, 0, sizeof(MSN_APP_Data.Stats));
    memset(MSN_APP_Data.SensorSum, 0, sizeof(MSN_APP_Data.SensorSum));

    if (OS_OpenCreate(&FileId, MSN_APP_LOG_FILE, OS_FILE_FLAG_CREATE | OS_FILE_FLAG_TRUNCATE, OS_WRITE_ONLY) ==
        OS_SUCCESS)
    {
        OS_close(FileId);
    }
    MSN_APP_WriteLog("Analysis and log reset");

    MSN_APP_Data.CmdCounter++;
    CFE_EVS_SendEvent(MSN_APP_ANALYSIS_INF_EID, CFE_EVS_EventType_INFORMATION, "MSN: Analysis and log reset");

    return CFE_SUCCESS;
}

/*
** MSN_APP_LoadState() -- read "dev0 dev1 dev2 dev3 altitude fuel" from the
** state file, or start from the defaults and create the file
*/
void MSN_APP_LoadState(void)
{
    osal_id_t    FileId;
    char         Line[80];
    int32        Length = 0;
    unsigned int Dev[MSN_APP_DEVICE_COUNT];
    float        Altitude;
    float        Fuel;
    int32        i;

    if (OS_OpenCreate(&FileId, MSN_APP_STATE_FILE, OS_FILE_FLAG_NONE, OS_READ_ONLY) == OS_SUCCESS)
    {
        Length = OS_read(FileId, Line, sizeof(Line) - 1);
        OS_close(FileId);
    }
    Line[(Length > 0) ? Length : 0] = '\0';

    if (sscanf(Line, "%u %u %u %u %f %f", &Dev[0], &Dev[1], &Dev[2], &Dev[3], &Altitude, &Fuel) == 6)
    {
        for (i = 0; i < MSN_APP_DEVICE_COUNT; i++)
        {
            MSN_APP_Data.DeviceState[i] = (Dev[i] != 0);
        }
        MSN_APP_Data.Altitude = Altitude;
        MSN_APP_Data.Fuel     = Fuel;
    }
    else
    {
        /* Defaults: all devices off, 400 km circular orbit, 5 kg fuel */
        MSN_APP_Data.Altitude = 400.0f;
        MSN_APP_Data.Fuel     = 5.0f;
        MSN_APP_SaveState();
    }
}

/* MSN_APP_SaveState() -- rewrite the state file with the current values */
void MSN_APP_SaveState(void)
{
    osal_id_t FileId;
    char      Line[80];

    if (OS_OpenCreate(&FileId, MSN_APP_STATE_FILE, OS_FILE_FLAG_CREATE | OS_FILE_FLAG_TRUNCATE, OS_WRITE_ONLY) !=
        OS_SUCCESS)
    {
        CFE_EVS_SendEvent(MSN_APP_FILE_ERR_EID, CFE_EVS_EventType_ERROR, "MSN: Cannot write %s", MSN_APP_STATE_FILE);
        return;
    }

    snprintf(Line, sizeof(Line), "%u %u %u %u %.3f %.3f\n", (unsigned int)MSN_APP_Data.DeviceState[0],
             (unsigned int)MSN_APP_Data.DeviceState[1], (unsigned int)MSN_APP_Data.DeviceState[2],
             (unsigned int)MSN_APP_Data.DeviceState[3], MSN_APP_Data.Altitude, MSN_APP_Data.Fuel);
    OS_write(FileId, Line, strlen(Line));
    OS_close(FileId);
}

/* MSN_APP_WriteLog() -- append "<MET seconds> <text>" to the log file */
void MSN_APP_WriteLog(const char *Text)
{
    osal_id_t FileId;
    char      Line[120];

    if (OS_OpenCreate(&FileId, MSN_APP_LOG_FILE, OS_FILE_FLAG_CREATE, OS_WRITE_ONLY) != OS_SUCCESS)
    {
        CFE_EVS_SendEvent(MSN_APP_FILE_ERR_EID, CFE_EVS_EventType_ERROR, "MSN: Cannot write %s", MSN_APP_LOG_FILE);
        return;
    }

    snprintf(Line, sizeof(Line), "%lu %s\n", (unsigned long)CFE_TIME_GetMETseconds(), Text);
    OS_lseek(FileId, 0, OS_SEEK_END);
    OS_write(FileId, Line, strlen(Line));
    OS_close(FileId);
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
