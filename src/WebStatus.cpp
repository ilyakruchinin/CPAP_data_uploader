#include "WebStatus.h"

char g_webStatusBuf[WEB_STATUS_BUF_SIZE] = "{\"state\":\"BOOT\"}";
char g_webConfigBuf[WEB_CONFIG_BUF_SIZE] = "{}";

volatile UploadSessionStatus g_uploadSessionStatus = {
    "",    // currentFolder
    0,     // filesUploaded
    0,     // filesTotal
    false, // uploadActive
    ""     // lastError
};
