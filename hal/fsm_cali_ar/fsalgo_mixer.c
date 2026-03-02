#define LOG_TAG "AHAL:FourSemi: mixer"

#include <AudioCommon.h>

#include <tinyalsa/asoundlib.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <getopt.h>
#include <errno.h>

static int tinymix_get_control(struct mixer *mixer, const char *control,
                                  int *vbuf, int *nvalue);
static int tinymix_set_value(struct mixer *mixer, const char *control,
                             char **values, unsigned int num_values);
static void tinymix_print_enum(struct mixer_ctl *ctl, const char *space,
                               int print_all);
static int args_parser(const char *args, char **buf, size_t max)
{
    const char *delimiters = " ,";
    char *p = NULL;
    int n = 0;
    char *str = strdup(args);

    p = strtok(str, delimiters);
    while(p != NULL) {
        buf[n++] = strdup(p);
        p = strtok(NULL, delimiters);

        if(n > max) {
            //print something
            break;
        }
    }

    free(str);
    return n;
}

static int isnumber(const char *str) {
    char *end;

    if (str == NULL || strlen(str) == 0)
        return 0;

    strtol(str, &end, 0);
    return strlen(end) == 0;
}

static void tinymix_print_enum(struct mixer_ctl *ctl, const char *space,
                               int print_all)
{
    unsigned int num_enums;
    unsigned int i;
    const char *string;
    int control_value = mixer_ctl_get_value(ctl, 0);

    if (print_all) {
        num_enums = mixer_ctl_get_num_enums(ctl);
        for (i = 0; i < num_enums; i++) {
            string = mixer_ctl_get_enum_string(ctl, i);
            AHAL_INFO("%s%s%s",
                   control_value == (int)i ? ">" : "", string,
                   (i < num_enums - 1) ? space : "");
        }
    }
    else {
        string = mixer_ctl_get_enum_string(ctl, control_value);
        AHAL_INFO("%s", string);
    }
 }
static int tinymix_get_control_info(struct mixer *mixer, const char *control,
                                  enum mixer_ctl_type *type, int *nvalue)
{
    struct mixer_ctl *ctl;

    if (isnumber(control))
        ctl = mixer_get_ctl(mixer, atoi(control));
    else
        ctl = mixer_get_ctl_by_name(mixer, control);

    if (!ctl) {
        AHAL_ERR("Invalid mixer control: %s\n", control);
        return ENOENT;
    }

    *type = mixer_ctl_get_type(ctl);
    *nvalue = mixer_ctl_get_num_values(ctl);

    return 0;
}

static int tinymix_get_control(struct mixer *mixer, const char *control,
                                  int *vbuf, int *nvalue)
{
    struct mixer_ctl *ctl;
    enum mixer_ctl_type type;
    unsigned int num_values;
    unsigned int value;
    unsigned int i;
    int ret = 0;
    char *buf = NULL;
    char *cvbuf = (char *)vbuf;
    size_t len;
    unsigned int tlv_header_size = 0;
    const char *space = " ";

    if (isnumber(control))
        ctl = mixer_get_ctl(mixer, atoi(control));
    else
        ctl = mixer_get_ctl_by_name(mixer, control);

    if (!ctl) {
        AHAL_ERR("Invalid mixer control: %s\n", control);
        return ENOENT;
    }

    type = mixer_ctl_get_type(ctl);
    num_values = mixer_ctl_get_num_values(ctl);

    if (type == MIXER_CTL_TYPE_BYTE) {
        if (mixer_ctl_is_access_tlv_rw(ctl)) {
            tlv_header_size = TLV_HEADER_SIZE;
        }
        buf = calloc(1, num_values + tlv_header_size);
        if (buf == NULL) {
            AHAL_ERR("Failed to alloc mem for bytes %d\n", num_values);
            return ENOENT;
        }

        len = num_values;
        ret = mixer_ctl_get_array(ctl, buf, len + tlv_header_size);
        if (ret < 0) {
            AHAL_ERR("Failed to mixer_ctl_get_array\n");
            free(buf);
            return ENOENT;
        }
    }
    for (i = 0; i < num_values; i++) {
        switch (type)
        {
        case MIXER_CTL_TYPE_INT:
            value =  mixer_ctl_get_value(ctl, i);
            vbuf[i] = value;
            break;
        case MIXER_CTL_TYPE_BOOL:
            value =  mixer_ctl_get_value(ctl, i);
            vbuf[i] = value;
            break;
        case MIXER_CTL_TYPE_ENUM:
            value = mixer_ctl_get_value(ctl, 0);
            vbuf[i] = value;
            tinymix_print_enum(ctl, space, 0);
            break;
        case MIXER_CTL_TYPE_BYTE:
            /* skip printing TLV header if exists */
            cvbuf[i] =  buf[i + tlv_header_size];
            break;
        default:
            AHAL_INFO("unknown ctl type");
            break;
        }
    }

    *nvalue = num_values;
    free(buf);

    return 0;
}

static int tinymix_set_byte_ctl(struct mixer_ctl *ctl,
    char **values, unsigned int num_values)
{
    int ret;
    char *buf;
    char *end;
    unsigned int i;
    long n;
    unsigned int *tlv, tlv_size;
    unsigned int tlv_header_size = 0;

    if (mixer_ctl_is_access_tlv_rw(ctl)) {
        tlv_header_size = TLV_HEADER_SIZE;
    }

    tlv_size = num_values + tlv_header_size;

    buf = calloc(1, tlv_size);
    if (buf == NULL) {
        AHAL_ERR("set_byte_ctl: Failed to alloc mem for bytes %d\n", num_values);
        return -1;
    }

    tlv = (unsigned int *)buf;
    tlv[0] = 0;
    tlv[1] = num_values;

    for (i = 0; i < num_values; i++) {
        errno = 0;
        n = strtol(values[i], &end, 0);
        if (*end) {
            AHAL_ERR("%s not an integer\n", values[i]);
            goto fail;
        }
        if (errno) {
            AHAL_ERR("strtol: %s: %s\n", values[i],
                strerror(errno));
            goto fail;
        }
        if (n < 0 || n > 0xff) {
            AHAL_ERR("%s should be between [0, 0xff]\n",
                values[i]);
            goto fail;
        }
        /* start filling after the TLV header */
        buf[i + tlv_header_size] = n;
    }

    ret = mixer_ctl_set_array(ctl, buf, tlv_size);
    if (ret < 0) {
        AHAL_ERR("Failed to set binary control\n");
        goto fail;
    }

    free(buf);
    return 0;

fail:
    free(buf);
    return -1;
}

static int tinymix_set_value(struct mixer *mixer, const char *control,
                             char **values, unsigned int num_values)
{
    struct mixer_ctl *ctl;
    enum mixer_ctl_type type;
    unsigned int num_ctl_values;
    unsigned int i;

    if (isnumber(control))
        ctl = mixer_get_ctl(mixer, atoi(control));
    else
        ctl = mixer_get_ctl_by_name(mixer, control);

    if (!ctl) {
        AHAL_ERR("Invalid mixer control: %s\n", control);
        return ENOENT;
    }

    type = mixer_ctl_get_type(ctl);
    num_ctl_values = mixer_ctl_get_num_values(ctl);

    if (type == MIXER_CTL_TYPE_BYTE) {
        return tinymix_set_byte_ctl(ctl, values, num_values);
    }

    if (isnumber(values[0])) {
        if (num_values == 1) {
            /* Set all values the same */
            int value = atoi(values[0]);

            for (i = 0; i < num_ctl_values; i++) {
                if (mixer_ctl_set_value(ctl, i, value)) {
                    AHAL_ERR("Error: invalid value\n");
                    return EINVAL;
                }
            }
        } else {
            /* Set multiple values */
            if (num_values > num_ctl_values) {
                AHAL_ERR("Error: %u values given, but control only takes %u\n",
                        num_values, num_ctl_values);
                return EINVAL;
            }
            for (i = 0; i < num_values; i++) {
                if (mixer_ctl_set_value(ctl, i, atoi(values[i]))) {
                    AHAL_ERR("Error: invalid value for index %d\n", i);
                    return EINVAL;
                }
            }
        }
    } else {
        if (type == MIXER_CTL_TYPE_ENUM) {
            if (num_values != 1) {
                AHAL_ERR("Enclose strings in quotes and try again\n");
                return EINVAL;
            }
            if (mixer_ctl_set_enum_by_string(ctl, values[0])) {
                AHAL_ERR("Error: invalid enum value\n");
                return EINVAL;
            }
        } else {
            AHAL_ERR("Error: only enum types can be set with strings\n");
            return EINVAL;
        }
    }

    return 0;
}

/*
 *set mixer contols to Asoc, it work like tinymix.
 *it base tinymix actully
 *
 *@return - 0     : success
 *        - other : failed
 */
int fsm_tmixer_set(struct mixer *mixer, char const *control, const char *value)
{
    int ret = 0;
    char *argv[1024];
    int argc = 1;
    int i = 0;

    if (value==NULL)
        return -1;

    argc = args_parser(value, argv, 1024);
    if (argc <= 0)
        return -1;

    ret = tinymix_set_value(mixer, control, argv, argc);

    for (i = 0; i < argc; i ++)
        free(argv[i]);

    return ret;
}

/*
 *get mixer contols to Asoc, it work like tinymix.
 *it base tinymix actully
 *
 *@return - 0     : success
 *        - other : failed
 *TODO: Check the size!!!
 */
int fsm_tmixer_get(struct mixer *mixer, const char *control, int *value)
{
    char *cvalue = (char *)value;
    int *vbuf = NULL;
    enum mixer_ctl_type type;
    int ret = 0, i;
    int nvalue = 0;

    ret = tinymix_get_control_info(mixer, control, &type, &nvalue);
    if (ret != 0) {
        AHAL_INFO("Get '%s' Failed\n", control);
        goto exit;
    }

    if (type == MIXER_CTL_TYPE_BYTE) {
        vbuf = calloc(nvalue, sizeof(char));
    } else {
        vbuf = calloc(1, sizeof(int));
    }

    ret = tinymix_get_control(mixer, control, vbuf, &nvalue);
    if (ret != 0) {
        AHAL_INFO("Get '%s' Failed\n", control);
        goto exit;
    }

    if (type == MIXER_CTL_TYPE_BYTE) {
        for(i = 0; i < nvalue; i ++)
            cvalue[i] = ((char *)vbuf)[i];
    } else {
        *value = *vbuf;
        AHAL_INFO("isBug ? ( %d )\n", nvalue != 1);
    }

exit:
    free(vbuf);
    return nvalue;
}

int fsm_tmixer_ctl_set(char *control, char *str_value)
{
    int i = 0;
    int num_values = 0;

    int value[128] = {-1,};
    struct mixer *mixer;

    mixer = mixer_open(0);
    if (!mixer) {
        AHAL_ERR("Failed to open mixer\n");
        return ENODEV;
    }

    fsm_tmixer_set(mixer, control, str_value);
    num_values = fsm_tmixer_get(mixer, control, value);

    AHAL_INFO("Default: %s, num=%d\n", str_value, num_values);

    AHAL_INFO("%s: ", control);
    for(i = 0; i < num_values; i++)
        AHAL_INFO("%x ", value[i]);

    AHAL_INFO("\n");
    mixer_close(mixer);

    return 0;
}
