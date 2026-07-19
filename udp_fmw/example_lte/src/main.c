/*
 * Terrestrial LTE-M/NB-IoT UDP sender for nRF9151 DK.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <zephyr/posix/arpa/inet.h>
#include <zephyr/posix/unistd.h>
#include <zephyr/sys/atomic.h>

#include <dk_buttons_and_leds.h>
#include <modem/at_monitor.h>
#include <modem/lte_lc.h>
#include <modem/modem_info.h>
#include <modem/nrf_modem_lib.h>
#include <nrf_modem_at.h>
#include <nrf_modem_gnss.h>

LOG_MODULE_REGISTER(lte_udp_button, CONFIG_LTE_UDP_BUTTON_LOG_LEVEL);

#define BUTTON_SEND_EVENT BIT(0)

static K_EVENT_DEFINE(app_events);
static K_SEM_DEFINE(gnss_fix_sem, 0, 1);

static atomic_t button_counter;
static volatile bool lte_ready;
static bool sim_status_printed;
static bool gnss_fix_valid;
static struct nrf_modem_gnss_pvt_data_frame last_fix;

static void connect_work_fn(struct k_work *work);
static void sim_status_work_fn(struct k_work *work);
static void location_print_work_fn(struct k_work *work);
static void cereg_mon(const char *notif);
static void cnec_mon(const char *notif);

static K_WORK_DELAYABLE_DEFINE(reconnect_work, connect_work_fn);
static K_WORK_DEFINE(sim_status_work, sim_status_work_fn);
static K_WORK_DEFINE(location_print_work, location_print_work_fn);

AT_MONITOR(cereg_monitor, "CEREG", cereg_mon);
AT_MONITOR(cnec_monitor, "CNEC", cnec_mon);

static bool registered(enum lte_lc_nw_reg_status status)
{
	return status == LTE_LC_NW_REG_REGISTERED_HOME ||
	       status == LTE_LC_NW_REG_REGISTERED_ROAMING;
}

static const char *reg_status_str(enum lte_lc_nw_reg_status status)
{
	switch (status) {
	case LTE_LC_NW_REG_NOT_REGISTERED:
		return "not registered";
	case LTE_LC_NW_REG_REGISTERED_HOME:
		return "registered home";
	case LTE_LC_NW_REG_SEARCHING:
		return "searching";
	case LTE_LC_NW_REG_REGISTRATION_DENIED:
		return "registration denied";
	case LTE_LC_NW_REG_UNKNOWN:
		return "unknown";
	case LTE_LC_NW_REG_REGISTERED_ROAMING:
		return "registered roaming";
	case LTE_LC_NW_REG_UICC_FAIL:
		return "UICC failure";
	case LTE_LC_NW_REG_NO_SUITABLE_CELL:
		return "no suitable cell";
	default:
		return "other";
	}
}

static void cereg_mon(const char *notif)
{
	LOG_INF("AT notification: %s", notif);
}

static void cnec_mon(const char *notif)
{
	LOG_WRN("Network error notification: %s", notif);
}

static void button_handler(uint32_t button_states, uint32_t has_changed)
{
	if ((has_changed & DK_BTN1_MSK) && (button_states & DK_BTN1_MSK)) {
		atomic_inc(&button_counter);
		k_event_post(&app_events, BUTTON_SEND_EVENT);
	}

	if ((has_changed & DK_BTN2_MSK) && (button_states & DK_BTN2_MSK)) {
		k_work_submit(&location_print_work);
	}
}

static void format_degrees(char *buf, size_t len, double value)
{
	double abs_value = value < 0.0 ? -value : value;
	uint32_t whole = (uint32_t)abs_value;
	uint32_t fraction = (uint32_t)((abs_value - whole) * 1000000.0);

	snprintk(buf, len, "%s%u.%06u", value < 0.0 ? "-" : "", whole, fraction);
}

static void format_meters(char *buf, size_t len, float value)
{
	float abs_value = value < 0.0f ? -value : value;
	uint32_t whole = (uint32_t)abs_value;
	uint32_t fraction = (uint32_t)((abs_value - whole) * 100.0f);

	snprintk(buf, len, "%s%u.%02u", value < 0.0f ? "-" : "", whole, fraction);
}

static void print_last_location(void)
{
	char latitude[20];
	char longitude[20];
	char altitude[16];
	char accuracy[16];

	if (!gnss_fix_valid) {
		LOG_WRN("GNSS location is not available yet");
		return;
	}

	format_degrees(latitude, sizeof(latitude), last_fix.latitude);
	format_degrees(longitude, sizeof(longitude), last_fix.longitude);
	format_meters(altitude, sizeof(altitude), last_fix.altitude);
	format_meters(accuracy, sizeof(accuracy), last_fix.accuracy);

	LOG_INF("GNSS location: lat=%s lon=%s alt=%s m accuracy=%s m",
		latitude, longitude, altitude, accuracy);
}

static void location_print_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	print_last_location();
}

static uint8_t satellites_tracked(const struct nrf_modem_gnss_pvt_data_frame *pvt)
{
	uint8_t tracked = 0;

	for (int i = 0; i < NRF_MODEM_GNSS_MAX_SATELLITES; i++) {
		if (pvt->sv[i].sv > 0) {
			tracked++;
		}
	}

	return tracked;
}

static void gnss_event_handler(int event)
{
	int err;
	struct nrf_modem_gnss_pvt_data_frame pvt;

	if (event != NRF_MODEM_GNSS_EVT_PVT && event != NRF_MODEM_GNSS_EVT_FIX) {
		return;
	}

	err = nrf_modem_gnss_read(&pvt, sizeof(pvt), NRF_MODEM_GNSS_DATA_PVT);
	if (err) {
		LOG_WRN("Failed to read GNSS PVT, err %d", err);
		return;
	}

	if ((pvt.flags & NRF_MODEM_GNSS_PVT_FLAG_FIX_VALID) == 0) {
		LOG_INF("Waiting for GNSS fix, tracked satellites=%u", satellites_tracked(&pvt));
		return;
	}

	last_fix = pvt;
	gnss_fix_valid = true;
	LOG_INF("GNSS fix acquired");
	print_last_location();
	k_sem_give(&gnss_fix_sem);
}

static int acquire_gnss_fix(void)
{
	int err;

	LOG_INF("Starting GNSS before LTE attach");

	err = lte_lc_system_mode_set(LTE_LC_SYSTEM_MODE_GPS, LTE_LC_SYSTEM_MODE_PREFER_AUTO);
	if (err) {
		LOG_ERR("Failed to set GNSS system mode, err %d", err);
		return err;
	}

	err = lte_lc_func_mode_set(LTE_LC_FUNC_MODE_ACTIVATE_GNSS);
	if (err) {
		LOG_ERR("Failed to activate GNSS, err %d", err);
		return err;
	}

	err = nrf_modem_gnss_event_handler_set(gnss_event_handler);
	if (err) {
		LOG_ERR("Failed to set GNSS event handler, err %d", err);
		return err;
	}

	err = nrf_modem_gnss_use_case_set(NRF_MODEM_GNSS_USE_CASE_MULTIPLE_HOT_START);
	if (err) {
		LOG_WRN("Failed to set GNSS use case, err %d", err);
	}

	err = nrf_modem_gnss_fix_retry_set(0);
	if (err) {
		LOG_ERR("Failed to set GNSS fix retry, err %d", err);
		return err;
	}

	err = nrf_modem_gnss_fix_interval_set(1);
	if (err) {
		LOG_ERR("Failed to set GNSS fix interval, err %d", err);
		return err;
	}

	err = nrf_modem_gnss_start();
	if (err) {
		LOG_ERR("Failed to start GNSS, err %d", err);
		return err;
	}

	LOG_INF("Waiting for first GNSS fix. Keep antenna with open sky view.");
	k_sem_take(&gnss_fix_sem, K_FOREVER);

	err = nrf_modem_gnss_stop();
	if (err) {
		LOG_WRN("Failed to stop GNSS, err %d", err);
	}

	err = lte_lc_func_mode_set(LTE_LC_FUNC_MODE_OFFLINE);
	if (err) {
		LOG_WRN("Failed to return modem offline after GNSS, err %d", err);
	}

	return 0;
}

static int at_print_response(const char *label, const char *cmd)
{
	int err;
	char response[128];

	err = nrf_modem_at_cmd(response, sizeof(response), "%s", cmd);
	if (err) {
		LOG_ERR("%s failed, err %d", label, err);
		return err;
	}

	LOG_INF("%s: %s", label, response);
	return 0;
}

static void print_sim_status(void)
{
	int err;
	char response[128];
	char iccid[MODEM_INFO_MAX_RESPONSE_SIZE];

	err = nrf_modem_at_cmd(response, sizeof(response), "AT+CPIN?");
	if (err) {
		LOG_ERR("SIM check failed, err %d", err);
		return;
	}

	if (strstr(response, "READY")) {
		LOG_INF("SIM status: valid/ready");
	} else {
		LOG_WRN("SIM status is not READY: %s", response);
	}

	err = modem_info_string_get(MODEM_INFO_ICCID, iccid, sizeof(iccid));
	if (err > 0) {
		LOG_INF("SIM ICCID: %s", iccid);
	} else {
		LOG_WRN("Could not read SIM ICCID, err %d", err);
	}
}

static void sim_status_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	print_sim_status();
}

static int configure_modem(void)
{
	int err;

	err = nrf_modem_at_printf("AT+CMEE=1");
	if (err) {
		LOG_ERR("AT+CMEE failed, err %d", err);
		return err;
	}

	err = nrf_modem_at_printf("AT+CEREG=5");
	if (err) {
		LOG_ERR("AT+CEREG failed, err %d", err);
		return err;
	}

	err = nrf_modem_at_printf("AT+CNEC=24");
	if (err) {
		LOG_ERR("AT+CNEC failed, err %d", err);
		return err;
	}

	err = nrf_modem_at_printf("AT+CSCON=3");
	if (err) {
		LOG_ERR("AT+CSCON failed, err %d", err);
		return err;
	}

	LOG_INF("Configuring terrestrial LTE-M + NB-IoT, LTE-M preferred");
	err = lte_lc_system_mode_set(LTE_LC_SYSTEM_MODE_LTEM_NBIOT,
				     LTE_LC_SYSTEM_MODE_PREFER_LTEM_PLMN_PRIO);
	if (err) {
		LOG_ERR("lte_lc_system_mode_set failed, err %d", err);
		return err;
	}

	if (strlen(CONFIG_LTE_UDP_APN) > 0) {
		LOG_INF("Configuring APN: %s", CONFIG_LTE_UDP_APN);
		err = nrf_modem_at_printf("AT+CGDCONT=0,\"IP\",\"%s\"", CONFIG_LTE_UDP_APN);
		if (err) {
			LOG_ERR("APN configuration failed, err %d", err);
			return err;
		}
	}

	return 0;
}

static int udp_send(uint32_t counter)
{
	int fd;
	int err;
	char payload[32];
	struct sockaddr_in server = {
		.sin_family = AF_INET,
		.sin_port = htons(CONFIG_LTE_UDP_SERVER_PORT),
	};

	if (strcmp(CONFIG_LTE_UDP_SERVER_ADDRESS, "CHANGE_ME") == 0) {
		LOG_ERR("Set CONFIG_LTE_UDP_SERVER_ADDRESS to the EC2 public IPv4 address");
		return -EINVAL;
	}

	err = inet_pton(AF_INET, CONFIG_LTE_UDP_SERVER_ADDRESS, &server.sin_addr);
	if (err != 1) {
		LOG_ERR("Invalid server IPv4 address: %s", CONFIG_LTE_UDP_SERVER_ADDRESS);
		return -EINVAL;
	}

	snprintk(payload, sizeof(payload), "Hello 9151 %u", counter);

	fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (fd < 0) {
		LOG_ERR("socket() failed, errno %d", errno);
		return -errno;
	}

	err = sendto(fd, payload, strlen(payload), 0, (struct sockaddr *)&server, sizeof(server));
	if (err < 0) {
		LOG_ERR("sendto() failed, errno %d", errno);
		(void)close(fd);
		return -errno;
	}

	LOG_INF("Sent UDP payload to %s:%d: %s",
		CONFIG_LTE_UDP_SERVER_ADDRESS, CONFIG_LTE_UDP_SERVER_PORT, payload);

	(void)close(fd);
	return 0;
}

static void lte_handler(const struct lte_lc_evt *const evt)
{
	switch (evt->type) {
	case LTE_LC_EVT_NW_REG_STATUS:
		LOG_INF("Network registration: %s", reg_status_str(evt->nw_reg_status));
		lte_ready = registered(evt->nw_reg_status);

		if (lte_ready && !sim_status_printed) {
			sim_status_printed = true;
			k_work_submit(&sim_status_work);
		}

		if (!lte_ready && evt->nw_reg_status == LTE_LC_NW_REG_NO_SUITABLE_CELL) {
			k_work_reschedule(&reconnect_work,
					  K_SECONDS(CONFIG_LTE_UDP_RECONNECT_DELAY_SECONDS));
		}
		break;
	case LTE_LC_EVT_RRC_UPDATE:
		LOG_INF("RRC mode: %s",
			evt->rrc_mode == LTE_LC_RRC_MODE_CONNECTED ? "connected" : "idle");
		break;
	case LTE_LC_EVT_CELL_UPDATE:
		LOG_INF("Cell update: id=%d tac=%d", evt->cell.id, evt->cell.tac);
		break;
	default:
		break;
	}
}

static void connect_work_fn(struct k_work *work)
{
	int err;

	LOG_INF("Connecting to terrestrial LTE network");
	err = lte_lc_connect_async(lte_handler);
	if (err) {
		LOG_ERR("lte_lc_connect_async failed, err %d", err);
		k_work_reschedule(&reconnect_work,
				  K_SECONDS(CONFIG_LTE_UDP_RECONNECT_DELAY_SECONDS));
	}
}

int main(void)
{
	int err;

	LOG_INF("nRF9151 terrestrial LTE UDP button POC started");

	err = dk_buttons_init(button_handler);
	if (err) {
		LOG_ERR("dk_buttons_init failed, err %d", err);
		return 0;
	}

	err = nrf_modem_lib_init();
	if (err) {
		LOG_ERR("nrf_modem_lib_init failed, err %d", err);
		return 0;
	}

	err = modem_info_init();
	if (err) {
		LOG_ERR("modem_info_init failed, err %d", err);
		return 0;
	}

	(void)at_print_response("Modem FW", "AT+CGMR");

	err = lte_lc_offline();
	if (err) {
		LOG_WRN("lte_lc_offline failed, err %d", err);
	}

	err = acquire_gnss_fix();
	if (err) {
		return 0;
	}

	err = configure_modem();
	if (err) {
		return 0;
	}

	k_work_schedule(&reconnect_work, K_NO_WAIT);

	while (true) {
		uint32_t events = k_event_wait(&app_events, BUTTON_SEND_EVENT, true, K_FOREVER);

		if ((events & BUTTON_SEND_EVENT) == 0) {
			continue;
		}

		uint32_t count = (uint32_t)atomic_get(&button_counter);

		if (!lte_ready) {
			LOG_WRN("BUTTON 1 pressed, but LTE is not ready. Counter=%u", count);
			k_work_reschedule(&reconnect_work, K_NO_WAIT);
			continue;
		}

		(void)udp_send(count);
	}
}
