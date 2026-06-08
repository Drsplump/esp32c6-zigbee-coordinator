"""
PlatformIO extra_script: adds Zigbee (ZBOSS) libraries for ESP32-C6.
Required because board_build.zigbee.mode is not supported by pioarduino 53.x.
"""
import os
Import("env")

platform = env.PioPlatform()
sdk_dir = platform.get_package_dir("framework-arduinoespressif32-libs")
if not sdk_dir:
    print("WARNING: framework-arduinoespressif32-libs not found, Zigbee won't link!")
else:
    zigbee_lib_dir = os.path.join(sdk_dir, "esp32c6", "lib")
    print(f"[Zigbee] Adding lib path: {zigbee_lib_dir}")
    env.Append(LIBPATH=[zigbee_lib_dir])
    # esp_zb_api_zczr = high-level Zigbee API (coordinator/router role)
    # zboss_stack.zczr = ZBOSS stack (coordinator/router role)
    # zboss_port       = HAL portability layer
    env.Append(LIBS=["esp_zb_api_zczr", "zboss_stack.zczr", "zboss_port"])
