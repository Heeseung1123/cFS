/* Message IDs of the byte-offset file downlink prototype (file_dl). */
#ifndef _file_dl_msgids_h_
#define _file_dl_msgids_h_

#define FILE_DL_CMD_MID      0x18C0 /* NOOP and START (file name) */
#define FILE_DL_WAKEUP_MID   0x18C1 /* sch_lab, 1 Hz: send the next segments */
#define FILE_DL_SEG_TLM_MID  0x08C0 /* START, DATA and END segments */
#define FILE_DL_UL_STATUS_TLM_MID 0x08C1 /* upload state, missing ranges, verify result */

#endif /* _file_dl_msgids_h_ */
