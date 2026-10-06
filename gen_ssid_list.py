#!/usr/bin/env python3
"""Sinh ssid_list.bin cho R3a, R3b va R3c (100,000 SSID).

Moi record co dinh 64 bytes: 32 bytes SSID (ASCII) + 32 bytes Pass (ASCII).
Tong kich thuoc: 100,000 * 64 bytes = 6,400,000 bytes (6.4 MB).
Record 0 (index 0) CO DINH = AP_SSID/AP_PASS mac dinh de handshake khoi dau.
"""
import os
import random
import time

TOTAL_COUNT = 100000
RECORD_SIZE = 64  # 32 bytes SSID + 32 bytes Pass

DEFAULT_ENTRY = {
    "ssid": "1A3bcba8b69c7aea0c5942b5ce87f102",
    "pass": "d1b07167d06adf161a21d4096041d40b",
}

HEX = "0123456789abcdef"


def rand_ssid():
    return "1A" + "".join(random.choice(HEX) for _ in range(30))


def rand_pass():
    return "".join(random.choice(HEX) for _ in range(32))


def generate_bin_file(target_path: str, count: int = TOTAL_COUNT):
    os.makedirs(os.path.dirname(os.path.abspath(target_path)), exist_ok=True)
    t0 = time.time()
    
    # We will write in chunks of 5,000 entries (320 KB per chunk)
    chunk_size = 5000
    total_written = 0
    
    with open(target_path, "wb") as f:
        # First entry: index 0
        buf = bytearray()
        s0 = DEFAULT_ENTRY["ssid"].encode("ascii").ljust(32, b"\x00")[:32]
        p0 = DEFAULT_ENTRY["pass"].encode("ascii").ljust(32, b"\x00")[:32]
        buf.extend(s0 + p0)
        total_written += 1
        
        while total_written < count:
            batch_target = min(chunk_size, count - total_written)
            for _ in range(batch_target):
                s = rand_ssid().encode("ascii")[:32]
                p = rand_pass().encode("ascii")[:32]
                buf.extend(s + p)
            f.write(buf)
            total_written += batch_target
            buf.clear()
            
    dur = time.time() - t0
    print(f"Generated {target_path} ({os.path.getsize(target_path)} bytes, {count} records) in {dur:.2f}s")


def main():
    destinations = [
        "E:/Phantom_esp_r3b/R3b_S3/ssid_list.bin",
        "E:/Phantom_esp_r3b/ssid_list.bin",
        "E:/Phantom_esp_r3c/R3b_S3/ssid_list.bin",
        "E:/Phantom_esp_r3c/ssid_list.bin",
        "E:/Phantom_esp_r3a/pi2w_pi_package_i2s_v3/ssid_list.bin",
        "E:/Phantom_esp_r3a/ssid_list.bin",
    ]

    primary_path = destinations[0]
    generate_bin_file(primary_path, TOTAL_COUNT)

    # Copy to all other destinations so all devices have the EXACT same binary file
    with open(primary_path, "rb") as f_src:
        content = f_src.read()

    for dest in destinations[1:]:
        try:
            os.makedirs(os.path.dirname(os.path.abspath(dest)), exist_ok=True)
            with open(dest, "wb") as f_dst:
                f_dst.write(content)
            print(f"Synced to {dest} ({len(content)} bytes)")
        except Exception as e:
            print(f"Skip {dest}: {e}")


if __name__ == "__main__":
    main()