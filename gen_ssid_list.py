#!/usr/bin/env python3
"""Sinh ssid_list.json cho R3b va R3c.

100 SSID co dinh 32 ky tu hex, prefix giong SSID mac dinh ...0002
nhung bo qua index 2 de khong trung SSID handshake mac dinh.

Copy file ssid_list.json vao goc SD ca R3b va R3c.
"""
import json
import os

DEFAULT_SSID = "7068616e746f6d303030303030300002"
PREFIX = DEFAULT_SSID[:28]  # 28 ky tu: '7068616e746f6d30303030303030'

ssids = [PREFIX + format(i, "04x") for i in range(101) if i != 2]
assert len(ssids) == 100
assert DEFAULT_SSID not in ssids

data = {"count": len(ssids), "ssids": ssids}

for base in ["R3b_S3", "E:/Phantom_esp_r3b/R3b_S3", "E:/Phantom_esp_r3c/R3b_S3"]:
    if os.path.isdir(base):
        path = os.path.join(base, "ssid_list.json")
        with open(path, "w", encoding="utf-8") as f:
            json.dump(data, f, indent=2)
        print(f"Wrote {path} ({os.path.getsize(path)} bytes)")

print(f"First 3: {ssids[:3]}")
print(f"Last 3:  {ssids[-3:]}")
