// SPDX-License-Identifier: GPL-2.0-only
/*
 *  Steelseries Headsets
 *
 *  Copyright (c) 2023 Bastien Nocera
 */


#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/device.h>
#include <linux/hid.h>
#include <linux/module.h>
#include <linux/usb.h>
#include "hid-ids.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Bastien Nocera <hadess@hadess.net>");

#define STEELSERIES_ARCTIS_1		BIT(0)

struct steelseries_headset {
	struct hid_device *hdev;
	unsigned long quirks;

	struct delayed_work battery_work;
	spinlock_t lock;
	bool removed;

	struct power_supply_desc battery_desc;
	struct power_supply *battery;
	uint8_t battery_capacity;
	bool headset_connected;
};

#define STEELSERIES_HEADSET_BATTERY_TIMEOUT_MS	3000

#define ARCTIS_1_BATTERY_RESPONSE_LEN		8

static int steelseries_headset_arctis_1_fetch_battery(struct hid_device *hdev)
{
	u8 *write_buf;
	int ret;
	char battery_request[2] = { 0x06, 0x12 };

	/* Request battery information */
	write_buf = kmemdup(battery_request, sizeof(battery_request), GFP_KERNEL);
	if (!write_buf)
		return -ENOMEM;

	ret = hid_hw_raw_request(hdev, battery_request[0],
				 write_buf, sizeof(battery_request),
				 HID_OUTPUT_REPORT, HID_REQ_SET_REPORT);
	if (ret < sizeof(battery_request)) {
		hid_err(hdev, "hid_hw_raw_request() failed with %d\n", ret);
		ret = -ENODATA;
	}
	kfree(write_buf);
	return ret;
}

static void steelseries_headset_fetch_battery(struct hid_device *hdev)
{
	struct steelseries_headset *headset = hid_get_drvdata(hdev);
	int ret = 0;

	if (headset->quirks & STEELSERIES_ARCTIS_1)
		ret = steelseries_headset_arctis_1_fetch_battery(hdev);

	if (ret < 0)
		hid_dbg(hdev,
			"Battery query failed (err: %d)\n", ret);
}

static void steelseries_headset_battery_timer_tick(struct work_struct *work)
{
	struct steelseries_headset *headset = container_of(work,
		struct steelseries_headset, battery_work.work);
	struct hid_device *hdev = headset->hdev;

	steelseries_headset_fetch_battery(hdev);
}

static int steelseries_headset_battery_get_property(struct power_supply *psy,
				enum power_supply_property psp,
				union power_supply_propval *val)
{
	struct steelseries_headset *headset = power_supply_get_drvdata(psy);
	int ret = 0;

	switch (psp) {
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = 1;
		break;
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = headset->headset_connected ?
			POWER_SUPPLY_STATUS_DISCHARGING :
			POWER_SUPPLY_STATUS_UNKNOWN;
		break;
	case POWER_SUPPLY_PROP_SCOPE:
		val->intval = POWER_SUPPLY_SCOPE_DEVICE;
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		val->intval = headset->battery_capacity;
		break;
	default:
		ret = -EINVAL;
		break;
	}
	return ret;
}

static enum power_supply_property steelseries_headset_battery_props[] = {
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_SCOPE,
	POWER_SUPPLY_PROP_CAPACITY,
};

static int steelseries_headset_battery_register(struct steelseries_headset *headset)
{
	static atomic_t battery_no = ATOMIC_INIT(0);
	struct power_supply_config battery_cfg = { .drv_data = headset, };
	unsigned long n;
	int ret;

	headset->battery_desc.type = POWER_SUPPLY_TYPE_BATTERY;
	headset->battery_desc.properties = steelseries_headset_battery_props;
	headset->battery_desc.num_properties = ARRAY_SIZE(steelseries_headset_battery_props);
	headset->battery_desc.get_property = steelseries_headset_battery_get_property;
	headset->battery_desc.use_for_apm = 0;
	n = atomic_inc_return(&battery_no) - 1;
	headset->battery_desc.name = devm_kasprintf(&headset->hdev->dev, GFP_KERNEL,
						    "steelseries_headset_battery_%ld", n);
	if (!headset->battery_desc.name)
		return -ENOMEM;

	/* avoid the warning of 0% battery while waiting for the first info */
	headset->battery_capacity = 100;

	headset->battery = devm_power_supply_register(&headset->hdev->dev,
			&headset->battery_desc, &battery_cfg);
	if (IS_ERR(headset->battery)) {
		ret = PTR_ERR(headset->battery);
		hid_err(headset->hdev,
				"%s:power_supply_register failed with error %d\n",
				__func__, ret);
		return ret;
	}
	power_supply_powers(headset->battery, &headset->hdev->dev);

	INIT_DELAYED_WORK(&headset->battery_work, steelseries_headset_battery_timer_tick);
	steelseries_headset_fetch_battery(headset->hdev);

	return 0;
}

static int steelseries_headset_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	struct steelseries_headset *headset;
	int ret;

	headset = devm_kzalloc(&hdev->dev, sizeof(*headset), GFP_KERNEL);
	if (!headset)
		return -ENOMEM;
	hid_set_drvdata(hdev, headset);
	headset->hdev = hdev;
	headset->quirks = id->driver_data;

	ret = hid_parse(hdev);
	if (ret)
		return ret;

	ret = hid_hw_start(hdev, HID_CONNECT_DEFAULT);
	if (ret)
		return ret;

	if (steelseries_headset_battery_register(headset) < 0)
		hid_err(headset->hdev,
			"Failed to register battery for headset\n");

	return ret;
}

static void steelseries_headset_remove(struct hid_device *hdev)
{
	struct steelseries_headset *headset = hid_get_drvdata(hdev);
	unsigned long flags;

	spin_lock_irqsave(&headset->lock, flags);
	headset->removed = true;
	spin_unlock_irqrestore(&headset->lock, flags);

	cancel_delayed_work_sync(&headset->battery_work);

	hid_hw_stop(hdev);
}

static int steelseries_headset_raw_event(struct hid_device *hdev,
					struct hid_report *report, u8 *read_buf,
					int size)
{
	struct steelseries_headset *headset = hid_get_drvdata(hdev);
	int capacity = headset->battery_capacity;
	bool connected = headset->headset_connected;
	unsigned long flags;

	if (headset->quirks & STEELSERIES_ARCTIS_1) {
		hid_dbg(headset->hdev,
			"Parsing raw event for Arctis 1 headset (len: %d)\n", size);
		if (size < 8)
			return 0;
		if (read_buf[2] == 0x01) {
			connected = false;
			capacity = 100;
		} else {
			connected = true;
			capacity = read_buf[3];
		}
	}

	if (connected != headset->headset_connected) {
		struct usb_interface *intf;

		hid_dbg(headset->hdev,
			"Connected status changed from %sconnected to %sconnected\n",
			headset->headset_connected ? "" : "not ",
			connected ? "" : "not ");
		headset->headset_connected = connected;

		intf = to_usb_interface(hdev->dev.parent);
		usb_set_wireless_status(intf, connected ?
					USB_WIRELESS_STATUS_CONNECTED :
					USB_WIRELESS_STATUS_DISCONNECTED);

	}

	if (capacity != headset->battery_capacity) {
		hid_dbg(headset->hdev,
			"Battery capacity changed from %d%% to %d%%\n",
			headset->battery_capacity, capacity);
		headset->battery_capacity = capacity;
		power_supply_changed(headset->battery);
	}

	spin_lock_irqsave(&headset->lock, flags);
	if (!headset->removed)
		schedule_delayed_work(&headset->battery_work,
				msecs_to_jiffies(STEELSERIES_HEADSET_BATTERY_TIMEOUT_MS));
	spin_unlock_irqrestore(&headset->lock, flags);

	return 0;
}

static const struct hid_device_id steelseries_headset_devices[] = {
	{ /* SteelSeries Arctis 1 Wireless for XBox */
	  HID_USB_DEVICE(USB_VENDOR_ID_STEELSERIES, 0x12b6),
	.driver_data = STEELSERIES_ARCTIS_1 },

	{}
};

MODULE_DEVICE_TABLE(hid, steelseries_headset_devices);

static struct hid_driver steelseries_headset_driver = {
	.name = "logitech-steelseries_headset-device",
	.id_table = steelseries_headset_devices,
	.probe = steelseries_headset_probe,
	.remove = steelseries_headset_remove,
	.raw_event = steelseries_headset_raw_event,
};

module_hid_driver(steelseries_headset_driver);
