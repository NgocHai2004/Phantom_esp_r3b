python -m platformio run -e esp32-c6-devkitc-1 -t upload --upload-port COM16      

pio run -e esp32s3_manual_boot --target upload --upload-port COM7
pio device monitor -e esp32s3_manual_boot   