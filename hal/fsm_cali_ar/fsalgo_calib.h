#ifndef FSALGO_CALIB_H
#define FSALGO_CALIB_H

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdbool.h>
#include <tinyalsa/asoundlib.h>
#include "agmmixer.h"
#include <agm/agm_api.h>
//#include "PalApi.h"
#include "AudioCommon.h"

#ifdef FEATURE_IPQ_OPENWRT
#include <audio_utils/log.h>
#else
#include <log/log.h>
#endif

#if defined(__cplusplus) || defined(c_plusplus)
extern "C" {
#endif

//#include "fsalgo_dsp_intf.h"

#define FSM_CODE_OK      (0)
#define FSM_CODE_FAIL    (-1)
#define FSM_DEV_NUM   (4)
//#define FSM_CKV_MODE_SUPPORT    // Need special CKV mode for F0 Calibration !!!

#define FS_CALI_BIN_PATH     "/mnt/vendor/persist/factory/audio/fsm_calib.bin"   // same as before upgrade
#define FS_CALI_TXT_PATH     "/mnt/vendor/persist/factory/audio/fsalgo_rdc.txt"
#define CALI_RESULT_FILE     "/mnt/vendor/persist/factory/audio/cali_test.bin"

#define FALGO_COEF_RANGE_PATH "/data/fsm_calib_range.config"

#define FSADSP_SET_ALGO_RE25          0x10001FA7
#define FSADSP_GET_ALGO_LIVEDATA      0x10001FA6
#define FSADSP_GET_ALGO_CALIB         0x10001FAB
#define FSADSP_SET_ALGO_RX_EN         0x11111611

//+P86801AA1, zhouweijie.lux, ADD, 2025/08/20, add mmi test
typedef enum {
    CALIB_STATUS_NOTIN=0,
    CALIB_STATUS_ONGING,
    CALIB_STATUS_OK,
    CALIB_STATUS_ERROR,
}audio_calib_type;

struct smartpa_cali_data{
    audio_calib_type calib_status;//0:notin; 1:onging; 2:end;3:end&&error
    double calib_Re[4];// 4 spks
    double calib_F0[4];
};

struct smartpa_cali_data cali_data;

int fs18xx_cali(struct smartpa_cali_data *cali_data);

//-P86801AA1, zhouweijie.lux, ADD, 2025/08/20, add mmi test
struct live_data_info
{
    uint16_t rstrim;
    uint16_t channel;
    uint32_t re25;
    uint32_t f0;
};

struct fsm_algo_live_data
{
    uint16_t version;
    uint16_t ndev;
    struct live_data_info live_data[FSM_DEV_NUM];
};

struct calib_data_info
{
    int32_t re25;
    int32_t tempr;
    int32_t reserve1;
    int32_t f0;
    int32_t q;
    int32_t reserve2;
};

struct fsm_range_data
{
    float r0_min;
    float r0_max;
    float f0_min;
    float f0_max;
};

struct fsm_algo_info {
    uint32_t     miid;
    uint32_t     fe_pcm;
    pthread_t    pthread_id;
    struct mixer *virt_mixer;
    struct mixer *hw_mixer;
    char pcm_device_name[8];
    FILE *fp;
    char be_name[128];
};

struct fsm_dev_info {
	struct mixer *mixer;
	uint32_t miid;
	char *pcm_device_name;
	char *p_intf_name;
};

#define ID_RIFF 0x46464952
#define ID_WAVE 0x45564157
#define ID_FMT  0x20746d66
#define ID_DATA 0x61746164

struct riff_wave_header {
    uint32_t riff_id;
    uint32_t riff_sz;
    uint32_t wave_id;
};

struct chunk_header {
    uint32_t id;
    uint32_t sz;
};

struct chunk_fmt {
    uint16_t audio_format;
    uint16_t num_channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
};

#if defined(__cplusplus) || defined(c_plusplus)
}
#endif

#endif
