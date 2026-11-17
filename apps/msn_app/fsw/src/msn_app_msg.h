/* MSN_APP command and telemetry message definitions */
#ifndef MSN_APP_MSG_H
#define MSN_APP_MSG_H

/*
** Command codes
*/
#define MSN_APP_NOOP_CC           0
#define MSN_APP_RESET_COUNTERS_CC 1
#define MSN_APP_READ_SENSOR_CC    2
#define MSN_APP_SET_DEVICE_CC     3
#define MSN_APP_ARM_THRUSTER_CC   4
#define MSN_APP_FIRE_THRUSTER_CC  5
#define MSN_APP_GET_ANALYSIS_CC   6
#define MSN_APP_GET_LOG_CC        7
#define MSN_APP_RESET_ANALYSIS_CC 8

#define MSN_APP_SENSOR_COUNT   3 /* Temperature (C), bus voltage (V), bus current (A) */
#define MSN_APP_DEVICE_COUNT   4
#define MSN_APP_LOG_CHUNK_SIZE 200

/*
** Command packets
*/
typedef struct
{
    CFE_MSG_CommandHeader_t CmdHeader;
} MSN_APP_NoArgsCmd_t;

typedef MSN_APP_NoArgsCmd_t MSN_APP_NoopCmd_t;
typedef MSN_APP_NoArgsCmd_t MSN_APP_ResetCountersCmd_t;
typedef MSN_APP_NoArgsCmd_t MSN_APP_ReadSensorCmd_t;
typedef MSN_APP_NoArgsCmd_t MSN_APP_ArmThrusterCmd_t;
typedef MSN_APP_NoArgsCmd_t MSN_APP_GetAnalysisCmd_t;
typedef MSN_APP_NoArgsCmd_t MSN_APP_ResetAnalysisCmd_t;

typedef struct
{
    uint8 DeviceId; /* 0 .. MSN_APP_DEVICE_COUNT-1 */
    uint8 State;    /* 0 = off, 1 = on */
    uint8 Spare[2];
} MSN_APP_SetDevice_Payload_t;

typedef struct
{
    CFE_MSG_CommandHeader_t     CmdHeader;
    MSN_APP_SetDevice_Payload_t Payload;
} MSN_APP_SetDeviceCmd_t;

typedef struct
{
    int16 DeltaV; /* cm/s, positive raises the orbit */
    uint8 Spare[2];
} MSN_APP_FireThruster_Payload_t;

typedef struct
{
    CFE_MSG_CommandHeader_t        CmdHeader;
    MSN_APP_FireThruster_Payload_t Payload;
} MSN_APP_FireThrusterCmd_t;

typedef struct
{
    uint32 Offset; /* Byte offset in the log file */
} MSN_APP_GetLog_Payload_t;

typedef struct
{
    CFE_MSG_CommandHeader_t  CmdHeader;
    MSN_APP_GetLog_Payload_t Payload;
} MSN_APP_GetLogCmd_t;

/*
** Telemetry packets
*/
typedef struct
{
    uint8 CommandCounter;
    uint8 CommandErrorCounter;
    uint8 ThrusterArmed;
    uint8 Spare;
    uint8 DeviceState[MSN_APP_DEVICE_COUNT];
    float Altitude; /* km */
    float Fuel;     /* kg */
} MSN_APP_HkTlm_Payload_t;

typedef struct
{
    CFE_MSG_TelemetryHeader_t TelemetryHeader;
    MSN_APP_HkTlm_Payload_t   Payload;
} MSN_APP_HkTlm_t;

typedef struct
{
    uint32 SampleCount;
    float  Value[MSN_APP_SENSOR_COUNT];
} MSN_APP_SensorTlm_Payload_t;

typedef struct
{
    CFE_MSG_TelemetryHeader_t   TelemetryHeader;
    MSN_APP_SensorTlm_Payload_t Payload;
} MSN_APP_SensorTlm_t;

typedef struct
{
    uint32 SampleCount;
    float  Min[MSN_APP_SENSOR_COUNT];
    float  Max[MSN_APP_SENSOR_COUNT];
    float  Avg[MSN_APP_SENSOR_COUNT];
    uint32 ManeuverCount;
    float  TotalDeltaV; /* m/s */
    uint32 DeviceSwitchCount;
    uint32 Uptime; /* MET seconds */
} MSN_APP_AnalysisTlm_Payload_t;

typedef struct
{
    CFE_MSG_TelemetryHeader_t     TelemetryHeader;
    MSN_APP_AnalysisTlm_Payload_t Payload;
} MSN_APP_AnalysisTlm_t;

typedef struct
{
    uint32 FileSize;
    uint32 Offset;
    uint16 Length; /* Valid bytes in Text, 0 at the end of the file */
    uint16 Spare;
    char   Text[MSN_APP_LOG_CHUNK_SIZE];
} MSN_APP_LogTlm_Payload_t;

typedef struct
{
    CFE_MSG_TelemetryHeader_t TelemetryHeader;
    MSN_APP_LogTlm_Payload_t  Payload;
} MSN_APP_LogTlm_t;

#endif /* MSN_APP_MSG_H */
