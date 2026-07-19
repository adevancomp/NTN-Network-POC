from __future__ import annotations

import argparse
import logging
import socket
import sys


LOG_FORMAT = "%(asctime)s %(levelname)s %(message)s"


def decode_payload(payload: bytes) -> str:
    try:
        return payload.decode("utf-8")
    except UnicodeDecodeError:
        return payload.hex()


def run_server(host: str, port: int) -> None:
    logging.basicConfig(level=logging.INFO, format=LOG_FORMAT)

    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.bind((host, port))
        logging.info("UDP server listening on %s:%s", host, port)

        while True:
            payload, sender = sock.recvfrom(4096)
            text = decode_payload(payload)

            logging.info("RX from %s:%s bytes=%s payload=%r", sender[0], sender[1], len(payload), text)


def main() -> int:
    parser = argparse.ArgumentParser(description="Simple UDP server for NB-IoT NTN tests.")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=9000)
    args = parser.parse_args()

    try:
        run_server(args.host, args.port)
    except KeyboardInterrupt:
        print("\nStopped.", file=sys.stderr)
        return 130

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
