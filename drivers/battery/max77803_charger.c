/*
 *  max77803_charger.c
 *  Samsung max77803 Charger Driver
 *
 *  Copyright (C) 2012 Samsung Electronics
 *
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/mfd/max77803.h>
#include <linux/mfd/max77803-private.h>
#include <linux/of_gpio.h>
#ifdef CONFIG_USB_HOST_NOTIFY
#include <linux/host_notify.h>
#endif
#ifdef CONFIG_FORCE_FAST_CHARGE
#include <linux/fastchg.h>
#endif

#define ENABLE 1
#define DISABLE 0


#define RECOVERY_DELAY		3000
#define RECOVERY_CNT		5
#define REDUCE_CURRENT_STEP	100
#if defined(CONFIG_MACH_HLTEDCM) || defined(CONFIG_MACH_JS01LTEDCM)
#define MINIMUM_INPUT_CURRENT	100
#else
#define MINIMUM_INPUT_CURRENT	300
#endif

#if defined(CONFIG_MACH_FLTESKT)
#define SIOP_INPUT_LIMIT_CURRENT 1001
#else
#define SIOP_INPUT_LIMIT_CURRENT 1200
#endif
#define SIOP_CHARGING_LIMIT_CURRENT 1000
#define SLOW_CHARGING_CURRENT_STANDARD 400

struct max77803_charger_data {
	struct max77803_dev	*max77803;

	struct power_supply	psy_chg;

	struct workqueue_struct *wqueue;
	struct work_struct	chgin_work;
	struct delayed_work	isr_work;
	struct delayed_work	recovery_work;	/*  softreg recovery work */
	struct delayed_work	wpc_work;	/*  wpc detect work */
	struct delayed_work	chgin_init_work;	/*  chgin init work */

	/* mutex */
	struct mutex irq_lock;
	struct mutex ops_lock;

	/* wakelock */
	struct wakeup_source recovery_ws;
	struct wakeup_source wpc_ws;
	struct wakeup_source chgin_ws;

	unsigned int	is_charging;
	unsigned int	charging_type;
	unsigned int	battery_state;
	unsigned int	battery_present;
	unsigned int	cable_type;
	unsigned int	charging_current_max;
	unsigned int	charging_current;
	unsigned int	vbus_state;
	int		aicl_on;
	int		status;
	int		siop_level;

	int		irq_bypass;
#if defined(CONFIG_CHARGER_MAX77803)
	int		irq_batp;
#else
	int		irq_therm;
#endif
	int		irq_battery;
	int		irq_chg;
#if defined(CONFIG_CHARGER_MAX77803)
	int		irq_wcin;
#endif
	int		irq_chgin;

	/* software regulation */
	bool		soft_reg_state;
	int		soft_reg_current;

	/* unsufficient power */
	bool		reg_loop_deted;

#if defined(CONFIG_CHARGER_MAX77803)
	/* wireless charge, w(wpc), v(vbus) */
	int		wc_w_gpio;
	int		wc_w_irq;
	int		wc_w_state;
	int		wc_v_gpio;
	int		wc_v_irq;
	int		wc_v_state;
	bool		wc_pwr_det;
#endif
	int		soft_reg_recovery_cnt;

	int pmic_ver;
	int input_curr_limit_step;
	int charging_curr_step;

	sec_battery_platform_data_t	*pdata;
};

static enum power_supply_property sec_charger_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_CHARGE_TYPE,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_CURRENT_MAX,
	POWER_SUPPLY_PROP_CURRENT_AVG,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
#ifdef WPC_CHECK_CVPRM_FEATURE
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
#endif
};

static void max77803_charger_initialize(struct max77803_charger_data *charger);
static int max77803_get_vbus_state(struct max77803_charger_data *charger);
static int max77803_get_charger_state(struct max77803_charger_data *charger);

/*
static void max77803_dump_reg(struct max77803_charger_data *charger)
{
	u8 reg_data;
	u32 reg_addr;
	pr_info("%s\n", __func__);

	for (reg_addr = 0xB0; reg_addr <= 0xC5; reg_addr++) {
		max77803_read_reg(charger->max77803->i2c, reg_addr, &reg_data);
		pr_info("max77803: c: 0x%02x(0x%02x)\n", reg_addr, reg_data);
	}
}
*/
static bool max77803_charger_unlock(struct max77803_charger_data *chg_data)
{
	struct i2c_client *i2c = chg_data->max77803->i2c;
	u8 reg_data;
	u8 chgprot;
	int retry_cnt = 0;
	bool need_init = false;

	do {
		max77803_read_reg(i2c, MAX77803_CHG_REG_CHG_CNFG_06, &reg_data);
		chgprot = ((reg_data & 0x0C) >> 2);
		if (chgprot != 0x03) {
			pr_err("%s: unlock err, chgprot(0x%x), retry(%d)\n",
					__func__, chgprot, retry_cnt);
			max77803_write_reg(i2c, MAX77803_CHG_REG_CHG_CNFG_06,
				(0x03 << 2));
			need_init = true;
			msleep(20);
		} else {
			pr_debug("%s: unlock success, chgprot(0x%x)\n",
				__func__, chgprot);
			break;
		}
	} while ((chgprot != 0x03) && (++retry_cnt < 10));

	return need_init;
}

static void check_charger_unlock_state(struct max77803_charger_data *chg_data)
{
	bool need_reg_init;
	pr_debug("%s\n", __func__);

	need_reg_init = max77803_charger_unlock(chg_data);
	if (need_reg_init) {
		pr_err("%s: charger locked state, reg init\n", __func__);
		max77803_charger_initialize(chg_data);
	}
}

static int max77803_get_battery_present(struct max77803_charger_data *charger)
{
	u8 reg_data;

	if (max77803_read_reg(charger->max77803->i2c,
			MAX77803_CHG_REG_CHG_INT_OK, &reg_data) < 0) {
		/* Eventhough there is an error,
		   don't do power-off */
		return 1;
	}

	pr_debug("%s: CHG_INT_OK(0x%02x)\n", __func__, reg_data);
#if defined(CONFIG_CHARGER_MAX77803)
	reg_data = ((reg_data & MAX77803_BATP_OK) >> MAX77803_BATP_OK_SHIFT);
#else
	reg_data = ((reg_data & MAX77803_DETBAT) >> MAX77803_DETBAT_SHIFT);
#endif
	return reg_data;
}

static void max77803_set_charger_state(struct max77803_charger_data *charger,
		int enable)
{
	u8 reg_data;

	max77803_read_reg(charger->max77803->i2c,
			MAX77803_CHG_REG_CHG_CNFG_00, &reg_data);

	if (enable)
		reg_data |= MAX77803_MODE_CHGR;
	else
		reg_data &= ~MAX77803_MODE_CHGR;

	pr_debug("%s: CHG_CNFG_00(0x%02x)\n", __func__, reg_data);
	max77803_write_reg(charger->max77803->i2c,
			MAX77803_CHG_REG_CHG_CNFG_00, reg_data);
}

static void max77803_set_buck(struct max77803_charger_data *charger,
		int enable)
{
	u8 reg_data;

	max77803_read_reg(charger->max77803->i2c,
		MAX77803_CHG_REG_CHG_CNFG_00, &reg_data);

	if (enable)
		reg_data |= MAX77803_MODE_BUCK;
	else
		reg_data &= ~MAX77803_MODE_BUCK;

	pr_debug("%s: CHG_CNFG_00(0x%02x)\n", __func__, reg_data);
	max77803_write_reg(charger->max77803->i2c,
		MAX77803_CHG_REG_CHG_CNFG_00, reg_data);
}

#ifdef WPC_CHECK_CVPRM_FEATURE
static void max77803_check_cvprm(struct max77803_charger_data *charger, u8 data)
{
	u8 reg_data;

	max77803_read_reg(charger->max77803->i2c,
			MAX77803_CHG_REG_CHG_CNFG_04, &reg_data);
	if ((reg_data & 0x1f) != data) {
		reg_data &= ~(0x1f << 0);
		reg_data |= (data << 0);
		max77803_write_reg(charger->max77803->i2c,
				MAX77803_CHG_REG_CHG_CNFG_04, reg_data);
	}
}

static int max77803_get_charge_voltage(struct max77803_charger_data *charger)
{
	u8 reg_data;
	int charge_voltage;

	max77803_read_reg(charger->max77803->i2c,
			MAX77803_CHG_REG_CHG_CNFG_04, &reg_data);

	reg_data &= 0x1f;
	if (reg_data == 0x1c) {
		charge_voltage = 4340;
	} else {
		if (reg_data > 0x1c) {
			reg_data -= 1;
		}
		charge_voltage = (reg_data * 25) + 3650;
	}

	return charge_voltage;
}
#endif

static void max77803_check_slow_charging(struct max77803_charger_data *charger, int set_current_reg)
{
	/* under 500mA, slow rate */
	if (set_current_reg <= (SLOW_CHARGING_CURRENT_STANDARD / charger->input_curr_limit_step) &&
			(charger->cable_type != POWER_SUPPLY_TYPE_BATTERY)) {
		charger->aicl_on = true;
		pr_info("%s: slow charging on : set_current_reg(0x%02x), cable type(%d)\n", __func__, set_current_reg, charger->cable_type);
	}
	else
		charger->aicl_on = false;
}

extern unsigned int system_rev;
static void max77803_change_charge_path(struct max77803_charger_data *charger,
		int path)
{
	u8 cnfg12, ctrl3;

	if (path == POWER_SUPPLY_TYPE_WIRELESS) {
		cnfg12 = (0 << CHG_CNFG_12_CHGINSEL_SHIFT);
		ctrl3 = (1 << CTRL3_JIGSET_SHIFT);
	} else {
		cnfg12 = (1 << CHG_CNFG_12_CHGINSEL_SHIFT);
		ctrl3 = (0 << CTRL3_JIGSET_SHIFT);
	}
//#if defined(CONFIG_MACH_HLTEVZW) || defined(CONFIG_MACH_HLTESPR) || defined(CONFIG_MACH_HLTEUSC)
	if (system_rev == 0)
		ctrl3 = (1 << CTRL3_JIGSET_SHIFT);
//#endif
	if (charger->pmic_ver == 0x04)
		max77803_update_reg(charger->max77803->muic, MAX77803_MUIC_REG_CTRL3,
				ctrl3,	CTRL3_JIGSET_MASK);
	max77803_update_reg(charger->max77803->i2c, MAX77803_CHG_REG_CHG_CNFG_12,
			cnfg12,	CHG_CNFG_12_CHGINSEL_MASK);
}

static void max77803_set_input_current(struct max77803_charger_data *charger,
		int cur)
{
	int set_current_reg, now_current_reg;
	int vbus_state, curr_step, delay;
	u8 set_reg, reg_data;
	int chg_state;

	mutex_lock(&charger->ops_lock);
	reg_data = (1 << CHGIN_SHIFT);
	max77803_update_reg(charger->max77803->i2c, MAX77803_CHG_REG_CHG_INT_MASK, reg_data,
			CHGIN_MASK);

	if (charger->cable_type == POWER_SUPPLY_TYPE_WIRELESS) {
		set_reg = MAX77803_CHG_REG_CHG_CNFG_10;
		charger->input_curr_limit_step = 20;
	} else {
		set_reg = MAX77803_CHG_REG_CHG_CNFG_09;
		if (charger->pmic_ver == 0x04)
			charger->input_curr_limit_step = 25;
		else
			charger->input_curr_limit_step = 20;
	}

	if (cur <= 0) {
		/* disable only buck because power onoff test issue */
		max77803_write_reg(charger->max77803->i2c,
			set_reg, 0x19);
		max77803_set_buck(charger, DISABLE);
		goto exit;
	} else {
		max77803_change_charge_path(charger, charger->cable_type);
		max77803_set_buck(charger, ENABLE);
	}

	set_current_reg = cur / charger->input_curr_limit_step;
	if (charger->cable_type == POWER_SUPPLY_TYPE_BATTERY)
		goto set_input_current;

	max77803_read_reg(charger->max77803->i2c,
		set_reg, &reg_data);
	if (reg_data == set_current_reg) {
		/* check uvlo  */
		while((set_current_reg > (MINIMUM_INPUT_CURRENT / charger->input_curr_limit_step)) && (set_current_reg < 255)) {
			vbus_state = max77803_get_vbus_state(charger);
			if (((vbus_state == 0x00) || (vbus_state == 0x01)) &&
				(charger->cable_type != POWER_SUPPLY_TYPE_WIRELESS)) {
				/* UVLO */
				set_current_reg -= 5;
				if (set_current_reg < (MINIMUM_INPUT_CURRENT / charger->input_curr_limit_step))
					set_current_reg = (MINIMUM_INPUT_CURRENT / charger->input_curr_limit_step);
				max77803_write_reg(charger->max77803->i2c,
						set_reg, set_current_reg);
				pr_info("%s: set_current_reg(0x%02x)\n", __func__, set_current_reg);
				chg_state = max77803_get_charger_state(charger);
				if ((chg_state != POWER_SUPPLY_STATUS_CHARGING) &&
						(chg_state != POWER_SUPPLY_STATUS_FULL))
					break;
				msleep(50);
			} else
				break;
		}
		goto exit;
	}

	if (reg_data == 0) {
		now_current_reg = SOFT_CHG_START_CURR / charger->input_curr_limit_step;
		max77803_write_reg(charger->max77803->i2c,
			set_reg, now_current_reg);
		msleep(SOFT_CHG_START_DUR);
	} else
		now_current_reg = reg_data;

	if (cur <= 1000) {
		curr_step = 1;
		delay = 50;
	} else {
		curr_step = SOFT_CHG_CURR_STEP / charger->input_curr_limit_step;
		delay = SOFT_CHG_STEP_DUR;
	}
	now_current_reg += (curr_step);

	while (now_current_reg < set_current_reg &&
			charger->cable_type != POWER_SUPPLY_TYPE_BATTERY)
	{
		now_current_reg = min(now_current_reg, set_current_reg);
		max77803_write_reg(charger->max77803->i2c,
			set_reg, now_current_reg);
		msleep(delay);

		vbus_state = max77803_get_vbus_state(charger);
		if (((vbus_state == 0x00) || (vbus_state == 0x01)) &&
				(charger->cable_type != POWER_SUPPLY_TYPE_WIRELESS)) {
			/* UVLO */
			if (now_current_reg > (curr_step * 3))
				now_current_reg -= (curr_step * 3);
			/* current limit 300mA */
			if (now_current_reg < (MINIMUM_INPUT_CURRENT / charger->input_curr_limit_step))
				now_current_reg = (MINIMUM_INPUT_CURRENT / charger->input_curr_limit_step);
			curr_step /= 2;
			max77803_write_reg(charger->max77803->i2c,
					set_reg, now_current_reg);
			pr_info("%s: now_current_reg(0x%02x)\n", __func__, now_current_reg);
			chg_state = max77803_get_charger_state(charger);
			if ((chg_state != POWER_SUPPLY_STATUS_CHARGING) &&
					(chg_state != POWER_SUPPLY_STATUS_FULL))
				goto exit;
			if (curr_step < 2) {
				goto exit;
			}
			msleep(50);
		} else
			now_current_reg += (curr_step);
	}

set_input_current:
	pr_info("%s: reg_data(0x%02x), input(%d)\n",
		__func__, set_current_reg, cur);
	max77803_write_reg(charger->max77803->i2c,
		set_reg, set_current_reg);
exit:
	/* slow charging check */
	max77803_read_reg(charger->max77803->i2c,
		set_reg, &reg_data);
	max77803_check_slow_charging(charger, reg_data);

	reg_data = (0 << CHGIN_SHIFT);
	max77803_update_reg(charger->max77803->i2c, MAX77803_CHG_REG_CHG_INT_MASK, reg_data,
			CHGIN_MASK);
	mutex_unlock(&charger->ops_lock);
}

static int max77803_get_input_current(struct max77803_charger_data *charger)
{
	u8 reg_data;
	int get_current = 0;

	if (charger->cable_type == POWER_SUPPLY_TYPE_WIRELESS) {
		max77803_read_reg(charger->max77803->i2c,
				MAX77803_CHG_REG_CHG_CNFG_10, &reg_data);
		pr_info("%s: CHG_CNFG_10(0x%02x)\n", __func__, reg_data);
		/* AND operation for removing the formal 2bit  */
		reg_data = reg_data & 0x3F;
		charger->input_curr_limit_step = 20;
	} else {
		max77803_read_reg(charger->max77803->i2c,
				MAX77803_CHG_REG_CHG_CNFG_09, &reg_data);
		pr_info("%s: CHG_CNFG_09(0x%02x)\n", __func__, reg_data);
		/* AND operation for removing the formal 1bit  */
		reg_data = reg_data & 0x7F;
		if (charger->pmic_ver == 0x04)
			charger->input_curr_limit_step = 25;
		else
			charger->input_curr_limit_step = 20;
	}
	get_current = reg_data * charger->input_curr_limit_step;

	pr_debug("%s: get input current: %dmA\n", __func__, get_current);
	return get_current;
}

static void max77803_set_topoff_current(struct max77803_charger_data *charger,
		int cur, int timeout)
{
	u8 reg_data;

	if (cur >= 350)
		reg_data = 0x07;
	else if (cur >= 300)
		reg_data = 0x06;
	else if (cur >= 250)
		reg_data = 0x05;
	else if (cur >= 200)
		reg_data = 0x04;
	else if (cur >= 175)
		reg_data = 0x03;
	else if (cur >= 150)
		reg_data = 0x02;
	else if (cur >= 125)
		reg_data = 0x01;
	else
		reg_data = 0x00;

	/* the unit of timeout is second*/
	timeout = timeout / 60;
	reg_data |= ((timeout / 10) << 3);
	pr_info("%s: reg_data(0x%02x), topoff(%d)\n", __func__, reg_data, cur);

	max77803_write_reg(charger->max77803->i2c,
		MAX77803_CHG_REG_CHG_CNFG_03, reg_data);
}

static void max77803_set_charge_current(struct max77803_charger_data *charger,
		int cur)
{
	u8 reg_data = 0;

	max77803_read_reg(charger->max77803->i2c,
		MAX77803_CHG_REG_CHG_CNFG_02, &reg_data);
	reg_data &= ~MAX77803_CHG_CC;

	if (!cur) {
		/* No charger */
		max77803_write_reg(charger->max77803->i2c,
				MAX77803_CHG_REG_CHG_CNFG_02, reg_data);
	} else {
		reg_data |= ((cur * 10 / charger->charging_curr_step) << 0);
		max77803_write_reg(charger->max77803->i2c,
				MAX77803_CHG_REG_CHG_CNFG_02, reg_data);
	}
	pr_info("%s: reg_data(0x%02x), charge(%d)\n",
			__func__, reg_data, cur);
}

/*
static int max77803_get_charge_current(struct max77803_charger_data *charger)
{
	u8 reg_data;
	int get_current;

	max77803_read_reg(charger->max77803->i2c,
		MAX77803_CHG_REG_CHG_CNFG_02, &reg_data);
	pr_debug("%s: CHG_CNFG_02(0x%02x)\n", __func__, reg_data);

	reg_data &= MAX77803_CHG_CC;
	get_current = reg_data * charger->charging_curr_step / 10;

	pr_debug("%s: get charge current: %dmA\n", __func__, get_current);
	return get_current;
}
*/

/* in soft regulation, current recovery operation */
static void max77803_recovery_work(struct work_struct *work)
{
	struct max77803_charger_data *chg_data = container_of(work,
						struct max77803_charger_data,
						recovery_work.work);
	u8 dtls_00, chgin_dtls;
	u8 dtls_01, chg_dtls;
	u8 dtls_02, byp_dtls;
	pr_debug("%s\n", __func__);

	__pm_relax(&chg_data->recovery_ws);
	if ((!chg_data->is_charging) || mutex_is_locked(&chg_data->ops_lock) ||
			(chg_data->cable_type != POWER_SUPPLY_TYPE_MAINS))
		return;
	max77803_read_reg(chg_data->max77803->i2c,
				MAX77803_CHG_REG_CHG_DTLS_00, &dtls_00);
	max77803_read_reg(chg_data->max77803->i2c,
				MAX77803_CHG_REG_CHG_DTLS_01, &dtls_01);
	max77803_read_reg(chg_data->max77803->i2c,
				MAX77803_CHG_REG_CHG_DTLS_02, &dtls_02);

	chgin_dtls = ((dtls_00 & MAX77803_CHGIN_DTLS) >>
				MAX77803_CHGIN_DTLS_SHIFT);
	chg_dtls = ((dtls_01 & MAX77803_CHG_DTLS) >>
				MAX77803_CHG_DTLS_SHIFT);
	byp_dtls = ((dtls_02 & MAX77803_BYP_DTLS) >>
				MAX77803_BYP_DTLS_SHIFT);

	if ((chg_data->soft_reg_recovery_cnt < RECOVERY_CNT) && (
		(chgin_dtls == 0x3) && (chg_dtls != 0x8) && (byp_dtls == 0x0))) {
		pr_info("%s: try to recovery, cnt(%d)\n", __func__,
				(chg_data->soft_reg_recovery_cnt + 1));
		if (chg_data->siop_level < 100 &&
			chg_data->cable_type == POWER_SUPPLY_TYPE_MAINS) {
			pr_info("%s : LCD on status and recover current\n", __func__);
			max77803_set_input_current(chg_data,
					SIOP_INPUT_LIMIT_CURRENT);
		} else {
			max77803_set_input_current(chg_data,
				chg_data->charging_current_max);
		}
	} else {
		pr_info("%s: fail to recovery, cnt(%d)\n", __func__,
				(chg_data->soft_reg_recovery_cnt + 1));

		pr_info("%s:  CHGIN(0x%x), CHG(0x%x), BYP(0x%x)\n",
				__func__, chgin_dtls, chg_dtls, byp_dtls);

		/* schedule softreg recovery wq */
		if (chg_data->soft_reg_recovery_cnt < RECOVERY_CNT) {
			__pm_stay_awake(&chg_data->recovery_ws);
			queue_delayed_work(chg_data->wqueue, &chg_data->recovery_work,
				msecs_to_jiffies(RECOVERY_DELAY));
		} else {
			pr_info("%s: recovery cnt(%d) is over\n",
				__func__, RECOVERY_CNT);
		}
	}

	/* add recovery try count */
	chg_data->soft_reg_recovery_cnt++;
}

static void reduce_input_current(struct max77803_charger_data *charger, int cur)
{
	u8 set_reg;
	u8 set_value;
	unsigned int min_input_current = 0;

	if ((!charger->is_charging) || mutex_is_locked(&charger->ops_lock) ||
		(charger->cable_type == POWER_SUPPLY_TYPE_WIRELESS))
		return;
	set_reg = MAX77803_CHG_REG_CHG_CNFG_09;
	min_input_current = MINIMUM_INPUT_CURRENT;
	if (charger->pmic_ver == 0x04)
		charger->input_curr_limit_step = 25;
	else
		charger->input_curr_limit_step = 20;

	if (!max77803_read_reg(charger->max77803->i2c,
				set_reg, &set_value)) {
		if ((set_value <= (min_input_current / charger->input_curr_limit_step)) ||
		    (set_value <= (cur / charger->input_curr_limit_step)))
			return;
		set_value -= (cur / charger->input_curr_limit_step);
		set_value = (set_value < (min_input_current / charger->input_curr_limit_step)) ?
			(min_input_current / charger->input_curr_limit_step) : set_value;
		max77803_write_reg(charger->max77803->i2c,
				set_reg, set_value);
		pr_info("%s: set current: reg:(0x%x), val:(0x%x)\n",
				__func__, set_reg, set_value);
	}
	if(charger->cable_type == POWER_SUPPLY_TYPE_MAINS) {
		/* schedule softreg recovery wq */
		cancel_delayed_work_sync(&charger->recovery_work);
		__pm_stay_awake(&charger->recovery_ws);
		queue_delayed_work(charger->wqueue, &charger->recovery_work,
				msecs_to_jiffies(RECOVERY_DELAY));
	}
}

static int max77803_get_vbus_state(struct max77803_charger_data *charger)
{
	u8 reg_data;
	union power_supply_propval value;

	max77803_read_reg(charger->max77803->i2c,
		MAX77803_CHG_REG_CHG_DTLS_00, &reg_data);

	psy_do_property("battery", get, POWER_SUPPLY_PROP_ONLINE,
			value);

	if (value.intval  == POWER_SUPPLY_TYPE_WIRELESS)
		reg_data = ((reg_data & MAX77803_WCIN_DTLS) >>
			MAX77803_WCIN_DTLS_SHIFT);
	else
		reg_data = ((reg_data & MAX77803_CHGIN_DTLS) >>
			MAX77803_CHGIN_DTLS_SHIFT);

	switch (reg_data) {
	case 0x00:
		pr_info("%s: VBUS is invalid. CHGIN < CHGIN_UVLO\n",
			__func__);
		break;
	case 0x01:
		pr_info("%s: VBUS is invalid. CHGIN < MBAT+CHGIN2SYS" \
			"and CHGIN > CHGIN_UVLO\n", __func__);
		break;
	case 0x02:
		pr_info("%s: VBUS is invalid. CHGIN > CHGIN_OVLO",
			__func__);
		break;
	case 0x03:
		break;
	default:
		break;
	}

	return reg_data;
}

static int max77803_get_charger_state(struct max77803_charger_data *charger)
{
	int state;
	u8 reg_data;

	max77803_read_reg(charger->max77803->i2c,
		MAX77803_CHG_REG_CHG_DTLS_01, &reg_data);
	reg_data = ((reg_data & MAX77803_CHG_DTLS) >> MAX77803_CHG_DTLS_SHIFT);
	pr_info("%s: CHG_DTLS : 0x%2x\n", __func__, reg_data);

	switch (reg_data) {
	case 0x0:
	case 0x1:
	case 0x2:
		state = POWER_SUPPLY_STATUS_CHARGING;
		break;
	case 0x3:
	case 0x4:
		state = POWER_SUPPLY_STATUS_FULL;
		break;
	case 0x5:
	case 0x6:
	case 0x7:
		state = POWER_SUPPLY_STATUS_NOT_CHARGING;
		break;
	case 0x8:
	case 0xA:
	case 0xB:
		state = POWER_SUPPLY_STATUS_DISCHARGING;
		break;
	default:
		state = POWER_SUPPLY_STATUS_UNKNOWN;
		break;
	}

	return state;
}

static int max77803_get_health_state(struct max77803_charger_data *charger)
{
	int state;
	int vbus_state;
	int retry_cnt;
	u8 chg_dtls_00, chg_dtls, reg_data;
	u8 chg_cnfg_00, chg_cnfg_01 ,chg_cnfg_02, chg_cnfg_04, chg_cnfg_09, chg_cnfg_12;

	max77803_read_reg(charger->max77803->i2c,
		MAX77803_CHG_REG_CHG_DTLS_01, &reg_data);
	reg_data = ((reg_data & MAX77803_BAT_DTLS) >> MAX77803_BAT_DTLS_SHIFT);

	pr_info("%s: reg_data(0x%x)\n", __func__, reg_data);
	switch (reg_data) {
	case 0x00:
		pr_info("%s: No battery and the charger is suspended\n",
			__func__);
		state = POWER_SUPPLY_HEALTH_UNSPEC_FAILURE;
		break;
	case 0x01:
		pr_info("%s: battery is okay "
			"but its voltage is low(~VPQLB)\n", __func__);
		state = POWER_SUPPLY_HEALTH_GOOD;
		break;
	case 0x02:
		pr_info("%s: battery dead\n", __func__);
		state = POWER_SUPPLY_HEALTH_DEAD;
		break;
	case 0x03:
		state = POWER_SUPPLY_HEALTH_GOOD;
		break;
	case 0x04:
		pr_info("%s: battery is okay" \
			"but its voltage is low\n", __func__);
		state = POWER_SUPPLY_HEALTH_GOOD;
		break;
	case 0x05:
		pr_info("%s: battery ovp\n", __func__);
		state = POWER_SUPPLY_HEALTH_OVERVOLTAGE;
		break;
	default:
		pr_info("%s: battery unknown : 0x%d\n", __func__, reg_data);
		state = POWER_SUPPLY_HEALTH_UNKNOWN;
		break;
	}

	if (state == POWER_SUPPLY_HEALTH_GOOD) {
		union power_supply_propval value;
		psy_do_property("battery", get,
				POWER_SUPPLY_PROP_HEALTH, value);
		/* VBUS OVP state return battery OVP state */
		vbus_state = max77803_get_vbus_state(charger);
		/* read CHG_DTLS and detecting battery terminal error */
		max77803_read_reg(charger->max77803->i2c,
				MAX77803_CHG_REG_CHG_DTLS_01, &chg_dtls);
		chg_dtls = ((chg_dtls & MAX77803_CHG_DTLS) >>
				MAX77803_CHG_DTLS_SHIFT);
		max77803_read_reg(charger->max77803->i2c,
				MAX77803_CHG_REG_CHG_CNFG_00, &chg_cnfg_00);

		/* print the log at the abnormal case */
		if((charger->is_charging == 1) && (chg_dtls & 0x08)) {
			max77803_read_reg(charger->max77803->i2c,
				MAX77803_CHG_REG_CHG_DTLS_00, &chg_dtls_00);
			max77803_read_reg(charger->max77803->i2c,
				MAX77803_CHG_REG_CHG_CNFG_01, &chg_cnfg_01);
			max77803_read_reg(charger->max77803->i2c,
				MAX77803_CHG_REG_CHG_CNFG_02, &chg_cnfg_02);
		max77803_read_reg(charger->max77803->i2c,
				MAX77803_CHG_REG_CHG_CNFG_04, &chg_cnfg_04);
			max77803_read_reg(charger->max77803->i2c,
					MAX77803_CHG_REG_CHG_CNFG_09, &chg_cnfg_09);
			max77803_read_reg(charger->max77803->i2c,
					MAX77803_CHG_REG_CHG_CNFG_12, &chg_cnfg_12);

			pr_info("%s: CHG_DTLS_00(0x%x), CHG_DTLS_01(0x%x), CHG_CNFG_00(0x%x)\n",
					__func__, chg_dtls_00, chg_dtls, chg_cnfg_00);
			pr_info("%s:  CHG_CNFG_01(0x%x), CHG_CNFG_02(0x%x), CHG_CNFG_04(0x%x)\n",
					__func__, chg_cnfg_01, chg_cnfg_02, chg_cnfg_04);
			pr_info("%s:  CHG_CNFG_09(0x%x), CHG_CNFG_12(0x%x)\n",
					__func__, chg_cnfg_09, chg_cnfg_12);
		}

		pr_info("%s: vbus_state : 0x%d, chg_dtls : 0x%d\n", __func__, vbus_state, chg_dtls);
		/*  OVP is higher priority */
		if (vbus_state == 0x02) { /*  CHGIN_OVLO */
			pr_info("%s: vbus ovp\n", __func__);
			state = POWER_SUPPLY_HEALTH_OVERVOLTAGE;
			if (charger->cable_type == POWER_SUPPLY_TYPE_WIRELESS) {
				retry_cnt = 0;
				do {
					msleep(50);
					vbus_state = max77803_get_vbus_state(charger);
				} while((retry_cnt++ < 2) && (vbus_state == 0x02));
				if (vbus_state == 0x02) {
					state = POWER_SUPPLY_HEALTH_OVERVOLTAGE;
					pr_info("%s: wpc and over-voltage\n", __func__);
				} else
					state = POWER_SUPPLY_HEALTH_GOOD;
			}
		} else if (((vbus_state == 0x0) || (vbus_state == 0x01)) &&(chg_dtls & 0x08) && \
				(chg_cnfg_00 & MAX77803_MODE_BUCK) && \
				(chg_cnfg_00 & MAX77803_MODE_CHGR) && \
				(charger->cable_type != POWER_SUPPLY_TYPE_WIRELESS)) {
			pr_info("%s: vbus is under\n", __func__);
			state = POWER_SUPPLY_HEALTH_UNDERVOLTAGE;
		} else if((value.intval == POWER_SUPPLY_HEALTH_UNDERVOLTAGE) && \
				!((vbus_state == 0x0) || (vbus_state == 0x01))){
			max77803_set_input_current(charger,
					charger->charging_current_max);
		}
	}

	return state;
}

static int sec_chg_get_property(struct power_supply *psy,
		enum power_supply_property psp,
		union power_supply_propval *val)
{
	struct max77803_charger_data *charger =
		container_of(psy, struct max77803_charger_data, psy_chg);
	u8 reg_data;

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = POWER_SUPPLY_TYPE_BATTERY;
		if (max77803_read_reg(charger->max77803->i2c,
			MAX77803_CHG_REG_CHG_INT_OK, &reg_data) == 0) {
			pr_info("%s: reg_data(%d)\n", __func__, reg_data);
			if (reg_data & MAX77803_WCIN_OK) {
				val->intval = POWER_SUPPLY_TYPE_WIRELESS;
				charger->wc_w_state = 1;
			} else if (reg_data & MAX77803_CHGIN_OK) {
				val->intval = POWER_SUPPLY_TYPE_MAINS;
			}
		}
		break;
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = max77803_get_charger_state(charger);
		break;
	case POWER_SUPPLY_PROP_HEALTH:
		val->intval = max77803_get_health_state(charger);
		break;
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		// AOSP expects the charging current to be in microamperes
		// frameworks/base/core/java/android/os/BatteryManager.java L256
		val->intval = charger->charging_current_max * 1000;
		break;
	case POWER_SUPPLY_PROP_CURRENT_AVG:
		// AOSP expects the charging current to be in microamperes
		// frameworks/base/core/java/android/os/BatteryManager.java L263
		val->intval = charger->charging_current * 1000;
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		val->intval = max77803_get_input_current(charger);
		break;
	case POWER_SUPPLY_PROP_CHARGE_TYPE:
		if (!charger->is_charging)
			val->intval = POWER_SUPPLY_CHARGE_TYPE_NONE;
		else if (charger->aicl_on)
		{
			val->intval = POWER_SUPPLY_CHARGE_TYPE_SLOW;
			pr_info("%s: slow-charging mode\n", __func__);
		}
		else
			val->intval = POWER_SUPPLY_CHARGE_TYPE_FAST;
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = max77803_get_battery_present(charger);
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
		break;
#ifdef WPC_CHECK_CVPRM_FEATURE
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		val->intval = max77803_get_charge_voltage(charger);
		break;
#endif
	default:
		return -EINVAL;
	}

	return 0;
}

static int sec_chg_set_property(struct power_supply *psy,
		enum power_supply_property psp,
		const union power_supply_propval *val)
{
	struct max77803_charger_data *charger =
		container_of(psy, struct max77803_charger_data, psy_chg);
	union power_supply_propval value;
	int set_charging_current, set_charging_current_max;
	const int usb_charging_current = charger->pdata->charging_current[
		POWER_SUPPLY_TYPE_USB].fast_charging_current;
	u8 chg_cnfg_00;

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		charger->status = val->intval;
		break;
	/* val->intval : type */
	case POWER_SUPPLY_PROP_ONLINE:
		/* check and unlock */
		check_charger_unlock_state(charger);

		if (val->intval == POWER_SUPPLY_TYPE_POWER_SHARING) {
			psy_do_property("ps", get,
					POWER_SUPPLY_PROP_STATUS, value);
			chg_cnfg_00 = CHG_CNFG_00_OTG_MASK
				| CHG_CNFG_00_BOOST_MASK
				| CHG_CNFG_00_DIS_MUIC_CTRL_MASK;
			if (value.intval) {
				max77803_update_reg(charger->max77803->i2c, MAX77803_CHG_REG_CHG_CNFG_00,
						chg_cnfg_00, chg_cnfg_00);
				pr_info("%s: ps enable\n", __func__);
			} else {
				max77803_update_reg(charger->max77803->i2c, MAX77803_CHG_REG_CHG_CNFG_00,
						0, chg_cnfg_00);
				pr_info("%s: ps disable\n", __func__);
			}
			break;
		}

		charger->cable_type = val->intval;
		psy_do_property("battery", get,
				POWER_SUPPLY_PROP_HEALTH, value);
		if (val->intval == POWER_SUPPLY_TYPE_BATTERY) {
			charger->is_charging = false;
			charger->aicl_on = false;
			charger->soft_reg_recovery_cnt = 0;
			set_charging_current = 0;
			set_charging_current_max =
				charger->pdata->charging_current[
				POWER_SUPPLY_TYPE_USB].input_current_limit;
		} else {
			charger->is_charging = true;
			charger->charging_current_max =
					charger->pdata->charging_current
					[charger->cable_type].input_current_limit;
			charger->charging_current =
					charger->pdata->charging_current
					[charger->cable_type].fast_charging_current;
#ifdef CONFIG_FORCE_FAST_CHARGE
			/* Yank555 : Use Fast charge currents accroding to user settings */
			if (force_fast_charge == FAST_CHARGE_FORCE_AC) {/* We are in basic Fast Charge mode, so we substitute AC to USB levels */
				switch(charger->cable_type) {
					case POWER_SUPPLY_TYPE_USB:	/* These are low current USB connections, apply usual 1A/h AC levels to USB */
					case POWER_SUPPLY_TYPE_USB_ACA:
					case POWER_SUPPLY_TYPE_CARDOCK:
					case POWER_SUPPLY_TYPE_OTG:	charger->charging_current_max = USB_CHARGE_1000;
									charger->charging_current     = USB_CHARGE_1000;
									break;
					default:			/* Don't do anything for any other kind of connections and don't touch when type is unknown */
									break;
				}
			} else if (force_fast_charge == FAST_CHARGE_FORCE_CUSTOM_MA) { /* We are in custom current Fast Charge mode for both AC and USB */
				switch(charger->cable_type) {
					case POWER_SUPPLY_TYPE_USB:
					case POWER_SUPPLY_TYPE_USB_DCP:
					case POWER_SUPPLY_TYPE_USB_CDP:
					case POWER_SUPPLY_TYPE_USB_ACA:
					case POWER_SUPPLY_TYPE_CARDOCK:
					case POWER_SUPPLY_TYPE_OTG:	/* These are USB connections, apply custom USB current for all of them */
									charger->charging_current_max = usb_charge_level;
									charger->charging_current     = usb_charge_level;
									break;
					case POWER_SUPPLY_TYPE_MAINS:	/* These are AC connections, apply custom AC current for all of them */
									charger->charging_current_max = ac_charge_level;
									charger->charging_current     = min(ac_charge_level+300, MAX_CHARGE_LEVEL); /* Keep the 300mA/h delta, but never go above 2.1A/h */
									break;
					default:			/* Don't do anything for any other kind of connections and don't touch when type is unknown */
									break;
				}
			}
#endif // CONFIG_FORCE_FAST_CHARGE
			/* decrease the charging current according to siop level */
			set_charging_current =
				charger->charging_current * charger->siop_level / 100;
			if (set_charging_current > 0 &&
					set_charging_current < usb_charging_current)
				set_charging_current = usb_charging_current;

				set_charging_current_max =
						charger->charging_current_max;
#ifdef WPC_CHECK_CVPRM_FEATURE
			if (val->intval == POWER_SUPPLY_TYPE_WIRELESS)
				max77803_check_cvprm(charger, 0x1C);
			else
				max77803_check_cvprm(charger, 0x1D);
#endif

			if (charger->siop_level < 100 &&
				val->intval == POWER_SUPPLY_TYPE_MAINS) {
				set_charging_current_max = SIOP_INPUT_LIMIT_CURRENT;
				if (set_charging_current > SIOP_CHARGING_LIMIT_CURRENT)
					set_charging_current = SIOP_CHARGING_LIMIT_CURRENT;
			}
		}
		max77803_set_charger_state(charger, charger->is_charging);
		/* if battery full, only disable charging  */
		if ((charger->status == POWER_SUPPLY_STATUS_CHARGING) ||
				(charger->status == POWER_SUPPLY_STATUS_DISCHARGING) ||
				(value.intval == POWER_SUPPLY_HEALTH_UNSPEC_FAILURE)) {
			/* current setting */
			max77803_set_charge_current(charger,
				set_charging_current);
			/* if battery is removed, disable input current and reenable input current
			  *  to enable buck always */
			if (value.intval == POWER_SUPPLY_HEALTH_UNSPEC_FAILURE)
				max77803_set_input_current(charger, 0);
			else
				max77803_set_input_current(charger,
						set_charging_current_max);
			max77803_set_topoff_current(charger,
				charger->pdata->charging_current[
				val->intval].full_check_current_1st,
				charger->pdata->charging_current[
				val->intval].full_check_current_2nd);
		}
		break;
	/* val->intval : input charging current */
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		charger->charging_current_max = val->intval;
		break;
	/*  val->intval : charging current */
	case POWER_SUPPLY_PROP_CURRENT_AVG:
		charger->charging_current = val->intval;
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		max77803_set_charge_current(charger,
				val->intval);
		max77803_set_input_current(charger,
				val->intval);
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
		charger->siop_level = val->intval;
		if (charger->is_charging) {
			/* decrease the charging current according to siop level */
			int current_now =
				charger->charging_current * val->intval / 100;

			/* do forced set charging current */
			if (current_now > 0 &&
					current_now < usb_charging_current)
				current_now = usb_charging_current;

			if (charger->cable_type == POWER_SUPPLY_TYPE_MAINS) {
				if (charger->siop_level < 100 ) {
					set_charging_current_max = SIOP_INPUT_LIMIT_CURRENT;
				} else {
					set_charging_current_max =
						charger->charging_current_max;
				}

				if (charger->siop_level < 100 &&
						current_now > SIOP_CHARGING_LIMIT_CURRENT)
					current_now = SIOP_CHARGING_LIMIT_CURRENT;
				max77803_set_input_current(charger,
					set_charging_current_max);
			}

			max77803_set_charge_current(charger, current_now);
		}
		break;
#if defined(CONFIG_SAMSUNG_BATTERY_ENG_TEST)
	case POWER_SUPPLY_PROP_CHARGE_TYPE:
	{
		u8 ctrl3, cnfg12;
		if(val->intval == POWER_SUPPLY_TYPE_WIRELESS) {
			cnfg12 = (0 << CHG_CNFG_12_CHGINSEL_SHIFT);
			ctrl3 = (1 << CTRL3_JIGSET_SHIFT);
			if (charger->cable_type == POWER_SUPPLY_TYPE_WIRELESS) {
#ifdef CONFIG_FORCE_FAST_CHARGE
				/* Yank555 : Use Fast charge currents accroding to user settings */
				if (force_fast_charge == FAST_CHARGE_FORCE_AC) {
					/* We are in basic Fast Charge mode, so we substitute AC to WIRELESS levels */
					charger->charging_current_max = WIRELESS_CHARGE_1000;
					charger->charging_current = WIRELESS_CHARGE_1000 + 100;
				} else if (force_fast_charge == FAST_CHARGE_FORCE_CUSTOM_MA) {
					/* We are in custom current Fast Charge mode for WIRELESS */
					charger->charging_current_max = wireless_charge_level;
					charger->charging_current = min(wireless_charge_level+100, MAX_CHARGE_LEVEL);
				}
#else
				charger->charging_current_max = 650;
				charger->charging_current = 750;
#endif // CONFIG_FORCE_FAST_CHARGE
				max77803_set_input_current(charger,
						charger->charging_current_max);
				max77803_set_charge_current(charger, charger->charging_current);
			}
		}
		else {
			cnfg12 = (1 << CHG_CNFG_12_CHGINSEL_SHIFT);
			ctrl3 = (0 << CTRL3_JIGSET_SHIFT);
		}

		max77803_update_reg(charger->max77803->muic, MAX77803_MUIC_REG_CTRL3, ctrl3,
				CTRL3_JIGSET_MASK);
		max77803_update_reg(charger->max77803->i2c, MAX77803_CHG_REG_CHG_CNFG_12, cnfg12,
				CHG_CNFG_12_CHGINSEL_MASK);

		pr_info("%s: ctrl3 : (0x%02x)\n", __func__, ctrl3);
		pr_info("%s: set CNFG_12: 0x%x\n", __func__, cnfg12);
		break;
	}
#endif
	default:
		return -EINVAL;
	}

	return 0;
}

static u8 max77803_get_float_voltage_data(
					int float_voltage)
{
	u8 data = 0x16;

	if (float_voltage >= 4400)
		data = 0x1f;
	else if (float_voltage >= 4375)
		data = 0x1e;
	else if (float_voltage >= 4350)
		data = 0x1d;
	else if (float_voltage >= 4340)
		data = 0x1c;
	else
		data = (float_voltage - 3650) / 25;
	return data;
}

static void max77803_charger_initialize(struct max77803_charger_data *charger)
{
	u8 reg_data;
	pr_debug("%s\n", __func__);

	/* unmasked: CHGIN_I, WCIN_I, BATP_I, BYP_I	*/
	max77803_write_reg(charger->max77803->i2c,
		MAX77803_CHG_REG_CHG_INT_MASK, 0x9a);

	/* unlock charger setting protect */
	reg_data = (0x03 << 2);
	max77803_write_reg(charger->max77803->i2c,
		MAX77803_CHG_REG_CHG_CNFG_06, reg_data);

	/*
	 * fast charge timer disable
	 * restart threshold disable
	 * pre-qual charge enable(default)
	 */
	reg_data = (0x0 << 0) | (0x03 << 4);
	max77803_write_reg(charger->max77803->i2c,
		MAX77803_CHG_REG_CHG_CNFG_01, reg_data);

	/*
	 * charge current 466mA(default)
	 * otg current limit 900mA
	 */
	max77803_read_reg(charger->max77803->i2c,
			MAX77803_CHG_REG_CHG_CNFG_02, &reg_data);
	reg_data |= (1 << 7);
	max77803_write_reg(charger->max77803->i2c,
		MAX77803_CHG_REG_CHG_CNFG_02, reg_data);

	/*
	 * top off current 100mA
	 * top off timer 40min
	 */
	reg_data = (0x04 << 3);
	max77803_write_reg(charger->max77803->i2c,
		MAX77803_CHG_REG_CHG_CNFG_03, reg_data);

	/*
	 * cv voltage 4.2V or 4.35V
	 * MINVSYS 3.6V(default)
	 */
	reg_data = max77803_get_float_voltage_data(charger->pdata->chg_float_voltage);
	max77803_update_reg(charger->max77803->i2c, MAX77803_CHG_REG_CHG_CNFG_04,
			(reg_data << CHG_CNFG_04_CHG_CV_PRM_SHIFT),
			CHG_CNFG_04_CHG_CV_PRM_MASK);
	max77803_read_reg(charger->max77803->i2c,
			MAX77803_CHG_REG_CHG_CNFG_04, &reg_data);
	pr_info("%s: battery cv voltage 0x%x\n", __func__, reg_data);
}

static void sec_chg_isr_work(struct work_struct *work)
{
	struct max77803_charger_data *charger =
		container_of(work, struct max77803_charger_data, isr_work.work);

	union power_supply_propval val;

	if (charger->pdata->full_check_type ==
			SEC_BATTERY_FULLCHARGED_CHGINT) {

		val.intval = max77803_get_charger_state(charger);

		switch (val.intval) {
		case POWER_SUPPLY_STATUS_DISCHARGING:
			pr_err("%s: Interrupted but Discharging\n", __func__);
			break;

		case POWER_SUPPLY_STATUS_NOT_CHARGING:
			pr_err("%s: Interrupted but NOT Charging\n", __func__);
			break;

		case POWER_SUPPLY_STATUS_FULL:
			pr_info("%s: Interrupted by Full\n", __func__);
			psy_do_property("battery", set,
				POWER_SUPPLY_PROP_STATUS, val);
			break;

		case POWER_SUPPLY_STATUS_CHARGING:
			pr_err("%s: Interrupted but Charging\n", __func__);
			break;

		case POWER_SUPPLY_STATUS_UNKNOWN:
		default:
			pr_err("%s: Invalid Charger Status\n", __func__);
			break;
		}
	}

	if (charger->pdata->ovp_uvlo_check_type ==
			SEC_BATTERY_OVP_UVLO_CHGINT) {

		val.intval = max77803_get_health_state(charger);

		switch (val.intval) {
		case POWER_SUPPLY_HEALTH_OVERHEAT:
		case POWER_SUPPLY_HEALTH_COLD:
			pr_err("%s: Interrupted but Hot/Cold\n", __func__);
			break;

		case POWER_SUPPLY_HEALTH_DEAD:
			pr_err("%s: Interrupted but Dead\n", __func__);
			break;

		case POWER_SUPPLY_HEALTH_OVERVOLTAGE:
		case POWER_SUPPLY_HEALTH_UNDERVOLTAGE:
			pr_info("%s: Interrupted by OVP/UVLO\n", __func__);
			psy_do_property("battery", set,
				POWER_SUPPLY_PROP_HEALTH, val);
			break;

		case POWER_SUPPLY_HEALTH_UNSPEC_FAILURE:
			pr_err("%s: Interrupted but Unspec\n", __func__);
			break;

		case POWER_SUPPLY_HEALTH_GOOD:
			pr_err("%s: Interrupted but Good\n", __func__);
			break;

		case POWER_SUPPLY_HEALTH_UNKNOWN:
		default:
			pr_err("%s: Invalid Charger Health\n", __func__);
			break;
		}
	}
}

static irqreturn_t sec_chg_irq_thread(int irq, void *irq_data)
{
	struct max77803_charger_data *charger = irq_data;

	pr_info("%s: Charger interrupt occured\n", __func__);

	if ((charger->pdata->full_check_type ==
				SEC_BATTERY_FULLCHARGED_CHGINT) ||
			(charger->pdata->ovp_uvlo_check_type ==
			 SEC_BATTERY_OVP_UVLO_CHGINT))
		schedule_delayed_work(&charger->isr_work, 0);

	return IRQ_HANDLED;
}

#if defined(CONFIG_CHARGER_MAX77803)
static void wpc_detect_work(struct work_struct *work)
{
	struct max77803_charger_data *chg_data = container_of(work,
						struct max77803_charger_data,
						wpc_work.work);
	int wc_w_state;
	int retry_cnt;
	union power_supply_propval value;
	u8 reg_data = 0;
	pr_debug("%s\n", __func__);

	reg_data = (0 << WCIN_SHIFT);
	max77803_update_reg(chg_data->max77803->i2c, MAX77803_CHG_REG_CHG_INT_MASK, reg_data,
			WCIN_MASK);

	/* check and unlock */
	check_charger_unlock_state(chg_data);

	retry_cnt = 0;
	do {
		max77803_read_reg(chg_data->max77803->i2c,
				MAX77803_CHG_REG_CHG_INT_OK, &reg_data);
		wc_w_state = (reg_data & MAX77803_WCIN_OK)
					>> MAX77803_WCIN_OK_SHIFT;
		msleep(50);
	} while((retry_cnt++ < 2) && (wc_w_state == 0));

	if ((chg_data->wc_w_state == 0) && (wc_w_state == 1)) {
#if 0
		value.intval = POWER_SUPPLY_TYPE_WIRELESS;
		psy_do_property("battery", set,
				POWER_SUPPLY_PROP_ONLINE, value);
#else
		value.intval = 1;
		psy_do_property("wireless", set,
				POWER_SUPPLY_PROP_ONLINE, value);

#endif
		pr_info("%s: wpc activated, set V_INT as PN\n",
				__func__);
	} else if ((chg_data->wc_w_state == 1) && (wc_w_state == 0)) {
		if (!chg_data->is_charging)
			max77803_set_charger_state(chg_data, true);

		retry_cnt = 0;
		do {
			max77803_read_reg(chg_data->max77803->i2c,
					MAX77803_CHG_REG_CHG_DTLS_01, &reg_data);
			reg_data = ((reg_data & MAX77803_CHG_DTLS)
					>> MAX77803_CHG_DTLS_SHIFT);
			msleep(50);
		} while((retry_cnt++ < 2) && (reg_data == 0x8));
		pr_info("%s: reg_data: 0x%x, charging: %d\n", __func__,
					reg_data, chg_data->is_charging);

		if (!chg_data->is_charging)
			max77803_set_charger_state(chg_data, false);
		if ((reg_data != 0x08)
				&& (chg_data->cable_type == POWER_SUPPLY_TYPE_WIRELESS)) {
			pr_info("%s: wpc uvlo, but charging\n", __func__);
			queue_delayed_work(chg_data->wqueue, &chg_data->wpc_work,
					msecs_to_jiffies(500));
			return;
		} else {
#if 0
			value.intval = POWER_SUPPLY_TYPE_WIRELESS_REMOVE;
			psy_do_property("battery", set,
					POWER_SUPPLY_PROP_ONLINE, value);
#else
			value.intval = 0;
			psy_do_property("wireless", set,
					POWER_SUPPLY_PROP_ONLINE, value);
#endif
			pr_info("%s: wpc deactivated, set V_INT as PD\n",
					__func__);
		}
	}
	pr_info("%s: w(%d to %d)\n", __func__,
			chg_data->wc_w_state, wc_w_state);

	chg_data->wc_w_state = wc_w_state;

	__pm_relax(&chg_data->wpc_ws);
}

static irqreturn_t wpc_charger_irq(int irq, void *data)
{
	struct max77803_charger_data *chg_data = data;
	unsigned long delay;

	__pm_stay_awake(&chg_data->wpc_ws);
#ifdef CONFIG_SAMSUNG_BATTERY_FACTORY
	delay = msecs_to_jiffies(0);
#else
	if (chg_data->wc_w_state)
		delay = msecs_to_jiffies(500);
	else
		delay = msecs_to_jiffies(0);
#endif
	queue_delayed_work(chg_data->wqueue, &chg_data->wpc_work,
			delay);
	return IRQ_HANDLED;
}
#elif defined(CONFIG_WIRELESS_CHARGING)
static irqreturn_t wpc_charger_irq(int irq, void *data)
{
	struct max77803_charger_data *chg_data = data;
	int wc_w_state;
	union power_supply_propval value;
	pr_info("%s: irq(%d)\n", __func__, irq);

	/* check and unlock */
	check_charger_unlock_state(chg_data);

	wc_w_state = !gpio_get_value(chg_data->wc_w_gpio);
	if ((chg_data->wc_w_state == 0) && (wc_w_state == 1)) {
		value.intval = POWER_SUPPLY_TYPE_WIRELESS
					<<ONLINE_TYPE_MAIN_SHIFT;
		psy_do_property("battery", set,
				POWER_SUPPLY_PROP_ONLINE, value);
		pr_info("%s: wpc activated, set V_INT as PN\n",
				__func__);
	} else if ((chg_data->wc_w_state == 1) && (wc_w_state == 0)) {
		value.intval =
			POWER_SUPPLY_TYPE_BATTERY<<ONLINE_TYPE_MAIN_SHIFT;
		psy_do_property("battery", set,
				POWER_SUPPLY_PROP_ONLINE, value);
		pr_info("%s: wpc deactivated, set V_INT as PD\n",
				__func__);
	}
	pr_info("%s: w(%d to %d)\n", __func__,
			chg_data->wc_w_state, wc_w_state);

	chg_data->wc_w_state = wc_w_state;

	return IRQ_HANDLED;
}
#endif

static irqreturn_t max77803_bypass_irq(int irq, void *data)
{
	struct max77803_charger_data *chg_data = data;
	u8 dtls_02;
	u8 byp_dtls;
	u8 chg_cnfg_00;
	u8 vbus_state;

	pr_info("%s: irq(%d)\n", __func__, irq);

	/* check and unlock */
	check_charger_unlock_state(chg_data);

	max77803_read_reg(chg_data->max77803->i2c,
				MAX77803_CHG_REG_CHG_DTLS_02,
				&dtls_02);

	byp_dtls = ((dtls_02 & MAX77803_BYP_DTLS) >>
				MAX77803_BYP_DTLS_SHIFT);
	pr_info("%s: BYP_DTLS(0x%02x)\n", __func__, byp_dtls);
	vbus_state = max77803_get_vbus_state(chg_data);

	if (byp_dtls & 0x1) {
		pr_info("%s: bypass overcurrent limit\n", __func__);
#ifdef CONFIG_USB_HOST_NOTIFY
		sec_otg_notify(HNOTIFY_OVERCURRENT);
#endif
		/* disable the register values just related to OTG and
		   keep the values about the charging */
		max77803_read_reg(chg_data->max77803->i2c,
			MAX77803_CHG_REG_CHG_CNFG_00, &chg_cnfg_00);
		chg_cnfg_00 &= ~(CHG_CNFG_00_OTG_MASK
				| CHG_CNFG_00_BOOST_MASK
				| CHG_CNFG_00_DIS_MUIC_CTRL_MASK);
		max77803_write_reg(chg_data->max77803->i2c,
					MAX77803_CHG_REG_CHG_CNFG_00,
					chg_cnfg_00);
	}
	if ((byp_dtls & 0x8) && (vbus_state < 0x03)) {
		reduce_input_current(chg_data, REDUCE_CURRENT_STEP);
	}

	return IRQ_HANDLED;
}

static void max77803_chgin_isr_work(struct work_struct *work)
{
	struct max77803_charger_data *charger = container_of(work,
				struct max77803_charger_data, chgin_work);
	u8 chgin_dtls, chg_dtls, chg_cnfg_00, reg_data;
	u8 prev_chgin_dtls = 0xff;
	int battery_health;
	union power_supply_propval value;
	int stable_count = 0;

	__pm_stay_awake(&charger->chgin_ws);

	while (1) {
		psy_do_property("battery", get,
				POWER_SUPPLY_PROP_HEALTH, value);
		battery_health = value.intval;

		max77803_read_reg(charger->max77803->i2c,
				MAX77803_CHG_REG_CHG_DTLS_00,
				&chgin_dtls);
		chgin_dtls = ((chgin_dtls & MAX77803_CHGIN_DTLS) >>
				MAX77803_CHGIN_DTLS_SHIFT);
		max77803_read_reg(charger->max77803->i2c,
				MAX77803_CHG_REG_CHG_DTLS_01, &chg_dtls);
		chg_dtls = ((chg_dtls & MAX77803_CHG_DTLS) >>
				MAX77803_CHG_DTLS_SHIFT);
		max77803_read_reg(charger->max77803->i2c,
			MAX77803_CHG_REG_CHG_CNFG_00, &chg_cnfg_00);

		if (prev_chgin_dtls == chgin_dtls)
			stable_count++;
		else
			stable_count = 0;
		if (stable_count > 10) {
			pr_info("%s: irq(%d), chgin(0x%x), chg_dtls(0x%x) prev 0x%x\n",
					__func__, charger->irq_chgin,
					chgin_dtls, chg_dtls, prev_chgin_dtls);
			if (charger->is_charging) {
				if ((chgin_dtls == 0x02) && \
					(battery_health != POWER_SUPPLY_HEALTH_OVERVOLTAGE)) {
					pr_info("%s: charger is over voltage\n",
							__func__);
					value.intval = POWER_SUPPLY_HEALTH_OVERVOLTAGE;
					psy_do_property("battery", set,
						POWER_SUPPLY_PROP_HEALTH, value);
				} else if (((chgin_dtls == 0x0) || (chgin_dtls == 0x01)) &&(chg_dtls & 0x08) && \
						(chg_cnfg_00 & MAX77803_MODE_BUCK) && \
						(chg_cnfg_00 & MAX77803_MODE_CHGR) && \
						(battery_health != POWER_SUPPLY_HEALTH_UNDERVOLTAGE) && \
						(charger->cable_type != POWER_SUPPLY_TYPE_WIRELESS)) {
					pr_info("%s, vbus_state : 0x%d, chg_state : 0x%d\n", __func__, chgin_dtls, chg_dtls);
					pr_info("%s: vBus is undervoltage\n", __func__);
					value.intval = POWER_SUPPLY_HEALTH_UNDERVOLTAGE;
					psy_do_property("battery", set,
							POWER_SUPPLY_PROP_HEALTH, value);
				}
			} else {
				if ((battery_health == \
							POWER_SUPPLY_HEALTH_OVERVOLTAGE) &&
						(chgin_dtls != 0x02)) {
					pr_info("%s: vbus_state : 0x%d, chg_state : 0x%d\n", __func__, chgin_dtls, chg_dtls);
					pr_info("%s: overvoltage->normal\n", __func__);
					value.intval = POWER_SUPPLY_HEALTH_GOOD;
					psy_do_property("battery", set,
							POWER_SUPPLY_PROP_HEALTH, value);
				} else if ((battery_health == \
							POWER_SUPPLY_HEALTH_UNDERVOLTAGE) &&
						!((chgin_dtls == 0x0) || (chgin_dtls == 0x01))){
					pr_info("%s: vbus_state : 0x%d, chg_state : 0x%d\n", __func__, chgin_dtls, chg_dtls);
					pr_info("%s: undervoltage->normal\n", __func__);
					value.intval = POWER_SUPPLY_HEALTH_GOOD;
					psy_do_property("battery", set,
							POWER_SUPPLY_PROP_HEALTH, value);
					max77803_set_input_current(charger,
							charger->charging_current_max);
				}
			}
			break;
		}

		if (charger->is_charging) {
			/* reduce only at CC MODE */
			if (((chgin_dtls == 0x0) || (chgin_dtls == 0x01)) &&
					(chg_dtls == 0x01) && (stable_count > 2))
				reduce_input_current(charger, REDUCE_CURRENT_STEP);
		}
		prev_chgin_dtls = chgin_dtls;
		msleep(100);
	}
	reg_data = (0 << CHGIN_SHIFT);
	max77803_update_reg(charger->max77803->i2c, MAX77803_CHG_REG_CHG_INT_MASK, reg_data,
			CHGIN_MASK);
	__pm_relax(&charger->chgin_ws);
}

static irqreturn_t max77803_chgin_irq(int irq, void *data)
{
	struct max77803_charger_data *charger = data;
	queue_work(charger->wqueue, &charger->chgin_work);

	return IRQ_HANDLED;
}

static irqreturn_t max77803_battery_irq(int irq, void *data)
{
	struct max77803_charger_data *charger = data;
	union power_supply_propval value;

	value.intval = max77803_get_battery_present(charger);

	pr_info("%s: battery present(%d)\n", __func__, value.intval);
	psy_do_property("battery", set,
			POWER_SUPPLY_PROP_PRESENT, value);

	return IRQ_HANDLED;
}

/* register chgin/battery isr after sec_battery_probe */
static void max77803_chgin_init_work(struct work_struct *work)
{
	struct max77803_charger_data *charger = container_of(work,
						struct max77803_charger_data,
						chgin_init_work.work);
	int ret;

	pr_info("%s \n", __func__);
	ret = request_threaded_irq(charger->irq_chgin, NULL,
			max77803_chgin_irq, 0, "chgin-irq", charger);
	if (ret < 0) {
		pr_err("%s: fail to request chgin IRQ: %d: %d\n",
				__func__, charger->irq_chgin, ret);
	}

	if (charger->irq_battery > 0) {
		ret = request_threaded_irq(charger->irq_battery, NULL,
				max77803_battery_irq, IRQF_TRIGGER_FALLING,
				"battery-irq", charger);

		if (ret < 0)
			pr_err("%s: fail to request battery IRQ: %d: %d\n",
				__func__, charger->irq_battery, ret);
	}
}

#ifdef CONFIG_OF
static int sec_charger_read_u32_index_dt(const struct device_node *np,
				       const char *propname,
				       u32 index, u32 *out_value)
{
	struct property *prop = of_find_property(np, propname, NULL);
	u32 len = (index + 1) * sizeof(*out_value);

	if (!prop)
		return (-EINVAL);
	if (!prop->value)
		return (-ENODATA);
	if (len > prop->length)
		return (-EOVERFLOW);

	*out_value = be32_to_cpup(((__be32 *)prop->value) + index);

	return 0;
}

static int sec_charger_parse_dt(struct max77803_charger_data *charger)
{
	struct device_node *np = of_find_node_by_name(NULL, "charger");
	sec_battery_platform_data_t *pdata = charger->pdata;
	int ret = 0;
	int i, len;
	const u32 *p;

	if (np == NULL) {
		pr_err("%s np NULL\n", __func__);
	} else {
		ret = of_property_read_u32(np, "battery,chg_float_voltage",
					&pdata->chg_float_voltage);
		ret = of_property_read_u32(np, "battery,ovp_uvlo_check_type",
					&pdata->ovp_uvlo_check_type);
		ret = of_property_read_u32(np, "battery,full_check_type",
					&pdata->full_check_type);

		p = of_get_property(np, "battery,input_current_limit", &len);
		len = len / sizeof(u32);
		pdata->charging_current = kzalloc(sizeof(sec_charging_current_t) * len,
						  GFP_KERNEL);

		for(i = 0; i < len; i++) {
			ret = sec_charger_read_u32_index_dt(np,
					 "battery,input_current_limit", i,
					 &pdata->charging_current[i].input_current_limit);
			ret = sec_charger_read_u32_index_dt(np,
					 "battery,fast_charging_current", i,
					 &pdata->charging_current[i].fast_charging_current);
			ret = sec_charger_read_u32_index_dt(np,
					 "battery,full_check_current_1st", i,
					 &pdata->charging_current[i].full_check_current_1st);
			ret = sec_charger_read_u32_index_dt(np,
					 "battery,full_check_current_2nd", i,
					 &pdata->charging_current[i].full_check_current_2nd);
		}
		np = of_find_node_by_name(NULL, "battery");
		if (np == NULL) {
			pr_err("%s : np NULL\n", __func__);
			pdata->bat_irq_gpio = -1;
		} else {
			ret = of_get_named_gpio(np, "battery,ta_int", 0);
			if (ret < 0) {
				pr_info("%s use bat irq %d\n", __func__, ret);

				/* temporally assign for check*/
				pdata->bat_irq_gpio = ret;
			}
		}
	}

	return ret;
}
#endif

static __devinit int max77803_charger_probe(struct platform_device *pdev)
{
	struct max77803_dev *iodev = dev_get_drvdata(pdev->dev.parent);
	struct max77803_platform_data *pdata = dev_get_platdata(iodev->dev);
	struct max77803_charger_data *charger;
	int ret = 0;
	u8 reg_data;

	pr_info("%s: MAX77803 Charger driver probe\n", __func__);

	charger = kzalloc(sizeof(*charger), GFP_KERNEL);
	if (!charger)
		return -ENOMEM;

	pdata->charger_data = kzalloc(sizeof(sec_battery_platform_data_t), GFP_KERNEL);
	if (!pdata->charger_data) {
		ret = -ENOMEM;
		goto err_free;
	}

	charger->max77803 = iodev;
	charger->pdata = pdata->charger_data;
	charger->aicl_on = false;
	charger->siop_level = 100;

#ifdef CONFIG_OF
	if (sec_charger_parse_dt(charger))
		dev_err(&pdev->dev,
			"%s : Failed to get charger int\n", __func__);
#endif
	platform_set_drvdata(pdev, charger);

	charger->psy_chg.name           = "sec-charger";
	charger->psy_chg.type           = POWER_SUPPLY_TYPE_UNKNOWN;
	charger->psy_chg.get_property   = sec_chg_get_property;
	charger->psy_chg.set_property   = sec_chg_set_property;
	charger->psy_chg.properties     = sec_charger_props;
	charger->psy_chg.num_properties = ARRAY_SIZE(sec_charger_props);

	mutex_init(&charger->ops_lock);

	if (charger->pdata->chg_gpio_init) {
		if (!charger->pdata->chg_gpio_init()) {
			pr_err("%s: Failed to Initialize GPIO\n", __func__);
			goto err_free;
		}
	}

	max77803_charger_initialize(charger);

	if (max77803_read_reg(charger->max77803->i2c, MAX77803_PMIC_REG_PMIC_ID1, &reg_data) < 0) {
		pr_err("device not found on this channel (this is not an error)\n");
		ret = -ENODEV;
		goto err_free;
	} else {
		charger->pmic_ver = (reg_data & 0xf);
		pr_info("%s: device found: ver.0x%x\n", __func__,
				charger->pmic_ver);
	}

	if (charger->pmic_ver == 0x04) {
		charger->input_curr_limit_step = 25;
		charger->charging_curr_step= 400;  // 0.1mA unit
	} else {
		charger->input_curr_limit_step = 20;
		charger->charging_curr_step= 333;  // 0.1mA unit
	}

	charger->wqueue =
	    create_singlethread_workqueue(dev_name(&pdev->dev));
	if (!charger->wqueue) {
		pr_err("%s: Fail to Create Workqueue\n", __func__);
		goto err_free;
	}
	wakeup_source_init(&charger->chgin_ws, "charger-chgin");
	INIT_WORK(&charger->chgin_work, max77803_chgin_isr_work);
	INIT_DELAYED_WORK(&charger->chgin_init_work, max77803_chgin_init_work);
	wakeup_source_init(&charger->recovery_ws, "charger-recovery");
	INIT_DELAYED_WORK(&charger->recovery_work, max77803_recovery_work);
	wakeup_source_init(&charger->wpc_ws, "charger-wpc");
	INIT_DELAYED_WORK(&charger->wpc_work, wpc_detect_work);
	ret = power_supply_register(&pdev->dev, &charger->psy_chg);
	if (ret) {
		pr_err("%s: Failed to Register psy_chg\n", __func__);
		goto err_power_supply_register;
	}

	if (charger->pdata->chg_irq) {
		INIT_DELAYED_WORK_DEFERRABLE(
				&charger->isr_work, sec_chg_isr_work);
		ret = request_threaded_irq(charger->pdata->chg_irq,
				NULL, sec_chg_irq_thread,
				charger->pdata->chg_irq_attr,
				"charger-irq", charger);
		if (ret) {
			pr_err("%s: Failed to Reqeust IRQ\n", __func__);
			goto err_irq;
		}
	}
#if defined(CONFIG_CHARGER_MAX77803)
	charger->wc_w_irq = pdata->irq_base + MAX77803_CHG_IRQ_WCIN_I;
	ret = request_threaded_irq(charger->wc_w_irq,
			NULL, wpc_charger_irq,
			0, "wpc-int", charger);
	if (ret) {
		pr_err("%s: Failed to Reqeust IRQ\n", __func__);
		goto err_wc_irq;
	}
	max77803_read_reg(charger->max77803->i2c,
			MAX77803_CHG_REG_CHG_INT_OK, &reg_data);
	charger->wc_w_state = (reg_data & MAX77803_WCIN_OK)
				>> MAX77803_WCIN_OK_SHIFT;
#elif defined(CONFIG_WIRELESS_CHARGING)
	charger->wc_w_gpio = pdata->wc_irq_gpio;
	if (charger->wc_w_gpio) {
		charger->wc_w_irq = gpio_to_irq(charger->wc_w_gpio);
		ret = gpio_request(charger->wc_w_gpio, "wpc_charger-irq");
		if (ret < 0) {
			pr_err("%s: failed requesting gpio %d\n", __func__,
				charger->wc_w_gpio);
			goto err_wc_irq;
		}
		ret = request_threaded_irq(charger->wc_w_irq,
				NULL, wpc_charger_irq,
				IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING |
				IRQF_ONESHOT,
				"wpc-int", charger);
		if (ret) {
			pr_err("%s: Failed to Reqeust IRQ\n", __func__);
			goto err_wc_irq;
		}
		enable_irq_wake(charger->wc_w_irq);
		charger->wc_w_state = !gpio_get_value(charger->wc_w_gpio);
	}
#endif

	charger->irq_chgin = pdata->irq_base + MAX77803_CHG_IRQ_CHGIN_I;
	if (charger->pdata->bat_irq_gpio < 0)
		charger->irq_battery = pdata->irq_base + MAX77803_CHG_IRQ_BATP_I;
	/* enable chgin/battery irq after sec_battery_probe */
	queue_delayed_work(charger->wqueue, &charger->chgin_init_work,
			msecs_to_jiffies(3000));

	charger->irq_bypass = pdata->irq_base + MAX77803_CHG_IRQ_BYP_I;
	ret = request_threaded_irq(charger->irq_bypass, NULL,
			max77803_bypass_irq, 0, "bypass-irq", charger);
	if (ret < 0)
		pr_err("%s: fail to request bypass IRQ: %d: %d\n",
				__func__, charger->irq_bypass, ret);
	return 0;
err_wc_irq:
	free_irq(charger->pdata->chg_irq, NULL);
err_irq:
	power_supply_unregister(&charger->psy_chg);
err_power_supply_register:
	destroy_workqueue(charger->wqueue);
err_free:
	kfree(charger);

	return ret;

}

static int __devexit max77803_charger_remove(struct platform_device *pdev)
{
	struct max77803_charger_data *charger =
				platform_get_drvdata(pdev);

	destroy_workqueue(charger->wqueue);
	free_irq(charger->wc_w_irq, NULL);
	free_irq(charger->pdata->chg_irq, NULL);
	power_supply_unregister(&charger->psy_chg);
	kfree(charger);

	return 0;
}

#if defined CONFIG_PM
static int max77803_charger_suspend(struct device *dev)
{
	return 0;
}

static int max77803_charger_resume(struct device *dev)
{
	return 0;
}
#else
#define max77803_charger_suspend NULL
#define max77803_charger_resume NULL
#endif

static void max77803_charger_shutdown(struct device *dev)
{
	struct max77803_charger_data *charger = dev_get_drvdata(dev);
	u8 reg_data;

	pr_info("%s: MAX77803 Charger driver shutdown\n", __func__);
	if (!charger->max77803->i2c) {
		pr_err("%s: no max77803 i2c client\n", __func__);
		return;
	}
	reg_data = 0x04;
	max77803_write_reg(charger->max77803->i2c,
		MAX77803_CHG_REG_CHG_CNFG_00, reg_data);
	reg_data = 0x19;
	max77803_write_reg(charger->max77803->i2c,
		MAX77803_CHG_REG_CHG_CNFG_09, reg_data);
	reg_data = 0x19;
	max77803_write_reg(charger->max77803->i2c,
		MAX77803_CHG_REG_CHG_CNFG_10, reg_data);
	reg_data = 0x64;
	max77803_write_reg(charger->max77803->i2c,
		MAX77803_CHG_REG_CHG_CNFG_12, reg_data);
	pr_info("func:%s \n", __func__);
}

static SIMPLE_DEV_PM_OPS(max77803_charger_pm_ops, max77803_charger_suspend,
		max77803_charger_resume);

static struct platform_driver max77803_charger_driver = {
	.driver = {
		.name = "max77803-charger",
		.owner = THIS_MODULE,
		.pm = &max77803_charger_pm_ops,
		.shutdown = max77803_charger_shutdown,
	},
	.probe = max77803_charger_probe,
	.remove = __devexit_p(max77803_charger_remove),
};

static int __init max77803_charger_init(void)
{
	pr_info("func:%s\n", __func__);
	return platform_driver_register(&max77803_charger_driver);
}
module_init(max77803_charger_init);

static void __exit max77803_charger_exit(void)
{
	platform_driver_register(&max77803_charger_driver);
}

module_exit(max77803_charger_exit);

MODULE_DESCRIPTION("max77803 charger driver");
MODULE_AUTHOR("Samsung Electronics");
MODULE_LICENSE("GPL");
