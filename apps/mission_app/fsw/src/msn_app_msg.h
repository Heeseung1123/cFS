/* MSN_APP command and telemetry message definitions */
#ifndef MSN_APP_MSG_H
#define MSN_APP_MSG_H

/*
** Command codes
*/
#define MSN_APP_NOOP_CC           0
#define MSN_APP_RESET_COUNTERS_CC 1
#define MSN_APP_SET_PARAM_CC      2
#define MSN_APP_SET_STARTUP_CC    3
#define MSN_APP_RUN_DIAG_CC       4
#define MSN_APP_GET_APP_STATUS_CC 5
#define MSN_APP_READ_TEMP_CC      6

/* Parameter IDs for SET_PARAM, also the index in the HK Param array */
#define MSN_APP_PARAM_TEMP_HIGH   0 /* deg C */
#define MSN_APP_PARAM_TEMP_LOW    1 /* deg C */
#define MSN_APP_PARAM_TEMP_PERIOD 2 /* send TEMP tlm every N HK requests, 0 = only on READ_TEMP */
#define MSN_APP_PARAM_COUNT       3

/* Temperature status in TEMP and HK telemetry */
#define MSN_APP_TEMP_NORMAL  0
#define MSN_APP_TEMP_HIGH    1
#define MSN_APP_TEMP_LOW     2
#define MSN_APP_TEMP_NO_DATA 3

#define MSN_APP_DIAG_CHECK_COUNT 5 /* see MSN_APP_RunDiag() */

/*
** Command packets
*/
typedef struct
{
    CFE_MSG_CommandHeader_t CmdHeader;
} MSN_APP_NoArgsCmd_t;

typedef MSN_APP_NoArgsCmd_t MSN_APP_NoopCmd_t;
typedef MSN_APP_NoArgsCmd_t MSN_APP_ResetCountersCmd_t;
typedef MSN_APP_NoArgsCmd_t MSN_APP_RunDiagCmd_t;
typedef MSN_APP_NoArgsCmd_t MSN_APP_ReadTempCmd_t;

typedef struct
{
    uint8 ParamId;
    uint8 Spare[3];
    int32 Value;
} MSN_APP_SetParam_Payload_t;

typedef struct
{
    CFE_MSG_CommandHeader_t    CmdHeader;
    MSN_APP_SetParam_Payload_t Payload;
} MSN_APP_SetParamCmd_t;

typedef struct
{
    char  AppName[CFE_MISSION_MAX_API_LEN]; /* cFE name, 4th field of the startup script line */
    uint8 Enable;                           /* 0 = do not start at next boot, 1 = start */
    uint8 Spare[3];
} MSN_APP_SetStartup_Payload_t;

typedef struct
{
    CFE_MSG_CommandHeader_t      CmdHeader;
    MSN_APP_SetStartup_Payload_t Payload;
} MSN_APP_SetStartupCmd_t;

typedef struct
{
    char AppName[CFE_MISSION_MAX_API_LEN];
} MSN_APP_GetAppStatus_Payload_t;

typedef struct
{
    CFE_MSG_CommandHeader_t        CmdHeader;
    MSN_APP_GetAppStatus_Payload_t Payload;
} MSN_APP_GetAppStatusCmd_t;

/*
** Telemetry packets
*/
typedef struct
{
    uint8 CommandCounter;
    uint8 CommandErrorCounter;
    uint8 TempStatus;
    uint8 Spare;
    float Temperature; /* last value read, deg C */
    int32 Param[MSN_APP_PARAM_COUNT];
} MSN_APP_HkTlm_Payload_t;

typedef struct
{
    CFE_MSG_TelemetryHeader_t TelemetryHeader;
    MSN_APP_HkTlm_Payload_t   Payload;
} MSN_APP_HkTlm_t;

typedef struct
{
    float  Temperature;
    uint8  Status;
    uint8  Spare[3];
    uint32 SampleCount;
} MSN_APP_TempTlm_Payload_t;

typedef struct
{
    CFE_MSG_TelemetryHeader_t TelemetryHeader;
    MSN_APP_TempTlm_Payload_t Payload;
} MSN_APP_TempTlm_t;

typedef struct
{
    uint8  Result[MSN_APP_DIAG_CHECK_COUNT]; /* 0 = pass, 1 = fail */
    uint8  FailCount;
    uint8  Spare[2];
    uint32 RunCount;
} MSN_APP_DiagTlm_Payload_t;

typedef struct
{
    CFE_MSG_TelemetryHeader_t TelemetryHeader;
    MSN_APP_DiagTlm_Payload_t Payload;
} MSN_APP_DiagTlm_t;

typedef struct
{
    char   AppName[CFE_MISSION_MAX_API_LEN];
    uint8  Running; /* 1 if ES knows the app now */
    uint8  Startup; /* 0 = not in the startup script, 1 = enabled, 2 = disabled */
    uint8  Spare[2];
    uint32 Priority;
    uint32 StackSize;
    uint32 ExecutionCounter;
    uint32 ChildTasks;
} MSN_APP_AppStatusTlm_Payload_t;

typedef struct
{
    CFE_MSG_TelemetryHeader_t      TelemetryHeader;
    MSN_APP_AppStatusTlm_Payload_t Payload;
} MSN_APP_AppStatusTlm_t;

#endif /* MSN_APP_MSG_H */
