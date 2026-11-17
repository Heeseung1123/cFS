/* RELAY_APP: forwards an on-board file to another ground station through FILE_DL */
#ifndef RELAY_APP_H
#define RELAY_APP_H

#include "cfe.h"

#include "relay_app_perfids.h"
#include "relay_app_msgids.h"
#include "relay_app_msg.h"
#include "file_dl_my.h" /* FILE_DL message IDs, START command and segment header */

#define RELAY_APP_PIPE_DEPTH 32

/* IDLE -> FORWARD -> WAITING (for FILE_DL's START segment) -> FORWARDING -> STOP -> IDLE */
#define RELAY_APP_STATE_IDLE       0
#define RELAY_APP_STATE_WAITING    1
#define RELAY_APP_STATE_FORWARDING 2

typedef struct
{
    uint8 CmdCounter;
    uint8 ErrCounter;

    /* Current relay: file, destination and progress */
    uint8         State;
    uint8         StationId;
    char          Filename[RELAY_APP_MAX_NAME];
    OS_SockAddr_t StationAddr;
    osal_id_t     Socket;
    uint32        SegmentsForwarded;
    uint32        BytesForwarded;

    RELAY_APP_HkTlm_t  HkTlm;
    FILE_DL_StartCmd_t FileDlStartCmd; /* Sent to FILE_DL on the software bus */

    uint32          RunStatus;
    CFE_SB_PipeId_t CommandPipe;
} RELAY_APP_Data_t;

void  RELAY_APP_Main(void);
int32 RELAY_APP_Init(void);
void  RELAY_APP_ProcessCommandPacket(CFE_SB_Buffer_t *SBBufPtr);
void  RELAY_APP_ProcessGroundCommand(CFE_SB_Buffer_t *SBBufPtr);
int32 RELAY_APP_ReportHousekeeping(const CFE_MSG_CommandHeader_t *Msg);
bool  RELAY_APP_VerifyCmdLength(CFE_MSG_Message_t *MsgPtr, size_t ExpectedLength);

int32 RELAY_APP_Forward(const RELAY_APP_ForwardCmd_t *Msg);
int32 RELAY_APP_Stop(const RELAY_APP_StopCmd_t *Msg);
void  RELAY_APP_ForwardSegment(const CFE_SB_Buffer_t *SBBufPtr);

#endif /* RELAY_APP_H */
