#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>
#include <math.h>
#include <unistd.h>
#include "fsalgo_calib.h"

static int g_dev_num;
#define FRSM_DEV_MAX        (8)
#define FRSM_AMP_PATH       "/sys/class/frsm_amp/"

static int frsm_reg_read(int spkid, int reg, uint16_t *pval)
{
    const char *path = FRSM_AMP_PATH "regs";
    int data;
    FILE *fp;
    int ret;

    if (pval == NULL || g_dev_num <= 0) {
        AHAL_ERR("reg_read: Invalid parameters");
        return -1;
    }

    fp = fopen(path, "rw+");
    if (fp == NULL) {
        AHAL_ERR("Failed to open %s", path);
        return -1;
    }

    data = ((spkid & 0xFF) << 8) | (reg & 0xFF);
    ret = fprintf(fp, "%d", data);
    if (ret < 0) {
        AHAL_ERR("Failed to set reg %08x", data);
        fclose(fp);
        return ret;
    }

    ret = fscanf(fp, "%x:%x\n", &reg, &data);
    fclose(fp);
    if (ret != 2) {
        AHAL_ERR("Failed to read reg: %d %02x %d", spkid, reg, ret);
        return -1;
    }

    *pval = data & 0xFFFF;

    return 0;
}

static int frsm_reg_write(int spkid, int reg, uint16_t val)
{
    const char *path = FRSM_AMP_PATH "regs";
    int data, size;
    FILE *fp;
    int ret;

    if (g_dev_num <= 0) {
        AHAL_ERR("reg_write: Invalid parameters");
        return -1;
    }

    fp = fopen(path, "wb");
    if (fp == NULL) {
        AHAL_ERR("Failed to open %s", path);
        return -1;
    }

    size = sizeof(data);
    data = ((spkid & 0xFF) << 24) | ((reg & 0xFF) << 16) | val;
    ret = fprintf(fp, "%d", data);
    fclose(fp);
    if (ret < 0) {
        AHAL_ERR("Failed to write reg %08x %d", data, ret);
        return ret;
    }

    return 0;
}

static int frsm_get_ndev(int *ndev)
{
    const char *path = FRSM_AMP_PATH "ndev";
    FILE *fp;
    int ret;

    if (ndev == NULL)
        return -1;

    fp = fopen(path, "r");
    if (fp == NULL) {
        AHAL_ERR("Failed to open %s", path);
        return -1;
    }

    ret = fscanf(fp, "%d", ndev);
    fclose(fp);
    if (ret != 1) {
        AHAL_ERR("Failed to get ndev:%d", ret);
        return -1;
    }

    g_dev_num = *ndev;

    return 0;
}

static int fs199x_set_calib_mode(int spk, bool is_set)
{
    static uint16_t state[FRSM_DEV_MAX];
    uint16_t val;
    int ret;

    if (is_set) {
        ret = frsm_reg_read(spk, 0x39, &val);
        if (val & 0x8000)
            state[spk-1] |= (1 << 0);
        ret |= frsm_reg_write(spk, 0x39, val & 0x7FFF); // NG off
        ret |= frsm_reg_read(spk, 0x3C, &val);
        if (val & 0x8000)
            state[spk-1] |= (1 << 1);
        ret |= frsm_reg_write(spk, 0x3C, val & 0x7FFF); // LPM off
        ret |= frsm_reg_read(spk, 0x41, &val);
        if (val & 0x8000)
            state[spk-1] |= (2 << 1);
        ret |= frsm_reg_write(spk, 0x41, val | 0x8000); // DSP on
        ret |= frsm_reg_read(spk, 0x4C, &val);
        ret |= frsm_reg_write(spk, 0x4C, (val | 0x0008) & 0xDFFF); // TS always on
        ret |= frsm_reg_read(spk, 0x94, &val);
        ret |= frsm_reg_write(spk, 0x94, val | 0x0800); // set bit11
    } else {
        ret = frsm_reg_read(spk, 0x39, &val);
        if (state[spk-1] & (1 << 0))
            ret |= frsm_reg_write(spk, 0x39, val | 0x8000);
        ret |= frsm_reg_read(spk, 0x3C, &val);
        if (state[spk-1] & (1 << 1))
            ret |= frsm_reg_write(spk, 0x3C, val | 0x8000);
        if ((state[spk-1] & (2 << 1)) == 0)
            ret |= frsm_reg_write(spk, 0x41, val & 0x7FFF);
        ret |= frsm_reg_read(spk, 0x4C, &val);
        ret |= frsm_reg_write(spk, 0x4C, val | 0x2008);
        ret |= frsm_reg_read(spk, 0x94, &val);
        ret |= frsm_reg_write(spk, 0x94, val & 0xF7FF); // clear bit11
    }

    return ret;
}

static int fs194x_set_calib_mode(int spk, bool is_set)
{
    static uint16_t state[FRSM_DEV_MAX];
    uint16_t val;
    int ret;

    if (is_set) {
        ret = frsm_reg_read(spk, 0x39, &val);
        if (val & 0x8000)
            state[spk-1] |= (1 << 0);
        ret |= frsm_reg_write(spk, 0x39, val & 0x7FFF);
        ret |= frsm_reg_read(spk, 0x3C, &val);
        if (val & 0x8000)
            state[spk-1] |= (1 << 1);
        ret |= frsm_reg_write(spk, 0x3C, val & 0x7FFF);
        ret |= frsm_reg_read(spk, 0x4C, &val);
        ret |= frsm_reg_write(spk, 0x4C, (val | 0x0008) & 0xDFFF);
    } else {
        ret = frsm_reg_read(spk, 0x39, &val);
        if (state[spk-1] & (1 << 0))
            ret |= frsm_reg_write(spk, 0x39, val | 0x8000);
        ret |= frsm_reg_read(spk, 0x3C, &val);
        if (state[spk-1] & (1 << 1))
            ret |= frsm_reg_write(spk, 0x3C, val | 0x8000);
        ret |= frsm_reg_read(spk, 0x4C, &val);
        ret |= frsm_reg_write(spk, 0x4C, val | 0x2008);
    }

    return ret;
}

static int fs19xx_amp_switch(int spk, bool on)
{
    int ret;

    if (on)
        ret = frsm_reg_write(spk, 0x10, 0x0000);
    else
        ret = frsm_reg_write(spk, 0x10, 0x0001);

    return ret;
}

static int fs199x_set_calib_scene(int spk, int dev_num, bool is_set)
{
    int ret;

    ret = fs19xx_amp_switch(spk, false);
    usleep(35*1000); // wait power down
    ret |= fs199x_set_calib_mode(spk, is_set);
    ret |= fs19xx_amp_switch(spk, true);

    return ret;
}

static int fs194x_set_calib_scene(int spk, int dev_num, bool is_set)
{
    int ret;

    ret = fs19xx_amp_switch(spk, false);
    usleep(35*1000); // wait power down
    ret |= fs194x_set_calib_mode(spk, is_set);
    ret |= fs19xx_amp_switch(spk, true);

    return ret;
}

int frsm_set_calib_mode(bool is_set)
{
    int spk, dev_num;
    uint16_t devid;
    int ret;

    ret = frsm_get_ndev(&dev_num);
    if (ret) {
        AHAL_ERR("Failed to get ndev:%d", ret);
        return ret;
    }

    for (spk = 1; spk <= dev_num; spk++) {
        ret = frsm_reg_read(spk, 0x03, &devid);
        if (ret) {
            AHAL_ERR("Failed to set spk%d calib mode:%d", spk, ret);
            continue;
        }
        if (devid == 0x0500) {
            ret = fs199x_set_calib_scene(spk, dev_num, is_set);
        } else if ((devid >> 8) == 0x29) {
            ret = fs194x_set_calib_scene(spk, dev_num, is_set);
        } else {
            AHAL_ERR("Not support devid:%x\n", devid);
            return -1;
        }
    }
    usleep(35); // wait amp on

    return 0;
}

