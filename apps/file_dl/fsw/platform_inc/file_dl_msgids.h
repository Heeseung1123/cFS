/* Message IDs of the file_dl app: downlink, upload and relay each have their own
 * command MID and telemetry MIDs. */
#ifndef _file_dl_msgids_h_
#define _file_dl_msgids_h_

/* Commands */
#define FILE_DL_CMD_MID    0x18C0 /* downlink: NOOP, START, RESEND, DONE */
#define FILE_DL_WAKEUP_MID 0x18C1 /* optional: one downlink tick (sch_lab no longer sends it) */
#define FILE_UL_CMD_MID    0x18C2 /* upload: NOOP, START, DATA, END, STATUS, CANCEL */
#define FILE_RELAY_CMD_MID 0x18C3 /* relay: same codes as upload, then sent down */

/* Telemetry */
#define FILE_DL_SEG_TLM_MID       0x08C0 /* downlink START, DATA and END segments */
// #define FILE_DL_UL_STATUS_TLM_MID 0x08C1 /* upload state, missing ranges, verify result */
#define FILE_UL_STATUS_TLM_MID    0x08C1 /* upload state, missing ranges, verify result */
#define FILE_RELAY_STATUS_TLM_MID 0x08C2 /* relay upload state, missing ranges, verify result */
#define FILE_RELAY_SEG_TLM_MID    0x08C3 /* relay downlink segments (same format as 0x08C0) */

#endif /* _file_dl_msgids_h_ */
