/*
 * NTN GEO UDP proof of concept for nRF9151 DK.
 *
 * The setup path intentionally follows the AT command flow from the Nordic
 * NTN operation examples:
 *   CFUN off -> XSYSTEMMODE NTN -> XBANDLOCK -> LOCATION -> CGDCONT ->
 *   notifications -> CFUN on -> UDP socket send.
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

#include <dk_buttons_and_leds.h>
#include <modem/at_monitor.h>
#include <modem/lte_lc.h>
#include <modem/nrf_modem_lib.h>
#include <nrf_modem_at.h>
#include <nrf_modem_gnss.h>

LOG_MODULE_REGISTER(ntn_geo_udp, CONFIG_NTN_GEO_UDP_LOG_LEVEL);

static const char udp_payload[] = "hello nrf 091";

static K_SEM_DEFINE(ntn_connected_sem, 0, 1);
static K_SEM_DEFINE(gnss_fix_sem, 0, 1);
static K_SEM_DEFINE(button_send_sem, 0, 1);

static volatile bool ntn_registered;
static volatile bool pdn_active;
static struct nrf_modem_gnss_pvt_data_frame last_pvt;

struct ntn_location {
	char latitude[16];
	char longitude[16];
	char altitude[16];
	int accuracy;
};

static void at_notif_mon(const char *notif);
static void button_handler(uint32_t button_states, uint32_t has_changed);
static void gnss_event_handler(int event);
static void lte_lc_handler(const struct lte_lc_evt *const evt);

AT_MONITOR(cereg_monitor, "CEREG", at_notif_mon);
AT_MONITOR(cnec_monitor, "CNEC", at_notif_mon);
AT_MONITOR(cgerep_monitor, "CGEREP", at_notif_mon);
AT_MONITOR(cind_monitor, "CIND", at_notif_mon);
AT_MONITOR(cscon_monitor, "CSCON", at_notif_mon);
AT_MONITOR(mdmev_monitor, "%MDMEV", at_notif_mon);

static void at_notif_mon(const char *notif)
{
	LOG_INF("AT notification: %s", notif);
}

static void button_handler(uint32_t button_states, uint32_t has_changed)
{
	if ((has_changed & DK_BTN1_MSK) && (button_states & DK_BTN1_MSK)) {
		k_sem_give(&button_send_sem);
	}
}

static void gnss_event_handler(int event)
{
	int err;

	if (event != NRF_MODEM_GNSS_EVT_PVT) {
		return;
	}

	err = nrf_modem_gnss_read(&last_pvt, sizeof(last_pvt), NRF_MODEM_GNSS_DATA_PVT);
	if (err) {
		LOG_WRN("GNSS PVT read failed, err %d", err);
		return;
	}

	if (last_pvt.flags & NRF_MODEM_GNSS_PVT_FLAG_FIX_VALID) {
		LOG_INF("GNSS PVT valid: lat=%.08f lon=%.08f alt=%.1f acc=%.1f",
			last_pvt.latitude, last_pvt.longitude, (double)last_pvt.altitude,
			(double)last_pvt.accuracy);
		k_sem_give(&gnss_fix_sem);
	} else {
		LOG_INF("GNSS PVT waiting for valid fix");
	}
}

static int validate_config(void)
{
	struct sockaddr_in server;

	if (strcmp(CONFIG_NTN_GEO_UDP_SERVER_ADDRESS, "CHANGE_ME") == 0) {
		LOG_ERR("Set CONFIG_NTN_GEO_UDP_SERVER_ADDRESS in a local boards/*.conf file");
		return -EINVAL;
	}

	if (inet_pton(AF_INET, CONFIG_NTN_GEO_UDP_SERVER_ADDRESS, &server.sin_addr) != 1) {
		LOG_ERR("Invalid server IPv4 address: %s", CONFIG_NTN_GEO_UDP_SERVER_ADDRESS);
		return -EINVAL;
	}

	if (CONFIG_NTN_GEO_UDP_SERVER_PORT <= 0 || CONFIG_NTN_GEO_UDP_SERVER_PORT > 65535) {
		LOG_ERR("Invalid UDP port: %d", CONFIG_NTN_GEO_UDP_SERVER_PORT);
		return -EINVAL;
	}

	LOG_INF("UDP target: %s:%d", CONFIG_NTN_GEO_UDP_SERVER_ADDRESS,
		CONFIG_NTN_GEO_UDP_SERVER_PORT);

	if (strlen(CONFIG_NTN_GEO_UDP_APN) == 0) {
		LOG_WRN("No NTN APN configured; relying on SIM/operator default context");
	} else {
		LOG_INF("NTN APN configured");
	}

	if (strstr(CONFIG_NTN_GEO_UDP_BANDLOCK, "255") == NULL) {
		LOG_WRN("Brazil GEO NTN tests are expected on B255; current band lock is '%s'",
			CONFIG_NTN_GEO_UDP_BANDLOCK);
	}

	return 0;
}

static int acquire_gnss_location(struct ntn_location *location)
{
	int err;
	int accuracy;

	k_sem_reset(&gnss_fix_sem);

	LOG_INF("Getting GNSS fix before NTN attach");

	LOG_INF("AT> AT+CFUN=4");
	err = nrf_modem_at_printf("AT+CFUN=4");
	if (err) {
		LOG_ERR("AT+CFUN=4 before GNSS failed, err %d", err);
		return err;
	}

	LOG_INF("AT> AT%%XSYSTEMMODE=0,0,1,0,0");
	err = nrf_modem_at_printf("AT%%XSYSTEMMODE=0,0,1,0,0");
	if (err) {
		LOG_ERR("AT%%XSYSTEMMODE GNSS failed, err %d", err);
		return err;
	}

	err = nrf_modem_gnss_event_handler_set(gnss_event_handler);
	if (err) {
		LOG_ERR("nrf_modem_gnss_event_handler_set failed, err %d", err);
		return err;
	}

	err = nrf_modem_gnss_fix_interval_set(1);
	if (err) {
		LOG_ERR("nrf_modem_gnss_fix_interval_set failed, err %d", err);
		return err;
	}

	err = nrf_modem_gnss_fix_retry_set(CONFIG_NTN_GEO_UDP_GNSS_TIMEOUT_SECONDS);
	if (err) {
		LOG_ERR("nrf_modem_gnss_fix_retry_set failed, err %d", err);
		return err;
	}

	LOG_INF("AT> AT+CFUN=31");
	err = nrf_modem_at_printf("AT+CFUN=31");
	if (err) {
		LOG_ERR("AT+CFUN=31 failed, err %d", err);
		return err;
	}

	err = nrf_modem_gnss_start();
	if (err) {
		LOG_ERR("nrf_modem_gnss_start failed, err %d", err);
		return err;
	}

	LOG_INF("Waiting up to %d seconds for GNSS fix",
		CONFIG_NTN_GEO_UDP_GNSS_TIMEOUT_SECONDS);

	if (k_sem_take(&gnss_fix_sem, K_SECONDS(CONFIG_NTN_GEO_UDP_GNSS_TIMEOUT_SECONDS)) != 0) {
		(void)nrf_modem_gnss_stop();
		LOG_ERR("GNSS fix timeout");
		return -ETIMEDOUT;
	}

	(void)nrf_modem_gnss_stop();

	accuracy = (int)(last_pvt.accuracy + 0.5f);
	if (accuracy <= 0) {
		accuracy = 10;
	}

	snprintf(location->latitude, sizeof(location->latitude), "%.08f", last_pvt.latitude);
	snprintf(location->longitude, sizeof(location->longitude), "%.08f", last_pvt.longitude);
	snprintf(location->altitude, sizeof(location->altitude), "%.0f", (double)last_pvt.altitude);
	location->accuracy = accuracy;

	LOG_INF("GNSS fix selected for NTN: lat=%s lon=%s alt=%s accuracy=%d",
		location->latitude, location->longitude, location->altitude, location->accuracy);

	return 0;
}

static int check_sim_ready(void)
{
	int err;
	char response[128];

	err = nrf_modem_at_cmd(response, sizeof(response), "AT+CPIN?");
	if (err) {
		LOG_ERR("SIM check failed, err %d", err);
		return err;
	}

	if (!strstr(response, "READY")) {
		LOG_WRN("SIM status is not READY: %s", response);
		return -EACCES;
	}

	LOG_INF("SIM status: READY");

	return 0;
}

static int configure_ntn_radio(const struct ntn_location *location)
{
	int err;
	char response[512];

	LOG_INF("AT> AT+CMEE=1");
	err = nrf_modem_at_printf("AT+CMEE=1");
	if (err) {
		LOG_ERR("AT+CMEE failed, err %d", err);
		return err;
	}

	LOG_INF("AT> AT+CFUN=4");
	err = nrf_modem_at_printf("AT+CFUN=4");
	if (err) {
		LOG_ERR("AT+CFUN=4 failed, err %d", err);
		return err;
	}

	LOG_INF("AT> AT%%XSYSTEMMODE=0,0,0,0,1");
	err = nrf_modem_at_printf("AT%%XSYSTEMMODE=0,0,0,0,1");
	if (err) {
		LOG_ERR("AT%%XSYSTEMMODE failed, err %d", err);
		return err;
	}

	err = nrf_modem_at_cmd(response, sizeof(response), "AT%%XSYSTEMMODE?");
	if (err) {
		LOG_WRN("AT%%XSYSTEMMODE? failed, err %d", err);
	} else {
		LOG_INF("System mode after set: %s", response);
	}

	if (strlen(CONFIG_NTN_GEO_UDP_BANDLOCK) > 0) {
		LOG_INF("AT> AT%%XBANDLOCK=2,,\"%s\"", CONFIG_NTN_GEO_UDP_BANDLOCK);
		err = nrf_modem_at_printf("AT%%XBANDLOCK=2,,\"%s\"",
					  CONFIG_NTN_GEO_UDP_BANDLOCK);
		if (err) {
			LOG_ERR("AT%%XBANDLOCK failed, err %d", err);
			return err;
		}
		LOG_INF("NTN band lock: %s", CONFIG_NTN_GEO_UDP_BANDLOCK);

		err = nrf_modem_at_cmd(response, sizeof(response), "AT%%XBANDLOCK?");
		if (err) {
			LOG_WRN("AT%%XBANDLOCK? failed, err %d", err);
		} else {
			LOG_INF("Band lock after set: %s", response);
		}
	}

	LOG_INF("AT> AT%%LOCATION=2,\"%s\",\"%s\",\"%s\",%d,%d",
		location->latitude,
		location->longitude,
		location->altitude,
		location->accuracy,
		CONFIG_NTN_GEO_UDP_LOCATION_VALIDITY_SECONDS);
	err = nrf_modem_at_printf("AT%%LOCATION=2,\"%s\",\"%s\",\"%s\",%d,%d",
				  location->latitude,
				  location->longitude,
				  location->altitude,
				  location->accuracy,
				  CONFIG_NTN_GEO_UDP_LOCATION_VALIDITY_SECONDS);
	if (err) {
		LOG_ERR("AT%%LOCATION failed, err %d", err);
		return err;
	}

	LOG_INF("NTN location set: lat=%s lon=%s alt=%s accuracy=%d validity=%d",
		location->latitude,
		location->longitude,
		location->altitude,
		location->accuracy,
		CONFIG_NTN_GEO_UDP_LOCATION_VALIDITY_SECONDS);

	err = nrf_modem_at_cmd(response, sizeof(response), "AT%%LOCATION?");
	if (err) {
		LOG_WRN("AT%%LOCATION? failed, err %d", err);
	} else {
		LOG_INF("Location after set: %s", response);
	}

	if (strlen(CONFIG_NTN_GEO_UDP_APN) > 0) {
		LOG_INF("AT> AT+CGDCONT=0,\"ip\",\"%s\"", CONFIG_NTN_GEO_UDP_APN);
		err = nrf_modem_at_printf("AT+CGDCONT=0,\"ip\",\"%s\"", CONFIG_NTN_GEO_UDP_APN);
		if (err) {
			LOG_ERR("AT+CGDCONT failed, err %d", err);
			return err;
		}
		LOG_INF("NTN APN configured: %s", CONFIG_NTN_GEO_UDP_APN);
	} else {
		LOG_INF("No APN configured; using SIM/operator default PDP context");
	}

	return 0;
}

static int configure_ntn_notifications(void)
{
	int err;
	char response[512];

	LOG_INF("AT> AT%%MDMEV=2");
	err = nrf_modem_at_printf("AT%%MDMEV=2");
	if (err) {
		LOG_ERR("AT%%MDMEV failed, err %d", err);
		return err;
	}

	LOG_INF("AT> AT+CEREG=5");
	err = nrf_modem_at_printf("AT+CEREG=5");
	if (err) {
		LOG_ERR("AT+CEREG failed, err %d", err);
		return err;
	}

	LOG_INF("AT> AT+CGEREP=1");
	err = nrf_modem_at_printf("AT+CGEREP=1");
	if (err) {
		LOG_ERR("AT+CGEREP failed, err %d", err);
		return err;
	}

	LOG_INF("AT> AT+CIND=1,1,1");
	err = nrf_modem_at_printf("AT+CIND=1,1,1");
	if (err) {
		LOG_ERR("AT+CIND failed, err %d", err);
		return err;
	}

	LOG_INF("AT> AT+CNEC=24");
	err = nrf_modem_at_printf("AT+CNEC=24");
	if (err) {
		LOG_ERR("AT+CNEC failed, err %d", err);
		return err;
	}

	err = nrf_modem_at_cmd(response, sizeof(response), "AT+CNEC?");
	if (err) {
		LOG_WRN("AT+CNEC? failed, err %d", err);
	} else {
		LOG_INF("Network error reporting: %s", response);
	}

	LOG_INF("AT> AT+CSCON=3");
	err = nrf_modem_at_printf("AT+CSCON=3");
	if (err) {
		LOG_ERR("AT+CSCON failed, err %d", err);
		return err;
	}

	return 0;
}

static void log_modem_snapshot(const char *reason)
{
	int err;
	char response[512];

	LOG_INF("Modem snapshot: %s", reason);

	err = nrf_modem_at_cmd(response, sizeof(response), "AT+CEREG?");
	if (err) {
		LOG_WRN("AT+CEREG? failed, err %d", err);
	} else {
		LOG_INF("AT+CEREG?: %s", response);
	}

	err = nrf_modem_at_cmd(response, sizeof(response), "AT+CSCON?");
	if (err) {
		LOG_WRN("AT+CSCON? failed, err %d", err);
	} else {
		LOG_INF("AT+CSCON?: %s", response);
	}

	err = nrf_modem_at_cmd(response, sizeof(response), "AT+CGDCONT?");
	if (err) {
		LOG_WRN("AT+CGDCONT? failed, err %d", err);
	} else {
		LOG_INF("AT+CGDCONT?: %s", response);
	}

	err = nrf_modem_at_cmd(response, sizeof(response), "AT%%XSYSTEMMODE?");
	if (err) {
		LOG_WRN("AT%%XSYSTEMMODE? failed, err %d", err);
	} else {
		LOG_INF("AT%%XSYSTEMMODE?: %s", response);
	}

	err = nrf_modem_at_cmd(response, sizeof(response), "AT%%XMONITOR");
	if (err) {
		LOG_WRN("AT%%XMONITOR failed, err %d", err);
	} else {
		LOG_INF("AT%%XMONITOR: %s", response);
	}
}

static int wait_for_ntn_ready(void)
{
	LOG_INF("Waiting for NTN registration and default PDN; GEO search can take many minutes");

	if (k_sem_take(&ntn_connected_sem,
		       K_SECONDS(CONFIG_NTN_GEO_UDP_CONNECT_TIMEOUT_SECONDS)) != 0) {
		LOG_ERR("Timed out waiting for NTN registration/PDN");
		log_modem_snapshot("connection timeout");
		return -ETIMEDOUT;
	}

	LOG_INF("NTN registered and default PDN is active");

	return 0;
}

static int activate_ntn(void)
{
	int err;

	err = lte_lc_pdn_default_ctx_events_enable();
	if (err) {
		LOG_ERR("lte_lc_pdn_default_ctx_events_enable failed, err %d", err);
		return err;
	}

	LOG_INF("Activating NTN modem with AT+CFUN=1");

	LOG_INF("AT> AT+CFUN=1");
	err = nrf_modem_at_printf("AT+CFUN=1");
	if (err) {
		LOG_ERR("AT+CFUN=1 failed, err %d", err);
		return err;
	}

	/* LTE LC's PDN hook may subscribe CNEC=16 after CFUN; keep the PDF's wider diagnostics. */
	LOG_INF("AT> AT+CNEC=24");
	err = nrf_modem_at_printf("AT+CNEC=24");
	if (err) {
		LOG_WRN("AT+CNEC=24 after CFUN failed, err %d", err);
	}

	LOG_INF("AT> AT+CGEREP=1");
	err = nrf_modem_at_printf("AT+CGEREP=1");
	if (err) {
		LOG_WRN("AT+CGEREP=1 after CFUN failed, err %d", err);
	}

	return wait_for_ntn_ready();
}

static int udp_send_once(void)
{
	int fd;
	int err;
	struct sockaddr_in server = {
		.sin_family = AF_INET,
		.sin_port = htons(CONFIG_NTN_GEO_UDP_SERVER_PORT),
	};

	err = inet_pton(AF_INET, CONFIG_NTN_GEO_UDP_SERVER_ADDRESS, &server.sin_addr);
	if (err != 1) {
		LOG_ERR("Invalid server IPv4 address: %s", CONFIG_NTN_GEO_UDP_SERVER_ADDRESS);
		return -EINVAL;
	}

	fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (fd < 0) {
		LOG_ERR("socket() failed, errno %d", errno);
		return -errno;
	}

#if defined(CONFIG_NTN_GEO_UDP_RAI_ONE_RESP)
	/* CP-RAI can greatly reduce connected time when the NTN network supports it. */
	int rai = RAI_ONE_RESP;

	err = setsockopt(fd, SOL_SOCKET, SO_RAI, &rai, sizeof(rai));
	if (err) {
		LOG_WRN("SO_RAI setup failed, errno %d", errno);
	}
#endif

	err = sendto(fd, udp_payload, strlen(udp_payload), 0,
		     (struct sockaddr *)&server, sizeof(server));
	if (err < 0) {
		LOG_ERR("sendto() failed, errno %d", errno);
		(void)close(fd);
		return -errno;
	}

	LOG_INF("Sent UDP payload to %s:%d: %s",
		CONFIG_NTN_GEO_UDP_SERVER_ADDRESS,
		CONFIG_NTN_GEO_UDP_SERVER_PORT,
		udp_payload);

	log_modem_snapshot("after UDP send");

	(void)close(fd);
	return 0;
}

static void lte_lc_handler(const struct lte_lc_evt *const evt)
{
	switch (evt->type) {
	case LTE_LC_EVT_NW_REG_STATUS:
		ntn_registered = evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_HOME ||
				 evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_ROAMING;
		LOG_INF("NTN registration status: %d", evt->nw_reg_status);
		if (ntn_registered && pdn_active) {
			k_sem_give(&ntn_connected_sem);
		}
		break;
	case LTE_LC_EVT_PDN:
		switch (evt->pdn.type) {
		case LTE_LC_EVT_PDN_ACTIVATED:
			pdn_active = true;
			LOG_INF("PDN active");
			if (ntn_registered) {
				k_sem_give(&ntn_connected_sem);
			}
			break;
		case LTE_LC_EVT_PDN_DEACTIVATED:
		case LTE_LC_EVT_PDN_NETWORK_DETACH:
		case LTE_LC_EVT_PDN_CTX_DESTROYED:
			pdn_active = false;
			LOG_WRN("PDN inactive");
			break;
		case LTE_LC_EVT_PDN_ESM_ERROR:
			LOG_WRN("PDN ESM error: cid=%d esm=%d", evt->pdn.cid, evt->pdn.esm_err);
			break;
		default:
			break;
		}
		break;
	case LTE_LC_EVT_RRC_UPDATE:
		LOG_INF("RRC mode: %s",
			evt->rrc_mode == LTE_LC_RRC_MODE_CONNECTED ? "connected" : "idle");
		break;
	case LTE_LC_EVT_MODEM_EVENT:
		LOG_INF("Modem event type: %d", evt->modem_evt.type);
		break;
	default:
		break;
	}
}

int main(void)
{
	int err;
	char response[512];
	struct ntn_location location;

	LOG_INF("nRF9151 NTN GEO UDP POC started");

	err = validate_config();
	if (err) {
		return 0;
	}

	err = nrf_modem_lib_init();
	if (err) {
		LOG_ERR("nrf_modem_lib_init failed, err %d", err);
		return 0;
	}

	lte_lc_register_handler(lte_lc_handler);

	err = nrf_modem_at_cmd(response, sizeof(response), "AT+CGMM");
	if (err) {
		LOG_WRN("AT+CGMM failed, err %d", err);
	} else {
		LOG_INF("Modem model: %s", response);
	}

	err = nrf_modem_at_cmd(response, sizeof(response), "AT%%HWVERSION");
	if (err) {
		LOG_WRN("AT%%HWVERSION failed, err %d", err);
	} else {
		LOG_INF("Hardware version: %s", response);
	}

	err = nrf_modem_at_cmd(response, sizeof(response), "AT+CGMR");
	if (err) {
		LOG_WRN("AT+CGMR failed, err %d", err);
	} else {
		LOG_INF("Modem FW: %s", response);
	}

	err = nrf_modem_at_cmd(response, sizeof(response), "AT%%XSYSTEMMODE?");
	if (err) {
		LOG_WRN("AT%%XSYSTEMMODE? failed, err %d", err);
	} else {
		LOG_INF("System mode: %s", response);
	}

	(void)check_sim_ready();

	err = acquire_gnss_location(&location);
	if (err) {
		LOG_ERR("GNSS location acquisition failed");
		return 0;
	}

	err = configure_ntn_radio(&location);
	if (err) {
		LOG_ERR("NTN radio configuration failed");
		return 0;
	}

	err = configure_ntn_notifications();
	if (err) {
		LOG_ERR("NTN notification configuration failed");
		return 0;
	}

	log_modem_snapshot("after AT configuration, before CFUN=1");

	err = activate_ntn();
	if (err) {
		LOG_ERR("NTN connection failed");
		return 0;
	}

	log_modem_snapshot("before UDP send");

	err = udp_send_once();
	if (err) {
		LOG_ERR("UDP send failed");
		return 0;
	}

	err = dk_buttons_init(button_handler);
	if (err) {
		LOG_ERR("dk_buttons_init failed, err %d", err);
		return 0;
	}

	LOG_INF("First UDP payload sent; press Button 1 to send another payload");

	while (true) {
		if (k_sem_take(&button_send_sem, K_SECONDS(60)) == 0) {
			LOG_INF("Button 1 pressed; sending UDP payload");
			err = udp_send_once();
			if (err) {
				LOG_ERR("UDP send failed");
			}
		} else {
			log_modem_snapshot("periodic observation");
		}
	}
}
