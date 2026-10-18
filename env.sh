D="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"
export ARDUINO_DIRECTORIES_DATA=$D/.arduino/data ARDUINO_DIRECTORIES_USER=$D/.arduino/user ARDUINO_DIRECTORIES_DOWNLOADS=$D/.arduino/dl
export ARDUINO_BOARD_MANAGER_ADDITIONAL_URLS=https://static-cdn.m5stack.com/resource/arduino/package_m5stack_index.json
