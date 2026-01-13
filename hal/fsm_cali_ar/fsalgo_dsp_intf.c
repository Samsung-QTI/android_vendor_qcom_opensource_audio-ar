#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <tinyalsa/asoundlib.h>
#include "fsalgo_dsp_intf.h"
//#include "AudioCommon.h"
#include <agm/agm_api.h>

#ifdef LOG_TAG
#undef LOG_TAG
#define LOG_TAG  "AHAL:FourSemi:INTF"
#endif

#define FSM_ERR  (-1)
#define FSM_OK   (0)
#define FS_ALIGN_4BYTE(x)         (((x) + 3) & (~3))
#define FS_ALIGN_8BYTE(x)         (((x) + 7) & (~7))
#define FS_PADDING_ALIGN_8BYTE(x) ((((x) + 7) & 7) ^ 7)

#define FSM_CTL_CONTROL           ("control")
#define FSM_CTL_GET_PARAM         ("getParam")
#define FSM_CTL_SET_PARAM         ("setParam")
#define FSM_CTL_GET_TAGGED_INFO   ("getTaggedInfo")

#if 0

struct gsl_module_id_info_entry {
    uint32_t module_id; /**< module id */
    uint32_t module_iid; /**< globally unique module instance id */
};

/**
 * Structure mapping the tag_id to module info (mid and miid)
 */

struct gsl_tag_module_info_entry {
    uint32_t tag_id; /**< tag id of the module */
    uint32_t num_modules; /**< number of modules matching the tag_id */
    struct gsl_module_id_info_entry module_entry[0]; /**< module list */
};

struct gsl_tag_module_info {
    uint32_t num_tags; /**< number of tags */
    struct gsl_tag_module_info_entry tag_module_entry[0];
    /**< variable payload of type struct gsl_tag_module_info_entry */
};
#endif
struct fs_module_param_data_t
{
    uint32_t miid;
    uint32_t param_id;
    uint32_t param_size;//must > 0, in multiple of 4
    uint32_t error_code;// used only in out-of-band mode
};

static int fsm_dsp_get_ctl_by_pcm_info(struct mixer *mixer, char *pcm_dev_name, 
                                            const char *ctl_cmd, struct mixer_ctl **ctl) {
    char *mixer_name = NULL;
    uint32_t ctl_len = 0;

    if ((mixer == NULL) || (pcm_dev_name == NULL) || (ctl_cmd == NULL) || (ctl == NULL)) {
        AHAL_ERR("FourSemi INTF pointer is null");
        return FSM_ERR;
    }

    ctl_len = strlen(pcm_dev_name) + 1 + strlen(ctl_cmd) + 1;
    mixer_name = (char *)calloc(1, ctl_len);
    if (mixer_name == NULL) {
        AHAL_ERR("FourSemi INTF calloc for mixer name failed");
        return FSM_ERR;
    }
    snprintf(mixer_name, ctl_len, "%s %s", pcm_dev_name, ctl_cmd);
    AHAL_DBG("FourSemi INTF mixer name:%s", mixer_name);
    *ctl = mixer_get_ctl_by_name(mixer, mixer_name);
    if (*ctl == NULL) {
        AHAL_ERR("FourSemi INTF get mixer ctl(%s) failed", mixer_name);
        free(mixer_name);
        mixer_name = NULL;
        return FSM_ERR;
    }
    free(mixer_name);
    mixer_name = NULL;
    return FSM_OK;
}
#if 0


static int fsm_set_further_graph_config(struct fsm_monitor *monitor, const char *val) {
    struct mixer_ctl *ctl = NULL;
    int ret;

    ret = fsm_dsp_get_ctl_by_pcm_info(monitor->virt_mixer, monitor->pcm_device_name, FSM_CTL_CONTROL, &ctl);
    if (ret)
        return ret;
    ret = mixer_ctl_set_enum_by_string(ctl, val);
    if (ret < 0) {
        PAL_ERR("FourSemi INTF set ctl data failed");
        return FSM_ERR;
    }

    return FSM_OK;
}


static int fsm_dsp_get_miid(struct fsm_monitor *monitor, uint32_t tag_id, const char *backend_name) {
    int ret = 0;
    struct mixer_ctl *ctl = NULL;
    char payload[1024] = {0};
    struct gsl_tag_module_info *tag_info = NULL;
    struct gsl_tag_module_info_entry *tag_entry = NULL;
    unsigned int i = 0;
    int offset = 0;
    struct gsl_module_id_info_entry *mod_info_entry = NULL;

    ret = fsm_set_further_graph_config(monitor, backend_name);
    if (ret < 0) {
        PAL_ERR("FourSemi INTF set backend name:%s failed", backend_name);
        return FSM_ERR;
    }

    ret = fsm_dsp_get_ctl_by_pcm_info(monitor->virt_mixer, monitor->pcm_device_name, FSM_CTL_GET_TAGGED_INFO, &ctl);
    if (ret)
        return ret;
    ret = mixer_ctl_get_array(ctl, (void *)payload, 1024);
    if (ret < 0) {
        PAL_ERR("FourSemi INTF get payload failed");
        return FSM_ERR;
    }
    tag_info = (struct gsl_tag_module_info *)payload;
    PAL_DBG("FourSemi INTF num of tags are %d\n", tag_info->num_tags);
    ret = FSM_ERR;
    tag_entry = (struct gsl_tag_module_info_entry *)(&tag_info->tag_module_entry[0]);
    offset = 0;
    for (i = 0; i < tag_info->num_tags; i++) {
        tag_entry += offset / sizeof(struct gsl_tag_module_info_entry);

        PAL_DBG("FourSemi INTF tag id[%d] = 0x%x, num_modules = 0x%x\n", i, tag_entry->tag_id, tag_entry->num_modules);
        offset = sizeof(struct gsl_tag_module_info_entry) + (tag_entry->num_modules * sizeof(struct gsl_module_id_info_entry));
        if (tag_entry->tag_id == tag_id) {
            if (tag_entry->num_modules) {
                 mod_info_entry = &tag_entry->module_entry[0];
                 monitor->miid = mod_info_entry->module_iid;
                 PAL_DBG("FourSemi INTF MIID is 0x%x\n", monitor->miid);
                 ret = FSM_OK;
                 break;
            }
        }
    }
    if (monitor->miid == 0) {
         ret = FSM_ERR;
         PAL_ERR("FourSemi INTF No matching MIID found for tag: 0x%x, error:%d", tag_id, ret);
    }

    return ret;
}


int fsm_get_algo_flag(struct fsm_monitor *monitor, int32_t *flag) {
    int ret, i;
    uint8_t *payload = NULL;
    uint32_t payload_size = 0;
    struct mixer_ctl *ctl = NULL;
    struct fs_module_param_data_t *header = NULL;


    if (monitor == NULL) {
        PAL_ERR("FourSemi INTF monitor is null, return directly");
        return FSM_ERR;
    }

    ret = fsm_dsp_get_ctl_by_pcm_info(monitor->virt_mixer, monitor->pcm_device_name, FSM_CTL_GET_PARAM, &ctl);
    if (ret < 0) {
        PAL_ERR("FourSemi INTF get ctl failed");
        return ret;
    }
    payload_size = FS_ALIGN_8BYTE(sizeof(struct fs_module_param_data_t) + sizeof(int32_t));
    payload = (uint8_t *)calloc(1, payload_size);
    if (payload == NULL) {
        PAL_ERR("FourSemi INTF calloc memery for paylad failed");
        return FSM_ERR;
    }

    header = (struct fs_module_param_data_t *)payload;
    header->miid = monitor->miid;
    header->param_id = FSADSP_GET_ALGO_FLAG_PARAM_ID;
    header->param_size = payload_size - sizeof(struct fs_module_param_data_t);
    header->error_code = 0;

    ret = mixer_ctl_set_array(ctl, payload, payload_size);
    if (ret < 0) {
        PAL_ERR("FourSemi INTF send payload failed");
        goto exit;
    }
    memset(payload, 0, payload_size);
    ret = mixer_ctl_get_array(ctl, payload, payload_size);
    if (ret < 0) {
        PAL_ERR("FourSemi INTF get payload failed");
        goto exit;
    }

    for (i = 0; i < payload_size; i++) {
        PAL_DBG("FourSemi INTF algo flag payload[%d] = 0x%x", i, payload[i]);
    }
    memcpy(flag, payload + sizeof(struct fs_module_param_data_t), sizeof(int32_t));

    PAL_INFO("FourSemi INTF algo flag:0x%x", *flag);

exit:
    free(payload);
    payload = NULL;
    return ret;
}
static int fsm_init_cont_for_payload() {
    int ret = 0;
    int32_t flag = 0;
    std::string be_name;
    std::shared_ptr <ResourceManager> rm = NULL;
    int device_id = PAL_DEVICE_OUT_SPEAKER;
    struct fsm_monitor *monitor = fsm_get_monitor();
    unsigned int i = 0;

    if (monitor == NULL) {
        PAL_ERR("FourSemi INTF get monitor failed");
        return FSM_ERR;
    }

    rm = ResourceManager::getInstance();
    ret = rm->getBackendName(device_id, be_name);
    if (ret < 0) {
        PAL_ERR("FourSemi INTF get backend name failed");
        return FSM_ERR;
    }

    //if (monitor->miid == 0 || monitor->fe_pcm == 0) {
    for(i = 100; i < 140; i++) {
        monitor->pcm_device_name = rm->getDeviceNameFromID(i);
        if (!monitor->pcm_device_name) {
            continue;
        }

        ret = fsm_dsp_get_miid(monitor, MODULE_SP, be_name.c_str());
        if (ret < 0) {
            continue;
        }

        if(fsm_get_algo_flag(monitor, &flag) != FSM_OK)
            continue;

        if (flag == 0x1000FA08) {
            monitor->fe_pcm = i;
            break;
        }
    }
    //}

    if(i == 140){
        PAL_ERR("FourSemi INTF get miid/flag failed");
        return FSM_ERR;
    }

    PAL_INFO("FourSemi INTF pcm id[%d], pcm device name[%s], miid[0x%x]", monitor->fe_pcm, monitor->pcm_device_name, monitor->miid);

    return ret;
}
#endif
int fsm_send_payload_to_dsp(struct fsm_algo_info *info, uint32_t param_id, char *data, unsigned int data_size) {
    int ret = 0;
    uint8_t *payload = NULL;
    uint32_t payload_size = 0;
    uint32_t pad_bytes = 0;
    struct mixer_ctl *ctl = NULL;
    struct fs_module_param_data_t *header = NULL;

    if (data == NULL || data_size == 0 || param_id == 0) {
        AHAL_ERR("FourSemi INTF invalid param");
        return FSM_ERR;
    }
#if 0
    ret = fsm_init_cont_for_payload();
    if (ret < 0) {
        PAL_ERR("FourSemi INTF fsm_init_cont_for_payload failed");
        return ret;
    }
#endif
    ret = fsm_dsp_get_ctl_by_pcm_info(info->virt_mixer, info->pcm_device_name, FSM_CTL_SET_PARAM, &ctl);
    if (ret < 0)
        return ret;

    payload_size = sizeof(struct fs_module_param_data_t) + FS_ALIGN_4BYTE(data_size);
    pad_bytes = FS_PADDING_ALIGN_8BYTE(payload_size);
    payload = (uint8_t *)calloc(1, payload_size + pad_bytes);
    if (payload == NULL) {
        AHAL_ERR("FourSemi INTF calloc memory for payload failed");
        return FSM_ERR;
    }

    header = (struct fs_module_param_data_t *)payload;
    header->miid = info->miid;
    header->param_id = param_id;
    header->param_size = payload_size - sizeof(struct fs_module_param_data_t);
    header->error_code = 0;

    memcpy(payload + sizeof(struct fs_module_param_data_t), data, data_size);
    ret = mixer_ctl_set_array(ctl, payload, payload_size + pad_bytes);
    if (ret < 0)
        AHAL_ERR("FourSemi INTF %s send payload failed", __func__);

    free(payload);
    payload = NULL;
    return ret;
}

int fsm_get_payload_from_dsp(struct fsm_algo_info *info, uint32_t param_id, char * data, unsigned int data_size) {
    int ret, i;
    uint8_t *payload = NULL;
    uint32_t payload_size = 0;
    struct mixer_ctl *ctl = NULL;
    struct fs_module_param_data_t *header = NULL;

    if((data == NULL) || (param_id == 0) || (data_size == 0)) {
        AHAL_ERR("FourSemi INTF invalid param, please check");
        return FSM_ERR;
    }
#if 0
    ret = fsm_init_cont_for_payload();
    if (ret < 0) {
        PAL_ERR("FourSemi INTF fsm_init_cont_for_payload failed");
        return FSM_ERR;
    }
#endif
    ret = fsm_dsp_get_ctl_by_pcm_info(info->virt_mixer, info->pcm_device_name, FSM_CTL_GET_PARAM, &ctl);
    if (ret < 0)
        return ret;
    payload_size = FS_ALIGN_8BYTE(sizeof(struct fs_module_param_data_t) + data_size);
    payload = (uint8_t *)calloc(1, payload_size);
    if (payload == NULL) {
        AHAL_ERR("FourSemi INTF calloc memery for paylad failed");
        return FSM_ERR;
    }

    header = (struct fs_module_param_data_t *)payload;
    header->miid = info->miid;
    header->param_id = param_id;
    header->param_size = payload_size - sizeof(struct fs_module_param_data_t);
    header->error_code = 0;

    ret = mixer_ctl_set_array(ctl, payload, payload_size);
    if (ret < 0) {
        AHAL_ERR("FourSemi INTF send payload failed");
        goto exit;
    }
    memset(payload, 0, payload_size);
    ret = mixer_ctl_get_array(ctl, payload, payload_size);
    if (ret < 0) {
        AHAL_ERR("FourSemi INTF get payload failed");
        goto exit;
    }

    for (i = 0; i < payload_size; i++) {
        AHAL_DBG("FourSemi INTF payload[%d] = 0x%x", i, payload[i]);
    }
    AHAL_ERR("FourSemi INTF get payload data_size:%d", data_size);
    memcpy(data, payload + sizeof(struct fs_module_param_data_t), data_size);
    ret = FSM_OK;

exit:
    free(payload);
    payload = NULL;
    return ret;
}
