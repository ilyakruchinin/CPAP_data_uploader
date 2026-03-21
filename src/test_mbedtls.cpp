#include <Arduino.h>
#include <mbedtls/platform.h>

void* my_calloc(size_t n, size_t size) { return calloc(n, size); }
void my_free(void* ptr) { free(ptr); }

void dummy_setup() {
    mbedtls_platform_set_calloc_free(my_calloc, my_free);
}
