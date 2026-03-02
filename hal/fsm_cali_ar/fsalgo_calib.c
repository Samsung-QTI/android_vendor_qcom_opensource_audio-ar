#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>
#include <math.h>
#include <unistd.h>
#include <tinyalsa/asoundlib.h>
#include "fsalgo_calib.h"
#include "fsalgo_dsp_intf.h"

int frsm_set_calib_mode(bool is_set);

#ifdef LOG_TAG
#undef LOG_TAG
#define LOG_TAG  "AHAL:FourSemi:CALIB"
#endif

#define FSM_DEFAULT_RDC  (7*4096)
#define FSM_SCENE_CKV    0xEC000000

#ifndef BACKEND_CONF_FILE
#define BACKEND_CONF_FILE "/vendor/etc/backend_conf.xml"
#endif

// +P86801AA1, zhouweijie.lux, ADD, 2025/08/20, add mmi test
#define MAX_CALI_RE_SPK (8.4)
#define MIN_CALI_RE_SPK (5.6)
#define MAX_CALI_F0_SPK (1000)
#define MIN_CALI_F0_SPK (600)
#define MIN_CALI_RE_REC (4.0)
#define MAX_CALI_RE_REC (8.0)
#define MAX_CALI_F0_REC (750)
#define MIN_CALI_F0_REC (450)
double Re[FSM_DEV_NUM]={0};
double F0[FSM_DEV_NUM]={0};
// -P86801AA1, zhouweijie.lux, ADD, 2025/08/20, add mmi test

static char* channels_desc[] = {"T", "B"};

struct fsm_algo_info g_algo_info;
pthread_t playback_thread;
pthread_t cap_thread;
pthread_t smartpa_cali_thread;

static int capturing = 1;
static int close_pcm = 0;
static int pink_noise = 0;
struct mixer *g_mixer;
static int is_playing = 0;
static pthread_mutex_t g_fsmLock = PTHREAD_MUTEX_INITIALIZER;

int fsm_tmixer_ctl_set(char *control, char *str_value);

/*void sigint_handler(int sig)
{
    signal(sig, SIG_IGN);
    AHAL_INFO("FourSemi Calib  sigint_handler sig:%d", sig);
    capturing = 0;
}*/

/*void stream_close(int sig)
{
    signal(sig, SIG_IGN);
    AHAL_INFO("FourSemi Calib  stream_close sig:%d", sig);
    close_pcm = 1;
}*/

#ifdef FSM_CKV_MODE_SUPPORT
int fsm_set_agm_device_metadata(struct mixer *mixer, char *name, uint32_t key, uint32_t val)
{
    char *control = "metadata";
    char *mixer_str;
    struct mixer_ctl *ctl;
    uint8_t *metadata = NULL;
    struct agm_key_value *gkv = NULL;
    struct agm_key_value *ckv = NULL;
    struct prop_data *prop = NULL;
    uint32_t num_gkv = 1, num_ckv = 1, num_props = 0;
    uint32_t gkv_size, ckv_size, prop_size, index = 0;
    int ctl_len = 0, ret = 0, offset = 0;

    //ret = set_agm_stream_metadata_type(mixer, device, type, stype);
    //if (ret)
        //return ret;
    gkv_size = num_gkv * sizeof(struct agm_key_value);
    ckv_size = num_ckv * sizeof(struct agm_key_value);
    prop_size = sizeof(struct prop_data) + (num_props * sizeof(uint32_t));

    metadata = calloc(1, sizeof(num_gkv) + sizeof(num_ckv) + gkv_size + ckv_size + prop_size);
    if (!metadata)
        return -ENOMEM;

    gkv = calloc(num_gkv, sizeof(struct agm_key_value));
    ckv = calloc(num_ckv, sizeof(struct agm_key_value));
    prop = calloc(1, prop_size);
    if (!gkv || !ckv || !prop) {
        if (ckv)
            free(ckv);
        if (gkv)
            free(gkv);

        free(metadata);
        return -ENOMEM;
    }

    gkv[0].key = DEVICERX;
    gkv[0].value = SPEAKER;
    index = 0;
    ckv[index].key = key;
    ckv[index].value = val;

    prop->prop_id = 0;  //Update prop_id here
    prop->num_values = num_props;

    memcpy(metadata, &num_gkv, sizeof(num_gkv));
    offset += sizeof(num_gkv);
    memcpy(metadata + offset, gkv, gkv_size);
    offset += gkv_size;
    memcpy(metadata + offset, &num_ckv, sizeof(num_ckv));
    offset += sizeof(num_ckv);
    memcpy(metadata + offset, ckv, ckv_size);
    offset += ckv_size;
    memcpy(metadata + offset, prop, prop_size);

    ctl_len = strlen(name) + 4 + strlen(control) + 1;
    mixer_str = calloc(1, ctl_len);
    if (!mixer_str) {
        free(metadata);
        return -ENOMEM;
    }
    snprintf(mixer_str, ctl_len, "%s %s", name, control);
    printf("control name:%s\n", mixer_str);
    ctl = mixer_get_ctl_by_name(mixer, mixer_str);
    if (!ctl) {
        printf("Invalid mixer control: %s\n", mixer_str);
        free(ckv);
        free(gkv);
        free(prop);
        free(metadata);
        free(mixer_str);
        return ENOENT;
    }

    ret = mixer_ctl_set_array(ctl, metadata, sizeof(num_gkv) + sizeof(num_ckv) + gkv_size + ckv_size + prop_size);

    free(gkv);
    free(ckv);
    free(prop);
    free(metadata);
    free(mixer_str);
    return ret;
}
#endif

unsigned int fsm_capture_sample(unsigned int card, unsigned int device,
                            unsigned int dkv, int capture_path, unsigned int channels, unsigned int rate,
                            enum pcm_format format, unsigned int period_size,
                            unsigned int period_count, unsigned int cap_time,
                            struct device_config *dev_config)
{
    struct pcm_config config;
    struct pcm *pcm;
    struct mixer *mixer;
    char *buffer;
    char *intf_name = dev_config->name;
    unsigned int size;
    unsigned int bytes_read = 0;
    unsigned int frames = 0;
    struct timespec end;
    struct timespec now;
    uint32_t miid = 0;
    int ret = 0;

    memset(&config, 0, sizeof(config));
    config.channels = channels;
    config.rate = rate;
    config.period_size = period_size;
    config.period_count = period_count;
    config.format = format;
    config.start_threshold = 0;
    config.stop_threshold = 0;
    config.silence_threshold = 0;

    mixer = mixer_open(card);
    if (!mixer) {
        AHAL_ERR("FourSemi Calib Failed to open mixer\n");
        return 0;
    }

    /* set device/audio_intf media config mixer control */
    if (set_agm_device_media_config(mixer, dev_config->ch, dev_config->rate,
                                    dev_config->bits, intf_name)) {
        AHAL_ERR("FourSemi Calib Failed to set device media config\n");
        goto err_close_mixer;
    }
    AHAL_INFO("FourSemi Calib Capturing set_agm_device_media_config done");

    /* set audio interface metadata mixer control */
    if (set_agm_audio_intf_metadata(mixer, intf_name, dkv, (enum usecase_type)capture_path, dev_config->rate, dev_config->bits, RAW_RECORD)) {
        AHAL_ERR("FourSemi Calib Failed to set device metadata\n");
        goto err_close_mixer;
    }
    AHAL_INFO("FourSemi Calib Capturing set_agm_audio_intf_metadata done");

    /* set stream metadata mixer control */
    if (set_agm_capture_stream_metadata(mixer, device, RAW_RECORD, CAPTURE, STREAM_PCM, 0)) {
        AHAL_ERR("FourSemi Calib Failed to set pcm metadata\n");
        goto err_close_mixer;
    }
    AHAL_INFO("FourSemi Calib Capturing set_agm_capture_stream_metadata done");

    ret = agm_mixer_get_miid (mixer, device, intf_name, STREAM_PCM, TAG_STREAM_MFC, &miid);
    if (ret) {
        printf("MFC not present for this graph\n");
    } else {
        if (configure_mfc(mixer, device, intf_name, TAG_STREAM_MFC,
                     STREAM_PCM, rate, channels, pcm_format_to_bits(format), miid)) {
            AHAL_ERR("FourSemi Calib Failed to configure pspd mfc\n");
            goto err_close_mixer;
        }
    }
    AHAL_INFO("FourSemi Calib Capturing configure_mfc done");

    /* connect pcm stream to audio intf */
    if (connect_agm_audio_intf_to_stream(mixer, device, intf_name, STREAM_PCM, true)) {
        AHAL_ERR("FourSemi Calib Failed to connect pcm to audio interface\n");
        goto err_close_mixer;
    }
    AHAL_INFO("FourSemi Calib Capturing connect_agm_audio_intf_to_stream done");

    pcm = pcm_open(card, device, PCM_IN, &config);
    if (!pcm || !pcm_is_ready(pcm)) {
        AHAL_ERR("FourSemi Calib Unable to open PCM device (%s)\n",
                pcm_get_error(pcm));
        goto err_close_mixer;
    }

    size = pcm_frames_to_bytes(pcm, pcm_get_buffer_size(pcm));
    buffer = (char *)malloc(size);
    if (!buffer) {
        AHAL_ERR("Unable to allocate %u bytes\n", size);
        goto err_close_pcm;
    }

    AHAL_INFO("FourSemi Calib Capturing sample: %u ch, %u hz, %u bit\n", channels, rate,
           pcm_format_to_bits(format));

    if (pcm_start(pcm) < 0) {
        AHAL_ERR("FourSemi Calib start error\n");
        goto err_close_pcm;
    }
    AHAL_INFO("FourSemi Calib Capturing pcm_start done");

    clock_gettime(CLOCK_MONOTONIC, &now);
    end.tv_sec = now.tv_sec + cap_time;
    end.tv_nsec = now.tv_nsec;

    while (capturing && !pcm_read(pcm, buffer, size)) {
        bytes_read += size;
        if (cap_time) {
            clock_gettime(CLOCK_MONOTONIC, &now);
            if (now.tv_sec > end.tv_sec ||
                (now.tv_sec == end.tv_sec && now.tv_nsec >= end.tv_nsec))
                break;
        }
    }

    //capturing = 1;
    frames = pcm_bytes_to_frames(pcm, bytes_read);

    pcm_stop(pcm);
err_close_pcm:
    connect_agm_audio_intf_to_stream(mixer, device, intf_name, STREAM_PCM, false);
    pcm_close(pcm);
    free(buffer);
err_close_mixer:
    mixer_close(mixer);
    return frames;
}
void fsm_play_sample(FILE *file, unsigned int card, unsigned int device, unsigned int device_kv,
                 struct chunk_fmt fmt, struct device_config *dev_config, bool haptics)
{
    struct pcm_config config;
    struct pcm *pcm;
    struct mixer *mixer;
    char *buffer;
    int playback_path, playback_value;
    int size;
    int num_read;
    int ret = 0;
    uint32_t miid = 0;
    char *name = dev_config->name;
    struct group_config grp_config;
    uint32_t dppkv = DEVICEPP_RX_AUDIO_MBDRC;

    memset(&config, 0, sizeof(config));
    config.channels = fmt.num_channels;
    config.rate = fmt.sample_rate;
    config.period_size = 1024;
    config.period_count = 4;
    if (fmt.bits_per_sample == 32)
        config.format = PCM_FORMAT_S32_LE;
    else if (fmt.bits_per_sample == 24)
        config.format = PCM_FORMAT_S24_3LE;
    else if (fmt.bits_per_sample == 16)
        config.format = PCM_FORMAT_S16_LE;
    config.start_threshold = 0;
    config.stop_threshold = 0;
    config.silence_threshold = 0;

    AHAL_INFO("FourSemi Calib Backend %s rate ch bit : %d, %d, %d\n", name,
            dev_config->rate, dev_config->ch, dev_config->bits);
    mixer = mixer_open(card);
    if (!mixer) {
        AHAL_ERR("FourSemi Calib Failed to open mixer\n");
        return;
    }

    /* set device/audio_intf media config mixer control */
    if (set_agm_device_media_config(mixer, dev_config->ch, dev_config->rate,
                                    dev_config->bits, name)) {
        AHAL_ERR("FourSemi Calib Failed to set device media config\n");
        goto err_close_mixer;
    }
    AHAL_INFO("FourSemi Calib set_agm_device_media_config");

    if (haptics) {
        playback_path = HAPTICS;
        playback_value = HAPTICS_PLAYBACK;
    } else {
        playback_path = PLAYBACK;
        playback_value = PCM_LL_PLAYBACK;
    }
     /* set audio interface metadata mixer control */
    if (set_agm_audio_intf_metadata(mixer, name, device_kv, (enum usecase_type)playback_path,
                                    dev_config->rate, dev_config->bits, PCM_LL_PLAYBACK)) {
        AHAL_ERR("FourSemi Calib Failed to set device metadata\n");
        goto err_close_mixer;
    }
    AHAL_INFO("FourSemi Calib set_agm_audio_intf_metadata done");

#ifdef FSM_CKV_MODE_SUPPORT
    fsm_set_agm_device_metadata(mixer, name, FSM_SCENE_CKV, 1);//music
#endif

    /* set audio interface metadata mixer control */
    if (set_agm_stream_metadata(mixer, device, playback_value, PLAYBACK, STREAM_PCM, INSTANCE_1)) {
        AHAL_ERR("FourSemi Calib Failed to set pcm metadata\n");
        goto err_close_mixer;
    }
    AHAL_INFO("FourSemi Calib set_agm_stream_metadata done");

    if (set_agm_streamdevice_metadata(mixer, device, playback_value, PLAYBACK, STREAM_PCM, name,
                                      dppkv)) {
        printf("Failed to set pcm metadata\n");
        goto err_close_mixer;
    }
    /* set audio interface metadata as of now*/
    // if (set_agm_stream_metadata(mixer, device, playback_value, PLAYBACK, STREAM_PCM, 0)) {
        //AHAL_ERR("FourSemi Calib Failed to set pcm metadata\n");
        //goto err_close_mixer;
    //}

    /* Note:  No common metadata as of now*/
    AHAL_INFO("FourSemi Calib set_agm_streamdevice_metadata done");
    /* connect pcm stream to audio intf */
    if (connect_agm_audio_intf_to_stream(mixer, device, name, STREAM_PCM, true)) {
        AHAL_ERR("FourSemi Calib Failed to connect pcm to audio interface\n");
        goto err_close_mixer;
    }
    AHAL_INFO("FourSemi Calib connect_agm_audio_intf_to_stream done");
    ret = agm_mixer_get_miid(mixer, device, name, STREAM_PCM, PER_STREAM_PER_DEVICE_MFC, &miid);
    if (ret) {
        printf("MFC not present for this graph\n");
    } else {
        if (configure_mfc(mixer, device, name, PER_STREAM_PER_DEVICE_MFC,
                       STREAM_PCM, dev_config->rate, dev_config->ch,
                       dev_config->bits, miid)) {
            AHAL_ERR("FourSemi Calib Failed to configure pspd mfc\n");
            goto err_close_mixer;
        }
    }

    if (strstr(name, "VIRT-")) {
        if (get_group_device_info(BACKEND_CONF_FILE, name, &grp_config))
            goto err_close_mixer;

        if (set_agm_group_device_config(mixer, name, &grp_config)) {
            AHAL_ERR("FourSemi Calib Failed to set grp device config\n");
            goto err_close_mixer;
        }
    }

    pcm = pcm_open(card, device, PCM_OUT, &config);
    if (!pcm || !pcm_is_ready(pcm)) {
        AHAL_ERR("FourSemi Calib Unable to open PCM device %u (%s)\n",
                device, pcm_get_error(pcm));
        goto err_close_mixer;
    }

    if (strstr(name, "VIRT-")) {
        if (get_group_device_info(BACKEND_CONF_FILE, name, &grp_config))
            goto err_close_mixer;

        if (set_agm_group_mux_config(mixer, device, &grp_config, name, dev_config->ch)) {
            AHAL_ERR("FourSemi Calib Failed to set grp device config\n");
            goto err_close_mixer;
        }
    }

    size = pcm_frames_to_bytes(pcm, pcm_get_buffer_size(pcm));
    buffer = (char *)malloc(size);
    if (!buffer) {
        AHAL_ERR("FourSemi Calib Unable to allocate %d bytes\n", size);
        goto err_close_pcm;
    }

    AHAL_INFO("Playing sample: %u ch, %u hz, %u bit\n", fmt.num_channels,
            fmt.sample_rate, fmt.bits_per_sample);

    if (pcm_start(pcm) < 0) {
        AHAL_ERR("FourSemi Calib start error\n");
        goto err_close_pcm;
    }

    /* catch ctrl-c to shutdown cleanly */
    // signal(SIGINT, stream_close);
    // signal(SIGALRM, stream_close);

    do {
        num_read = fread(buffer, 1, size, file);
        if (num_read > 0) {
            if (pcm_write(pcm, buffer, num_read)) {
                AHAL_ERR("FourSemi Calib Error playing sample\n");
                break;
            }
        }
    } while (!close_pcm && num_read > 0);

    //close_pcm = 0;
    pcm_stop(pcm);
    /* connect pcm stream to audio intf */
    connect_agm_audio_intf_to_stream(mixer, device, name, STREAM_PCM, false);

err_close_pcm:
    pcm_close(pcm);
    free(buffer);
err_close_mixer:
    mixer_close(mixer);
}

void *cap_func(void *arg) {
    unsigned int card = 100;
    unsigned int device = 101;
    unsigned int channels = FSM_DEV_NUM; //P86801AA1, zhouweijie.lux, ADD, 2025/08/20, add mmi test
    unsigned int rate = 48000;
    unsigned int bits = 32;
    unsigned int frames = 0;
    unsigned int period_size = 1024;
    unsigned int period_count = 4;
    unsigned int cap_time = 8;
    char *intf_name = "TDM-LPAIF_WSA-TX-PRIMARY"; // P86801AA1, zhouweijie.lux, ADD, 2025/08/20, add mmi test
    unsigned int device_kv = VI_TX;
    struct device_config config;
    enum pcm_format format = PCM_FORMAT_S32_LE;
    int capture_path = CAPTURE;
    int ret = 0;

    /*switch (bits) {
    case 32:
        format = PCM_FORMAT_S32_LE;
        break;
    case 24:
        format = PCM_FORMAT_S24_LE;
        break;
    case 16:
        format = PCM_FORMAT_S16_LE;
        break;
    default:
        AHAL_ERR("FourSemi Calib %u bits is not supported.\n", bits);
        return NULL;
    }*/

    //if (intf_name == NULL)
    //    return NULL;

    AHAL_ERR("FourSemi Calib capture enter");
    ret = get_device_media_config(BACKEND_CONF_FILE, intf_name, &config);
    if (ret) {
        AHAL_ERR("FourSemi Calib Invalid input, entry not found for %s\n", intf_name);
        return NULL;
    } else {
        AHAL_DBG("FourSemi Calib TX intf_name: %s", intf_name);
    }

    /* install signal handler and begin capturing */
    //signal(SIGINT, sigint_handler);
    //signal(SIGHUP, sigint_handler);
    //signal(SIGTERM, sigint_handler);
    frames = fsm_capture_sample(card, device, device_kv, capture_path, channels,
                            rate, format,
                            period_size, period_count, cap_time, &config);
    AHAL_INFO("FourSemi Calib capture exit");
    AHAL_INFO("Captured %u frames\n", frames);

    return NULL;
}
void *play_func(void *arg) {
    char filename[128] = "/vendor/etc/spk_cal_silence.wav";
    char filename_pink_noise[128] = "/vendor/etc/spk_cal_pink_noise.wav";
    FILE *file;
    struct riff_wave_header riff_wave_header;
    struct chunk_header chunk_header;
    struct chunk_fmt chunk_fmt;
    unsigned int card = 100, device = 100;
    unsigned int device_kv = 0;
    bool haptics = false;
    char *intf_name = "TDM-LPAIF_WSA-RX-PRIMARY";
    struct device_config config;
    int more_chunks = 1, ret = 0;

    if (pink_noise)
        memcpy(filename, filename_pink_noise, sizeof(filename_pink_noise));

    AHAL_INFO("play file name:%s", filename);
    file = fopen(filename, "rb");
    if (!file) {
        AHAL_ERR("FourSemi Calib Unable to open file '%s'\n", filename);
        return NULL;
    }

    fread(&riff_wave_header, sizeof(riff_wave_header), 1, file);
    if ((riff_wave_header.riff_id != ID_RIFF) ||
        (riff_wave_header.wave_id != ID_WAVE)) {
        AHAL_ERR("FourSemi Calib Error: '%s' is not a riff/wave file\n", filename);
        fclose(file);
        return NULL;
    }

    do {
        fread(&chunk_header, sizeof(chunk_header), 1, file);

        switch (chunk_header.id) {
        case ID_FMT:
            fread(&chunk_fmt, sizeof(chunk_fmt), 1, file);
            /* If the format header is larger, skip the rest */
            if (chunk_header.sz > sizeof(chunk_fmt))
                fseek(file, chunk_header.sz - sizeof(chunk_fmt), SEEK_CUR);
            break;
        case ID_DATA:
            /* Stop looking for chunks */
            more_chunks = 0;
            break;
        default:
            /* Unknown chunk, skip bytes */
            fseek(file, chunk_header.sz, SEEK_CUR);
        }
    } while (more_chunks);

    //if (intf_name == NULL)
    //    return NULL;

    ret = get_device_media_config(BACKEND_CONF_FILE, intf_name, &config);
    if (ret) {
        AHAL_ERR("FourSemi Calib Invalid input, entry not found for : %s\n", intf_name);
        fclose(file);
        return NULL;
    } else {
        AHAL_ERR("FourSemi Calib RX intf_name: %s", intf_name);
    }
    fsm_play_sample(file, card, device, device_kv, chunk_fmt, &config, haptics);

    fclose(file);

    return NULL;

}
int32_t fsm_agm_play_cap()
{
    int ret = 0;

    capturing = 1;
    close_pcm = 0;

    ret = pthread_create(&playback_thread, (const pthread_attr_t *)NULL, play_func, NULL);
    if (ret)
        AHAL_ERR("FourSemi Calib create playback thread failed");
    pthread_detach(playback_thread);

    ret = pthread_create(&cap_thread, (const pthread_attr_t *)NULL, cap_func, NULL);
    if (ret)
        AHAL_ERR("FourSemi Calib create cap thread failed");
    pthread_detach(cap_thread);

    return ret;
}

int32_t fsm_algo_get_data_range(struct fsm_range_data *range)
{
    FILE *fp = NULL;
    int data_len = 4;
    int i, j;

    if (range == NULL){
        AHAL_ERR("FourSemi Calib invalid pointer");
        return FSM_CODE_FAIL;
    }

    fp = fopen(FALGO_COEF_RANGE_PATH, "rb");
    if (fp == NULL) {
        AHAL_ERR("FourSemi Calib open path(%s) failed, use default.", FALGO_COEF_RANGE_PATH);
        range[0].r0_min = 5.6; // 6.5 * (100% - 20%) * 4096
        range[0].r0_max = 8.4; // 6.5 * (100% + 20%) * 4096
        range[1].r0_min = 5.6; // 6.5 * (100% - 20%) * 4096
        range[1].r0_max = 8.4; // 6.5 * (100% + 20%) * 4096
        range[2].r0_min = 5.6; // 6.5 * (100% - 20%) * 4096
        range[2].r0_max = 8.4; // 6.5 * (100% + 20%) * 4096
        range[3].r0_min = 5.6; // 6.5 * (100% - 20%) * 4096
        range[3].r0_max = 8.4; // 6.5 * (100% + 20%) * 4096
    } else {
        for (i = 0; i < FSM_DEV_NUM; i++) {
            j = fscanf(fp, "%f %f %f %f", &range[i].r0_min, &range[i].r0_max, &range[i].f0_min,
                            &range[i].f0_max);
            if (j != data_len) {
                AHAL_ERR("FourSemi Calib get coef range failed");
                fclose(fp);
                return FSM_CODE_FAIL;
            }
            AHAL_INFO("FourSemi Calib dev[%d] range re25[%f-%f] f0[%f-%f] ", i, range[i].r0_min,
                  range[i].r0_max, range[i].f0_min, range[i].f0_max);
        }
        fclose(fp);
    }

    return FSM_CODE_OK;
}

int fsm_ascii_fwrite(struct calib_data_info *live_data, size_t chs, char *file)
{
    FILE *fp = NULL;
    //int len = 0;

    fp = fopen(file, "w");
    if (fp == NULL) {
        AHAL_ERR("FourSemi Calib open %s spk stored data failed", file);
        return FSM_CODE_FAIL;
    }

    for (int i = 0; i < chs; i++) {
        fprintf(fp, "CH[%d] %s %d (%f)\n", i, channels_desc[i],
            live_data[i].re25, live_data[i].re25 / 4096.0);
    }
    fclose(fp);

    return 0;
}

int fsm_default_fwrite(struct calib_data_info *live_data, size_t chs, const char *file)
{
    FILE *fp = NULL;
    int len = 0;

    fp = fopen(file, "wb+");
    if (!fp) {
        AHAL_ERR("FourSemi Calib open %s spk stored data failed", file);
        return FSM_CODE_FAIL;
    }

    for (int i = 0; i < chs; i++) {
        len += fwrite(&live_data[i].re25, sizeof(live_data[i].re25), 1, fp);
        AHAL_DBG("FourSemi Calib write file:%s r0[%d]:%f len:%d", file, i, live_data[i].re25 / 4096.0, len);
    }
    fclose(fp);

    return (len == FSM_DEV_NUM) ? 0 : -1; //P86801AA1, zhouweijie.lux, ADD, 2025/08/20, add mmi test
}

int fsm_mixer_dac_switch(bool unmute) {
    int i, len, ret = 0;
    char mixer_name[64];

    for (i = 0; i < FSM_DEV_NUM; i++) {
        memset(mixer_name, '\0', sizeof(mixer_name));
        len = snprintf(mixer_name, sizeof(mixer_name), "SPK%d FSAMP DAC_Port Switch", i + 1);
        if (len < 0 || len >= sizeof(mixer_name)) {
            ret |= FSM_CODE_FAIL;
            break;
        }
        ret |= fsm_tmixer_ctl_set(mixer_name, unmute ? "1" : "0");
        if (ret != 0) {
            AHAL_ERR("set mixer %s failed\n", mixer_name);
            break;
        }
    }
    return ret;
}

int fsm_algo_force_calib(char *rx_intf_name, double *calib_result, int calib_mode) {
    struct calib_data_info live_data[FSM_DEV_NUM];
    struct fsm_algo_info *info = &g_algo_info;
    struct fsm_range_data spk_range[FSM_DEV_NUM];
    struct mixer *virt_mixer = NULL;
    int pcm_min = 100, pcm_max = 150;
    //char *rx_intf_name = "MI2S-LPAIF_VA-RX-PRIMARY";
    int card = 100;
    int device = 0;
    uint32_t miid = 0;
    int ret, i;
    double r0, f0;

    if (calib_result == NULL) {
        AHAL_ERR("FourSemi Calib pointer is null");
        return FSM_CODE_FAIL;
    }

    if (calib_mode)
        pink_noise = 1;
    else
        pink_noise = 0; //P86801AA1, zhouweijie.lux, ADD, 2025/08/20, add mmi test

    AHAL_DBG("FourSemi Calib process enter");
    //fix miid
    pthread_mutex_lock(&g_fsmLock);
    fsm_algo_get_data_range(spk_range);
    virt_mixer = mixer_open(card);
    if (!virt_mixer) {
        AHAL_ERR("FourSemi Calib Failed to open mixer\n");
        pthread_mutex_unlock(&g_fsmLock);
        return FSM_CODE_FAIL;
    }
    info->virt_mixer = virt_mixer;

    ret = fsm_mixer_dac_switch(true);
    if (ret != 0) {
        AHAL_ERR("FourSemi Calib Failed to set mixer dac switch\n");
        mixer_close(virt_mixer);
        pthread_mutex_unlock(&g_fsmLock);
        return FSM_CODE_FAIL;
    }

    fsm_agm_play_cap();

    if (calib_mode)
        usleep(6000 * 1000); // 7s->6s
    else {
        ret = frsm_set_calib_mode(true);
        if (ret != 0) {
            AHAL_ERR("Failed to enable PA calib mode");
            goto exit_mode;
        }
        usleep(4000 * 1000);
    }

    AHAL_ERR("FourSemi Calib get r0");
    for (device = pcm_min; device <= pcm_max; device++) {
        miid = 0;
        ret = agm_mixer_get_miid(virt_mixer, device, rx_intf_name, STREAM_PCM, MODULE_SP, &miid);
        AHAL_INFO("FourSemi Calib get miid[%d]:%x", device, miid);
        if (ret || miid == 0) {
            AHAL_INFO("get miid failed with device %d\n", device);
            continue;
        }
        if (device == 150) {
            AHAL_ERR("FourSemi Calib get miid failed to max num\n");
            break;
        }
        info->miid = miid;
        sprintf(info->pcm_device_name, "PCM%d", device);
        AHAL_INFO("FourSemi Calib module sp miid:%x, device name:%s", miid, info->pcm_device_name);
        ret = fsm_get_payload_from_dsp((void *)info, FSADSP_GET_ALGO_CALIB, (char *)live_data, sizeof(struct calib_data_info) * FSM_DEV_NUM);
        if (ret < 0) {
            AHAL_ERR("FourSemi Calib get livedata info failed");
            continue;
        } else {
            if (calib_mode == 1) {
                if ((live_data[0].f0 == 0) && (live_data[1].f0 == 0) && (live_data[2].f0 == 0) && (live_data[3].f0 == 0))
                    continue;
            } else {
                if ((live_data[0].re25== 0) && (live_data[1].re25 == 0) && (live_data[2].re25== 0) && (live_data[3].re25 == 0))
                    continue;
            }
            break;
        }
    }

    for (i = 0; i < FSM_DEV_NUM; i++) {
        AHAL_DBG("FourSemi Calib livedata re25[%d] = %d", i, live_data[i].re25);
        AHAL_DBG("FourSemi Calib livedata tempr[%d] = %d", i, live_data[i].tempr);
        AHAL_DBG("FourSemi Calib livedata f0[%d] = %d", i, live_data[i].f0);
        AHAL_DBG("FourSemi Calib livedata q[%d] = %d", i, live_data[i].q);
        r0 = live_data[i].re25 / 4096.0;
        f0 = live_data[i].f0 / 256.0;
        if (calib_mode) {
            calib_result[i] = f0;
        } else {
            if (r0 < spk_range[i].r0_max && r0 > spk_range[i].r0_min) {
                calib_result[i] = r0;
            } else {
                calib_result[i] = r0;
                AHAL_DBG("FourSemi Calib invalid r0:%f", r0);
                //goto exit;
            }
        }
        AHAL_DBG("FourSemi Calib r0:%f f0:%f", r0, f0);
    }
    if (calib_mode == 0) {
        ret = fsm_default_fwrite(live_data, FSM_DEV_NUM, FS_CALI_BIN_PATH);
        if (ret) {
            AHAL_DBG("FourSemi Calib store rdc failed");
            memset((void *)calib_result, 0, sizeof(double) * FSM_DEV_NUM);
        }
    }
    //fsm_ascii_fwrite(live_data, FSM_DEV_CH_MAX, FS_CALI_TXT_PATH);

    AHAL_DBG("FourSemi Calib process end");

exit_mode:
    if (!calib_mode)
        frsm_set_calib_mode(false);

exit:
    capturing = 0;
    close_pcm = 1;
    fsm_mixer_dac_switch(false);
    mixer_close(virt_mixer);
    pthread_mutex_unlock(&g_fsmLock);

    return FSM_CODE_OK;
}
#if 0
int fsm_algo_set_re25() {
    struct calib_data_info send_data[FSM_DEV_NUM];
    struct fsm_algo_info *info = &g_algo_info;
    FILE *fp = NULL;
    int ret = 0;
    //int i;

    //send payload to dsp
    AHAL_DBG("FourSemi Calib read re25 from file");
    fp = fopen(FS_CALI_BIN_PATH, "rb");
    if (fp) {
        for (int i = 0; i < FSM_DEV_NUM; i++) {
            fread(&send_data[i].re25, sizeof(send_data[i].re25), 1, fp);
            //fread(&send_data[i].f0, sizeof(send_data[i].f0), 1, fp);
            AHAL_INFO("FourSemi Calib dev[%d] r0:%d f0:%d", i, send_data[i].re25 / 4096, send_data[i].f0);
        }
        fclose(fp);
    } else {
        AHAL_ERR("FourSemi Calib Speaker not calibrated. set a safe volume");
        for (int i = 0; i < FSM_DEV_NUM; i++) {
            //TODO: safe value
            send_data[i].re25 = FSM_DEFAULT_RDC;
        }
    }
    pthread_mutex_lock(&g_fsmLock);
    ret = fsm_send_payload_to_dsp(info, FSADSP_SET_ALGO_RE25, (char *)send_data, sizeof(struct calib_data_info) * FSM_DEV_NUM);
    if (ret < 0) {
        AHAL_ERR("FourSemi Calibcd send re25 to dsp failed");
        goto exit;
    }
    AHAL_INFO("FourSemi Calib send re25 to algo success");

exit:
    pthread_mutex_unlock(&g_fsmLock);
    if (fp)
        fclose(fp);

    return ret;
}

int fsm_algo_set_dsp_en(struct fsm_dev_info *info, uint32_t is_enable) {
    struct fsm_algo_info *algo_info = &g_algo_info;
    int ret = 0;

    if (info == NULL) {
        AHAL_ERR("fsm_dev_info is null!");
        return FSM_CODE_FAIL;
    }

    pthread_mutex_lock(&g_fsmLock);
    algo_info->miid = info->miid;
    algo_info->virt_mixer = info->mixer;
    algo_info->pcm_device_name = info->pcm_device_name;

    ret = fsm_send_payload_to_dsp(algo_info, FSADSP_SET_ALGO_RX_EN, (char *)&is_enable, sizeof(is_enable));
    if (ret < 0) {
        AHAL_ERR("FourSemi set dsp %s failed!", (is_enable ? "enable" : "disable"));
        goto exit;
    }
    AHAL_INFO("FourSemi set dsp %s", (is_enable ? "enable" : "disable"));

exit:
    pthread_mutex_unlock(&g_fsmLock);
    return ret;
}
#endif
/* +P86801AA1-1797, zhouweijie.lux, 2025.08.26, add for smartpa cail */
int checkCalibValue()
{
    if(!((Re[0] < MAX_CALI_RE_SPK) && (Re[0] > MIN_CALI_RE_SPK)
                                && (F0[0] < MAX_CALI_F0_SPK) && (F0[0] > MIN_CALI_F0_SPK)))
        return 0;

    if(!((Re[1] < MAX_CALI_RE_SPK) && (Re[1] > MIN_CALI_RE_SPK)
                                && (F0[1] < MAX_CALI_F0_SPK) && (F0[1] > MIN_CALI_F0_SPK)))
        return 0;

    if(!((Re[2] < MAX_CALI_RE_SPK) && (Re[2] > MIN_CALI_RE_SPK)
                                && (F0[2] < MAX_CALI_F0_SPK) && (F0[2] > MIN_CALI_F0_SPK)))
        return 0;

    if(!((Re[3] < MAX_CALI_RE_SPK) && (Re[3] > MIN_CALI_RE_SPK)
                                && (F0[3] < MAX_CALI_F0_SPK) && (F0[3] > MIN_CALI_F0_SPK)))
        return 0;
    return 1;
}
/* +P86801AA1-1797, zhouweijie.lux, 2025.08.26, add for smartpa cail */
void is_playing_status(int play_status) {
    is_playing = play_status;
}

static int cali_result_write_re_f0()
{
    FILE *pFile = NULL;
    int cnt = 0;
    char str_tmp[256] = {0};

    pFile = fopen(CALI_RESULT_FILE, "wb");
    if(pFile == NULL){
        AHAL_ERR("open %s fail\n", CALI_RESULT_FILE);
        return -1;
    }

    sprintf(str_tmp, "Re1=%1.2f,Re2=%1.2f,Re3=%1.2f,Re4=%1.2f,F0_1=%1.0f,F0_2=%1.0f,F0_3=%1.0f,F0_4=%1.0f",
        Re[0], Re[1], Re[2], Re[3],
        F0[0], F0[1], F0[2], F0[3]);
    AHAL_INFO("write re:%s to bin file", str_tmp);
    cnt = fwrite(str_tmp, 1, sizeof(str_tmp), pFile);
    if(cnt < sizeof(str_tmp)){
        AHAL_ERR("write start_cali fail: %d", cnt);
    }

    fclose(pFile);
    return 0;
}
/* -P86801AA1-1797, zhouweijie.lux, 2025.08.26, add for smartpa cail */
static void* do_spkcal(void *arg)
{
    struct smartpa_cali_data *cali_data = arg;
    int i = 0;

    AHAL_INFO("do_spkcal start");
    // init start
    for (i = 0; i < FSM_DEV_NUM; i++) {
        cali_data->calib_Re[i] = 0;
        cali_data->calib_F0[i] = 0;
        Re[i] = 0;
        F0[i] = 0;
    }
    cali_data->calib_status = CALIB_STATUS_ONGING;
    // init end
/* +P86801AA1-1797, zhouweijie.lux, 2025.08.26, add for smartpa cail */
    if (is_playing == 1) {
        AHAL_DBG("RX is playing: %d, waiting.", is_playing);
        usleep(2500*1000);
    }
/* -P86801AA1-1797, zhouweijie.lux, 2025.08.26, add for smartpa cail */
    AHAL_INFO("FourSemi Calib Get R0:");
    fsm_algo_force_calib("TDM-LPAIF_WSA-RX-PRIMARY", Re, 0);
    AHAL_DBG("FourSemi Calib r0:%f r1:%f r2:%f r3:%f", Re[0], Re[1], Re[2], Re[3]);
    cali_data->calib_Re[0] = Re[0];		// 0:top left --> ATO:top right
    cali_data->calib_Re[1] = Re[1];		// 1:btm left --> ATO:top left
    cali_data->calib_Re[2] = Re[2];		// 2:top right --> ATO:btm right
    cali_data->calib_Re[3] = Re[3];		// 3:btm right --> ATO:btm left

    usleep(500*1000);

    AHAL_INFO("FourSemi Calib Get F0:");
    fsm_algo_force_calib("TDM-LPAIF_WSA-RX-PRIMARY", F0, 1);
    AHAL_DBG("FourSemi Calib f0:%f f1:%f f2:%f f3:%f", F0[0], F0[1], F0[2], F0[3]);
    cali_data->calib_F0[0] = F0[0];
    cali_data->calib_F0[1] = F0[1];
    cali_data->calib_F0[2] = F0[2];
    cali_data->calib_F0[3] = F0[3];

    if (checkCalibValue() == 1)
    {
        cali_data->calib_status = CALIB_STATUS_OK;
    }
    else
    {
        AHAL_INFO("calib fail");
        cali_data->calib_status =CALIB_STATUS_ERROR;
    }

    cali_result_write_re_f0();

    AHAL_INFO("end calib_status:%d", cali_data->calib_status);
    pthread_exit(0);
}

int fs18xx_cali(struct smartpa_cali_data *cali_data) {
    AHAL_INFO("start");
    int ret = -1;

    ret = pthread_create(&smartpa_cali_thread,  (const pthread_attr_t *) NULL, do_spkcal, cali_data);
    if(ret) {
        AHAL_ERR("pthread_create fail do_spkcal %d", ret);
    }

    pthread_detach(smartpa_cali_thread);

    return ret;
}
/* -P86801AA1-1797, zhouweijie.lux, 2025.08.26, add for smartpa cail */
#if 0
int main(int argc, char** argv) {
    float data[4] = {0};
    int f0_test = 0, r0_test = 0;
    char intf_name[64] = {0};

    AHAL_INFO("FourSemi Calib enter argc:%d, argv=%s", argc, *argv);

    argv += 2;
    while (*argv) {
        if (strcmp(*argv, "f0") == 0)
            f0_test = 1;

        if (strcmp(*argv, "rdc") == 0)
            r0_test = 1;

        if (strcmp(*argv, "-rx") == 0) {
            argv++;
            if (*argv) {
                strcpy(intf_name, *argv);
                break;
            }
        }
        argv++;
    }

    if (f0_test) {
        AHAL_INFO("FourSemi Calib Get F0:");
        fsm_algo_force_calib(intf_name, data, 1);
        fprintf(stdout, "f0[0]=%f f0[1]=%f f0[2]=%f f0[3]=%f", data[0], data[1], data[2], data[3]);
    }

    if (r0_test) {
        AHAL_INFO("FourSemi Calib Get R0:");
        fsm_algo_force_calib(intf_name, data, 0);
        fprintf(stdout, "rdc[0]=%f rdc[1]=%f rdc[2]=%f rdc[3]=%f", data[0], data[1], data[2], data[3]);
    }

    return 0;
}
#endif
