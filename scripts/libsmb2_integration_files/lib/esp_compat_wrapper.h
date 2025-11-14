/* ESP32 Arduino compatibility wrapper for libsmb2 */
#ifndef ESP_COMPAT_WRAPPER_H
#define ESP_COMPAT_WRAPPER_H

/* Include standard headers needed for libsmb2 on ESP32 */
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/uio.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

/* Define random functions for ESP32 */
#define smb2_random esp_random
#define smb2_srandom(seed) /* ESP32 RNG doesn't need seeding */

/* Define login_num for getlogin_r */
#define login_num ENXIO

/* Declare esp_random if not already declared */
#ifdef __cplusplus
extern "C" {
#endif

uint32_t esp_random(void);

#ifdef __cplusplus
}
#endif

#endif /* ESP_COMPAT_WRAPPER_H */
