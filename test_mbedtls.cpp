#include <Arduino.h>
#include <mbedtls/platform.h>

void* my_calloc(size_t n, size_t size) { return calloc(n, size); }
void my_free(void* ptr) { free(ptr); }

void setup() {
#if defined(MBEDTLS_PLATFORM_MEMORY) && !defined(MBEDTLS_PLATFORM_CALLOC_MACRO)
    mbedtls_platform_set_calloc_free(my_calloc, my_free);
    Serial.println("Can use custom allocator!");
#else
    Serial.println("Cannot use custom allocator due to macros.");
#endif
}
void loop() {}
