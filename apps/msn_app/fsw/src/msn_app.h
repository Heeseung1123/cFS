/* MSN_APP: mission app for sensor read, device and thruster control, and log/analysis */
#ifndef MSN_APP_H
#define MSN_APP_H

#include "cfe.h"

#include "msn_app_perfids.h"
#include "msn_app_msgids.h"
#include "msn_app_msg.h"

#define MSN_APP_PIPE_DEPTH 32

#define MSN_APP_CMD_REJECTED_ERR_CODE -1

/* Files stand in for real hardware: the sensor file is read, the state file is rewritten */
#define MSN_APP_SENSOR_FILE "/cf/msn_sensor.csv"
#define MSN_APP_STATE_FILE  "/cf/msn_state.txt"
#define MSN_APP_LOG_FILE    "/cf/msn_log.txt"

/* Thruster model: |dV| limit (cm/s), altitude change per m/s and fuel per m/s at ~400 km */
#define MSN_APP_MAX_DELTAV   1000
#define MSN_APP_KM_PER_MPS   1.76f
#define MSN_APP_FUEL_PER_MPS 0.05f

typedef struct
{
    uint8 CmdCounter;
    uint8 ErrCounter;

    /* Scenario A: device, thruster and orbit state (kept in MSN_APP_STATE_FILE) */
    bool  ThrusterArmed;
    uint8 DeviceState[MSN_APP_DEVICE_COUNT];
    float Altitude;
    float Fuel;
    int32 SensorOffset;

    /* Scenario C: statistics reported by GET_ANALYSIS */
    MSN_APP_AnalysisTlm_Payload_t Stats;
    float                         SensorSum[MSN_APP_SENSOR_COUNT];

    MSN_APP_HkTlm_t       HkTlm;
    MSN_APP_SensorTlm_t   SensorTlm;
    MSN_APP_AnalysisTlm_t AnalysisTlm;
    MSN_APP_LogTlm_t      LogTlm;

    uint32          RunStatus;
    CFE_SB_PipeId_t CommandPipe;
} MSN_APP_Data_t;

void  MSN_APP_Main(void);
int32 MSN_APP_Init(void);
void  MSN_APP_ProcessCommandPacket(CFE_SB_Buffer_t *SBBufPtr);
void  MSN_APP_ProcessGroundCommand(CFE_SB_Buffer_t *SBBufPtr);
int32 MSN_APP_ReportHousekeeping(const CFE_MSG_CommandHeader_t *Msg);
bool  MSN_APP_VerifyCmdLength(CFE_MSG_Message_t *MsgPtr, size_t ExpectedLength);

int32 MSN_APP_ReadSensor(const MSN_APP_ReadSensorCmd_t *Msg);
int32 MSN_APP_SetDevice(const MSN_APP_SetDeviceCmd_t *Msg);
int32 MSN_APP_FireThruster(const MSN_APP_FireThrusterCmd_t *Msg);
int32 MSN_APP_GetAnalysis(const MSN_APP_GetAnalysisCmd_t *Msg);
int32 MSN_APP_GetLog(const MSN_APP_GetLogCmd_t *Msg);
int32 MSN_APP_ResetAnalysis(const MSN_APP_ResetAnalysisCmd_t *Msg);

void MSN_APP_LoadState(void);
void MSN_APP_SaveState(void);
void MSN_APP_WriteLog(const char *Text);

#endif /* MSN_APP_H */
