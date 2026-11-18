/* MSN_APP: satellite control app for parameters, startup apps, diagnostics, app status and temperature */
#ifndef MSN_APP_H
#define MSN_APP_H

#include "cfe.h"

#include "msn_app_perfids.h"
#include "msn_app_msgids.h"
#include "msn_app_msg.h"

#define MSN_APP_PIPE_DEPTH 32

#define MSN_APP_CMD_REJECTED_ERR_CODE -1

/* The temperature file stands in for a sensor: edit it by hand to change the reading */
#define MSN_APP_TEMP_FILE    "/cf/msn_temp.txt"
#define MSN_APP_PARAM_FILE   "/cf/msn_param.txt"
#define MSN_APP_STARTUP_FILE "/cf/cfe_es_startup.scr"
#define MSN_APP_STARTUP_TEMP "/cf/cfe_es_startup.tmp"

#define MSN_APP_STARTUP_MAX_SIZE 4096

/* Where an app is in the startup script (AppStatus Startup field) */
#define MSN_APP_STARTUP_NOT_FOUND 0
#define MSN_APP_STARTUP_ENABLED   1 /* before the first '!' line, started by ES */
#define MSN_APP_STARTUP_DISABLED  2 /* after the first '!' line, ignored by ES */

typedef struct
{
    uint8 CmdCounter;
    uint8 ErrCounter;

    int32  Param[MSN_APP_PARAM_COUNT]; /* kept in MSN_APP_PARAM_FILE */
    float  Temperature;
    uint8  TempStatus;
    uint32 TempSampleCount;
    uint32 HkCount;
    uint32 DiagRunCount;

    /* Startup script read by MSN_APP_FindStartupEntry() */
    char  StartupBuf[MSN_APP_STARTUP_MAX_SIZE];
    int32 StartupLength;

    MSN_APP_HkTlm_t        HkTlm;
    MSN_APP_TempTlm_t      TempTlm;
    MSN_APP_DiagTlm_t      DiagTlm;
    MSN_APP_AppStatusTlm_t AppStatusTlm;

    uint32          RunStatus;
    CFE_SB_PipeId_t CommandPipe;
} MSN_APP_Data_t;

void  MSN_APP_Main(void);
int32 MSN_APP_Init(void);
void  MSN_APP_ProcessCommandPacket(CFE_SB_Buffer_t *SBBufPtr);
void  MSN_APP_ProcessGroundCommand(CFE_SB_Buffer_t *SBBufPtr);
int32 MSN_APP_ReportHousekeeping(const CFE_MSG_CommandHeader_t *Msg);
bool  MSN_APP_VerifyCmdLength(CFE_MSG_Message_t *MsgPtr, size_t ExpectedLength);

int32 MSN_APP_SetParam(const MSN_APP_SetParamCmd_t *Msg);
int32 MSN_APP_SetStartup(const MSN_APP_SetStartupCmd_t *Msg);
int32 MSN_APP_RunDiag(const MSN_APP_RunDiagCmd_t *Msg);
int32 MSN_APP_GetAppStatus(const MSN_APP_GetAppStatusCmd_t *Msg);
int32 MSN_APP_SendTemperature(void);

int32 MSN_APP_ReadTemperature(void);
bool  MSN_APP_ReadParamFile(int32 *Param);
void  MSN_APP_SaveParams(void);
int32 MSN_APP_FindStartupEntry(const char *AppName, char **Entry, char **Marker);

#endif /* MSN_APP_H */
