#include "WebStatus.h"

char g_webStatusBuf[WEB_STATUS_BUF_SIZE] = "{\"state\":\"BOOT\"}";
char g_webConfigBuf[WEB_CONFIG_BUF_SIZE] = "{}";

volatile SessionStatus g_smbSessionStatus   = { false, "", 0, 0 };
volatile SessionStatus g_cloudSessionStatus = { false, "", 0, 0 };

BackendSummaryStatus g_activeBackendStatus   = { "NONE", 0, 0, 0, 0, false };
BackendSummaryStatus g_inactiveBackendStatus = { "NONE", 0, 0, 0, 0, false };
