// SPDX-License-Identifier:    GPL-2.0+
/*
 * Copyright (C) 2016 Stefan Roese <sr@denx.de>
 * Copyright (C) 2020 Marvell International Ltd.
 */

#include <dm.h>
#include <env.h>
#include <hexdump.h>
#include <i2c.h>
#include <phy.h>
#include <power/regulator.h>

/* I2C */
#define GSC_EEPROM_BUSNO		0
#define GSC_EEPROM_ADDR			0x51

DECLARE_GLOBAL_DATA_PTR;

struct malibu_board_info {
	u8 mac[6];		/* 0x00: MAC base */
	char equiv_dts[16];	/* 0x06: equivalent device-tree */
	u8 res0[2];		/* 0x16: reserved */
	u32 serial;		/* 0x18: Serial Number */
	u8 res1[4];		/* 0x1C: reserved */
	u8 mfgdate[4];		/* 0x20: MFG date */
	u8 macno;		/* 0x24: number of mac addrs */
	u8 res2[6];		/* 0x25 */
	u8 sdram_size;		/* 0x2B: (16 << n) MB */
	u8 sdram_speed;		/* 0x2C: (33.333 * n) MHz */
	u8 sdram_width;		/* 0x2D: (8 << n) bit */
	u8 res3[2];		/* 0x2E */
	char model[16];		/* 0x30: model string */
	u8 res4[14];		/* 0x40 */
	u8 chksum[2];		/* 0x4E */
};

struct malibu_board_info gsc_info;

/* return a mac address from EEPROM info */
int eeprom_getmac(int index, uint8_t *address)
{
	int i, j;
	u32 maclow, machigh;
	u64 mac;

	j = 0;
	maclow = gsc_info.mac[5];
	maclow |= gsc_info.mac[4] << 8;
	maclow |= gsc_info.mac[3] << 16;
	maclow |= gsc_info.mac[2] << 24;
	machigh = gsc_info.mac[1];
	machigh |= gsc_info.mac[0] << 8;
	mac = machigh;
	mac <<= 32;
	mac |= maclow;
	for (i = 0; i < gsc_info.macno; i++, j++) {
		if (index == j)
			goto out;
	}

	return -EINVAL;

out:
	mac += i;
	address[0] = (mac >> 40) & 0xff;
	address[1] = (mac >> 32) & 0xff;
	address[2] = (mac >> 24) & 0xff;
	address[3] = (mac >> 16) & 0xff;
	address[4] = (mac >> 8) & 0xff;
	address[5] = (mac >> 0) & 0xff;

	return 0;
}

static int eeprom_read(int busno, int slave, int alen, struct malibu_board_info *info)
{
	int i;
	int chksum;
	unsigned char *buf = (unsigned char *)info;
	struct udevice *dev, *bus;
	int ret;

	/* probe device */
	ret = uclass_get_device_by_seq(UCLASS_I2C, busno, &bus);
	if (ret)
		return ret;
	ret = dm_i2c_probe(bus, slave, 0, &dev);
	if (ret)
		return ret;

	/* read eeprom config section */
	memset(info, 0, sizeof(*info));
	ret = i2c_set_chip_offset_len(dev, alen);
	if (ret) {
		puts("EEPROM: Failed to set alen\n");
		return ret;
	}
	ret = dm_i2c_read(dev, 0x00, buf, sizeof(*info));
	if (ret) {
		printf("EEPROM: Failed to read EEPROM\n");
		return ret;
	}

	/* validate checksum */
	for (chksum = 0, i = 0; i < (int)sizeof(*info) - 2; i++)
		chksum += buf[i];
	if ((info->chksum[0] != chksum >> 8) ||
	    (info->chksum[1] != (chksum & 0xff))) {
		printf("EEPROM: I2C%d@0x%02x: Invalid Checksum\n", busno, slave);
		print_hex_dump_bytes("", DUMP_PREFIX_NONE, buf, sizeof(*info));
		memset(info, 0, sizeof(*info));
		return -EINVAL;
	}

	/* sanity check valid model */
	if (info->model[0] != 'G' || info->model[1] != 'W') {
		printf("EEPROM: I2C%d@0x%02x: Invalid Model in EEPROM\n", busno, slave);
		print_hex_dump_bytes("", DUMP_PREFIX_NONE, buf, sizeof(*info));
		memset(info, 0, sizeof(*info));
		return -EINVAL;
	}

	return 0;
}

/* determine BOM revision from model */
static int get_bom_rev(const char *str)
{
	int  rev_bom = 0;
	int i;

	for (i = strlen(str) - 1; i > 0; i--) {
		if (str[i] == '-')
			break;
		if (str[i] >= '1' && str[i] <= '9') {
			rev_bom = str[i] - '0';
			break;
		}
	}
	return rev_bom;
}

/* determine PCB revision from model */
static char get_pcb_rev(const char *str)
{
	char rev_pcb = 'A';
	int i;

	for (i = strlen(str) - 1; i > 0; i--) {
		if (str[i] == '-')
			break;
		if (str[i] >= 'A') {
			rev_pcb = str[i];
			break;
		}
	}
	return rev_pcb;
}

/*
 * get dt name based on model and detail level:
 */
#define snprintfcat(dest, sz, fmt, ...) \
	snprintf((dest) + strlen(dest), (sz) - strlen(dest), fmt, ##__VA_ARGS__)
static const char *eeprom_get_dtb_name(int level, char *buf, int sz)
{
	const char *pre = "cn9130-malibu-";
	int model, rev_pcb, rev_bom;

	model = ((gsc_info.model[2] - '0') * 1000)
		+ ((gsc_info.model[3] - '0') * 100)
		+ ((gsc_info.model[4] - '0') * 10)
		+ (gsc_info.model[5] - '0');
	rev_pcb = tolower(get_pcb_rev(gsc_info.model));
	rev_bom = get_bom_rev(gsc_info.model);

	snprintf(buf, sz, "%s%04d", pre, model);
	switch (level) {
	case 0: /* full model wth PCB and BOM revision first (ie gw8901-a1) */
		if (rev_bom)
			snprintfcat(buf, sz, "-%c%d", rev_pcb, rev_bom);
			else
			snprintfcat(buf, sz, "-%c", rev_pcb);
		break;
	case 1: /* don't care about BOM revision */
		snprintfcat(buf, sz, "-%c", rev_pcb);
		break;
	case 2: /* don't care about PCB or BOM revision */
		break;
	case 3: /* don't care about last digit of model */
		buf[strlen(buf) - 1] = 'x';
		break;
	case 4: /* don't care about last two digits of model */
		buf[strlen(buf) - 1] = 'x';
		buf[strlen(buf) - 2] = 'x';
		break;
	default:
		return NULL;
		break;
	}

	return buf;
}

void board_gsc_info(void)
{
	printf("Model   : %s\n", gsc_info.model);
	printf("Serial  : %d\n", gsc_info.serial);
	printf("MFGDate : %02x-%02x-%02x%02x\n",
	       gsc_info.mfgdate[0], gsc_info.mfgdate[1],
	       gsc_info.mfgdate[2], gsc_info.mfgdate[3]);
}

__weak int soc_early_init_f(void)
{
	return 0;
}

int board_early_init_f(void)
{
	soc_early_init_f();

	return 0;
}

int board_early_init_r(void)
{
	if (CONFIG_IS_ENABLED(DM_REGULATOR)) {
		/* Check if any existing regulator should be turned down */
		regulators_enable_boot_off(false);
	}

	return 0;
}

int board_init(void)
{
	struct udevice *dev;
	int ret;

	/* address of boot parameters */
	gd->bd->bi_boot_params = CONFIG_SYS_SDRAM_BASE + 0x100;

	/* probe comphy */
	if (IS_ENABLED(CONFIG_MVEBU_COMPHY_SUPPORT)) {
		ret = uclass_get_device_by_driver(UCLASS_MISC,
						  DM_DRIVER_GET(mvebu_comphy),
						  &dev);
		if (ret)
			printf("Failed to initialize comphy!\n");
	}

	return 0;
}

int board_late_init(void)
{
	const char *str;
	char env[32];
	int ret, i;
	u8 enetaddr[6];
	char fdt[64];

	ret = eeprom_read(GSC_EEPROM_BUSNO, GSC_EEPROM_ADDR, 1, &gsc_info);
	if (ret) {
		puts("ERROR: Failed to probe EEPROM\n");
		memset(&gsc_info, 0, sizeof(gsc_info));
		return -1;
	}
	board_gsc_info();

	/* Set board serial/model */
	if (!env_get("serial#"))
		env_set_ulong("serial#", gsc_info.serial);
	env_set("model", gsc_info.model);

	/* Set fdt_file vars */
	i = 0;
	do {
		str = eeprom_get_dtb_name(i, fdt, sizeof(fdt));
		if (str) {
			sprintf(env, "fdt_file%d", i + 1);
			strcat(fdt, ".dtb");
			env_set(env, fdt);
		}
		i++;
	} while (str);

	/* Set mac addrs */
	i = 0;
	do {
		if (i)
			sprintf(env, "eth%daddr", i);
		else
			sprintf(env, "ethaddr");
		str = env_get(env);
		if (!str) {
			ret = eeprom_getmac(i, enetaddr);
			if (!ret)
				eth_env_set_enetaddr(env, enetaddr);
		}
		i++;
	} while (!ret);

	return 0;
}

int ft_board_setup(void *fdt, struct bd_info *bd)
{
	printf("%s: %s\n", __FILE__, __func__);

	/* set board model dt prop */
	fdt_setprop_string(fdt, 0, "board", gsc_info.model);

	return 0;
}

int board_phy_config(struct phy_device *phydev)
{
	switch (phydev->phy_id) {
	case 0xd565a401: /* MaxLinear GPY111 */
		puts("GPY111 ");
		break;
	case 0x31c31c13:
		puts("AQR113C ");
		break;
	}

	if (phydev->drv->config)
		phydev->drv->config(phydev);

	return 0;
}
