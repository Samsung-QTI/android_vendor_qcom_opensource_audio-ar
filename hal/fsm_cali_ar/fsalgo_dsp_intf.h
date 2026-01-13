#ifndef FSALGO_DSP_INTF_H_
#define FSALGO_DSP_INTF_H_
#include "fsalgo_calib.h"
#if defined(__cplusplus) || defined(c_plusplus)
extern "C" {
#endif

#ifdef FEATURE_IPQ_OPENWRT
#include <audio_utils/log.h>
#else
#include <log/log.h>
#endif

#define FSM_ERR  (-1)
#define FSM_OK   (0)

int fsm_send_payload_to_dsp(struct fsm_algo_info *info, uint32_t param_id, char *data, unsigned int data_size);
int fsm_get_payload_from_dsp(struct fsm_algo_info *info, uint32_t param_id, char * data, unsigned int data_size);

#if defined(__cplusplus) || defined(c_plusplus)
}
#endif
#endif