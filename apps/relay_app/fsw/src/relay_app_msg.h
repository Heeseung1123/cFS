/* RELAY_APP command and telemetry message definitions */
#ifndef RELAY_APP_MSG_H
#define RELAY_APP_MSG_H

/*
** Command codes
*/
#define RELAY_APP_NOOP_CC           0
#define RELAY_APP_RESET_COUNTERS_CC 1
#define RELAY_APP_FORWARD_CC        2
#define RELAY_APP_STOP_CC           3

#define RELAY_APP_MAX_NAME 64 /* Same as FILE_DL_MAX_NAME */

/*
** Command packets
*/
typedef struct
{
    CFE_MSG_CommandHeader_t CmdHeader;
} RELAY_APP_NoArgsCmd_t;

typedef RELAY_APP_NoArgsCmd_t RELAY_APP_NoopCmd_t;
typedef RELAY_APP_NoArgsCmd_t RELAY_APP_ResetCountersCmd_t;
typedef RELAY_APP_NoArgsCmd_t RELAY_APP_StopCmd_t;

typedef struct
{
    char  Filename[RELAY_APP_MAX_NAME]; /* File already on board, e.g. "/cf/inbox/img.bin" */
    uint8 StationId;                    /* Index in the station list of relay_app.c */
    uint8 Spare[3];
} RELAY_APP_Forward_Payload_t;

typedef struct
{
    CFE_MSG_CommandHeader_t     CmdHeader;
    RELAY_APP_Forward_Payload_t Payload;
} RELAY_APP_ForwardCmd_t;

/*
** Telemetry packets
*/
typedef struct
{
    uint8  CommandCounter;
    uint8  CommandErrorCounter;
    uint8  State;     /* RELAY_APP_STATE_* */
    uint8  StationId;
    uint32 SegmentsForwarded;
    uint32 BytesForwarded;
} RELAY_APP_HkTlm_Payload_t;

typedef struct
{
    CFE_MSG_TelemetryHeader_t TelemetryHeader;
    RELAY_APP_HkTlm_Payload_t Payload;
} RELAY_APP_HkTlm_t;

#endif /* RELAY_APP_MSG_H */
