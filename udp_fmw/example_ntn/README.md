# NTN GEO UDP POC

Simple POC for sending UDP packets over NTN GEO with the nRF9151 SMA DK.

The app gets its own GNSS fix, injects that location into the modem with
`AT%LOCATION`, configures NTN with AT commands, and sends the payload with the
native Zephyr/nRF modem UDP socket API. It sends the first payload automatically
after NTN attach; after that, it sends only when Button 1 is pressed.

## Before Testing

Check:

- Kit: nRF9151 SMA DK with an `nRF9151 LACA A1A` module.
- Modem firmware: `MFW_nRF9151-NTN_1.0.x-x`.
- SIM enabled for NTN/Skylo. For Monogoto, use APN `data.mono`.
- GNSS and LTE/NTN antennas connected with open sky view.
- Public UDP server accepting packets on the configured port.

SIM from the kit: use Monogoto for the first NTN/Skylo test. Deutsche Telekom
can work if activated and NTN roaming is enabled in its portal. The Onomondo SIM
from the kit is for terrestrial networks and is not the right choice for this
NTN test.

## Configure

Edit the local overlay ignored by git:

```text
boards/nrf9151_nrf9151_udp_ip_ntn_ns.conf
```

Required fields:

```conf
CONFIG_NTN_GEO_UDP_SERVER_ADDRESS="xxx.xxx.xxx.xxx"
CONFIG_NTN_GEO_UDP_SERVER_PORT=9000
CONFIG_NTN_GEO_UDP_APN="data.mono"
CONFIG_NTN_GEO_UDP_BANDLOCK="255"
```

For another SIM/provider, change only the APN. If the provider tells you to use
`cm`, set `CONFIG_NTN_GEO_UDP_APN="cm"`.

## Build and Flash

```bash
west build -p -b nrf9151dk/nrf9151/ns . -- -DEXTRA_CONF_FILE=boards/nrf9151_nrf9151_udp_ip_ntn_ns.conf
west flash
```

## Implemented Flow

```text
AT+CGMM / AT%HWVERSION / AT+CGMR / AT+CPIN?
AT+CFUN=4
AT%XSYSTEMMODE=0,0,1,0,0
AT+CFUN=31
nrf_modem_gnss_start()
wait for valid PVT fix
nrf_modem_gnss_stop()
AT+CFUN=4
AT%XSYSTEMMODE=0,0,0,0,1
AT%XBANDLOCK=2,,"255"
AT%LOCATION=2,"<gnss_lat>","<gnss_lon>","<gnss_alt>",<gnss_accuracy>,0
AT%LOCATION?
AT+CGDCONT=0,"ip","<apn>"
AT%MDMEV=2
AT+CEREG=5
AT+CGEREP=1
AT+CIND=1,1,1
AT+CNEC=24
AT+CSCON=3
AT+CFUN=1
sendto() UDP once after attach
wait for Button 1
sendto() UDP on each Button 1 press
```
