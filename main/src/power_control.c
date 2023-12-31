#include "power_control.h"

#include <stdbool.h>
#include <stdint.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <driver/gpio.h>
#include <esp_log.h>
#include <esp_timer.h>

#include "debounce.h"
#include "scheduler.h"
#include "shared_config.h"
#include "util.h"

#define GPIO_POWER_ON	10
#define GPIO_CHARGE_EN	 1

#define CHARGER_WATCHDOG_RESET_INTERVAL_MS	10000

typedef enum power_state {
	POWER_STATE_ON,
	POWER_STATE_SOFT_OFF,
	POWER_STATE_HARD_OFF
} power_state_t;

typedef struct power_control {
	bq24295_t *charger;
	int64_t timestamp_charger_watchdog_reset;
	shared_config_t shared_cfg;
	debounce_bool_t power_switch_debounce;
	debounce_bool_t power_good_debounce;
	power_state_t power_state;
	bool ignore_power_switch;
	scheduler_task_t update_task;
} power_control_t;

#define FLAG_IGNORE_POWER_SWITCH	BIT(0)

typedef struct power_control_packet {
	uint8_t packet_type;
	uint8_t flags;
	shared_config_hdr_t shared_cfg_hdr;
} __attribute__((packed)) power_control_packet_t;

static const char *TAG = "power_control";

static power_control_t power_control = { 0 };

static void power_control_tx(void) {
	power_control_packet_t packet = {
		WIRELESS_PACKET_TYPE_POWER_CONTROL,
		.flags =
			(power_control.ignore_power_switch ? FLAG_IGNORE_POWER_SWITCH : 0)
	};
	shared_config_hdr_init(&power_control.shared_cfg, &packet.shared_cfg_hdr);
	wireless_broadcast((const uint8_t *)&packet, sizeof(packet));
	shared_config_tx_done(&power_control.shared_cfg);
}

static void config_changed(void) {
	shared_config_update_local(&power_control.shared_cfg);

	for (int i = 0; i < SHARED_CONFIG_TX_TIMES; i++) {
		power_control_tx();
	}
}

void power_control_rx(const wireless_packet_t *packet) {
	power_control_packet_t config_packet;
	if (packet->len < sizeof(config_packet)) {
		ESP_LOGD(TAG, "Received short packet, expected %u bytes but got only %u bytes",
		         sizeof(config_packet), packet->len);
		return;
	}
	memcpy(&config_packet, packet->data, sizeof(config_packet));

	if (shared_config_update_remote(&power_control.shared_cfg, &config_packet.shared_cfg_hdr)) {
		power_control.ignore_power_switch = !!(config_packet.flags & FLAG_IGNORE_POWER_SWITCH);
	}
}

static void force_input_current_limit(void) {
	esp_err_t err;
	unsigned int current_limit_ma;
	err = bq24295_get_input_current_limit(power_control.charger, &current_limit_ma);
	if (err) {
		ESP_LOGW(TAG, "Failed to check input current limit: %d", err);
		return;
	}

	if (current_limit_ma != 1500) {
		bq24295_set_input_current_limit(power_control.charger, 1500);
	}
}

static void power_control_update(void *arg) {
	bool power_switch_state = gpio_get_level(GPIO_POWER_ON) || power_control.ignore_power_switch;
	debounce_bool_update(&power_control.power_switch_debounce, power_switch_state);
	switch (power_control.power_state) {
	case POWER_STATE_ON:
		if (debounce_bool_get_value(&power_control.power_switch_debounce) == DEBOUNCE_FALSE) {
			debounce_bool_reset(&power_control.power_good_debounce);
			power_control.power_state = POWER_STATE_SOFT_OFF;
		}
		break;
	case POWER_STATE_SOFT_OFF:
		if (debounce_bool_get_value(&power_control.power_switch_debounce) == DEBOUNCE_TRUE) {
			power_control.power_state = POWER_STATE_ON;
		} else if (debounce_bool_get_value(&power_control.power_switch_debounce) == DEBOUNCE_FALSE) {
			bool power_good;
			esp_err_t err = bq24295_is_power_good(power_control.charger, &power_good);
			if (!err && debounce_bool_update(&power_control.power_good_debounce, power_good) &&
			    debounce_bool_get_value(&power_control.power_good_debounce) == DEBOUNCE_FALSE) {
				// Disable watchdog and other timers
				ESP_ERROR_CHECK(bq24295_set_watchdog_timeout(power_control.charger, BQ24295_WATCHDOG_TIMEOUT_DISABLED));
				// Disable BATFET
				ESP_ERROR_CHECK(bq24295_set_shutdown(power_control.charger, true));
				power_control.power_state = POWER_STATE_HARD_OFF;
			}
		}
		break;
	case POWER_STATE_HARD_OFF:
		if (debounce_bool_get_value(&power_control.power_switch_debounce) == DEBOUNCE_TRUE) {
			power_control.power_state = POWER_STATE_ON;
		}
	}

	uint64_t now = esp_timer_get_time();
	uint64_t ms_since_last_watchdog_reset = (now - power_control.timestamp_charger_watchdog_reset) / 1000LL;
	if (ms_since_last_watchdog_reset >= CHARGER_WATCHDOG_RESET_INTERVAL_MS) {
		force_input_current_limit();
		bq24295_watchdog_reset(power_control.charger);
	}

	if (shared_config_should_tx(&power_control.shared_cfg)) {
		power_control_tx();
	}
	scheduler_schedule_task_relative(&power_control.update_task, power_control_update, NULL, MS_TO_US(250));
}

esp_err_t power_control_init(bq24295_t *charger) {
	gpio_reset_pin(GPIO_CHARGE_EN);
	gpio_set_direction(GPIO_CHARGE_EN, GPIO_MODE_OUTPUT);
	gpio_set_level(GPIO_CHARGE_EN, 0);

	gpio_reset_pin(GPIO_POWER_ON);
	gpio_set_direction(GPIO_POWER_ON, GPIO_MODE_INPUT);
	gpio_set_pull_mode(GPIO_POWER_ON, GPIO_PULLDOWN_ONLY);

	// Enable BATFET
	esp_err_t err = bq24295_set_shutdown(charger, false);
	if (err) {
		ESP_LOGE(TAG, "Failed to enable batfet: %d", err);
		return err;
	}

	// Reset charger to default settings
	err = bq24295_reset(charger);
	if (err) {
		ESP_LOGE(TAG, "Failed to reset charger: %d", err);
		return err;
	}
	vTaskDelay(pdMS_TO_TICKS(10));

	// Setup charger settings
	// Min system voltage 3.0V
	err = bq24295_set_min_system_voltage(charger, 3000);
	if (err) {
		ESP_LOGE(TAG, "Failed to set minimum system voltage: %d", err);
		return err;
	}
	// Boost voltage 4.55V
	err = bq24295_set_boost_voltage(charger, 4550);
	if (err) {
		ESP_LOGE(TAG, "Failed to set LED boost voltage: %d", err);
		return err;
	}
	// Set input current limit to 1A
	err = bq24295_set_input_current_limit(charger, 1500);
	if (err) {
		ESP_LOGE(TAG, "Failed to set input current limit: %d", err);
		return err;
	}
	// Set charging current to 1024mA
	err = bq24295_set_charge_current(charger, 1024);
	if (err) {
		ESP_LOGE(TAG, "Failed to set charging current: %d", err);
		return err;
	}
	// Terminate charge at 128mA
	err = bq24295_set_termination_current(charger, 128);
	if (err) {
		ESP_LOGE(TAG, "Failed to set charge termination current: %d", err);
		return err;
	}
	// Assert battery low at 2.8V
	err = bq24295_set_battery_low_threshold(charger, BQ24295_BATTERY_LOW_THRESHOLD_2_8V);
	if (err) {
		ESP_LOGE(TAG, "Failed to set battery low threshold: %d", err);
		return err;
	}
	// Recharge battery if 300mV below charging voltage after charging
	err = bq24295_set_recharge_threshold(charger, BQ24295_RECHARGE_THRESHOLD_300MV);
	if (err) {
		ESP_LOGE(TAG, "Failed to set recharge threshold: %d", err);
		return err;
	}

	power_control.charger = charger;
	power_control.power_state = POWER_STATE_ON;
	debounce_bool_init(&power_control.power_switch_debounce, 3);
	debounce_bool_init(&power_control.power_good_debounce, 5);

	scheduler_task_init(&power_control.update_task);
	scheduler_schedule_task_relative(&power_control.update_task, power_control_update, NULL, MS_TO_US(100));
	return ESP_OK;
}

void power_control_set_ignore_power_switch(bool ignore) {
	if (ignore != power_control.ignore_power_switch) {
		power_control.ignore_power_switch = ignore;
		config_changed();
	}
}

bool power_control_is_powered_off(void) {
	return power_control.power_state != POWER_STATE_ON;
}
