#!/usr/bin/env sh
cd "$(dirname "$0")" || exit 1
exec python3 ble_card_client.py
