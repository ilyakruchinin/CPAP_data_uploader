#pragma once
#include <Arduino.h>

// ============================================================================
// Zero-heap web data buffers
// ============================================================================
// These static char arrays are written by the main task (TestWebServer) and
// served directly by web handlers — no malloc in the request/response path.
//
// g_webStatusBuf : rebuilt every ~3 s in the main loop via updateStatusSnapshot()
// g_webConfigBuf : built once at boot in initConfigSnapshot()
//
// g_uploadSessionStatus : written by the upload task (FileUploader); read by
// updateStatusSnapshot() in the main task. No mutex needed — torn reads on a
// status display are harmless.

static const size_t WEB_STATUS_BUF_SIZE = 896;
static const size_t WEB_CONFIG_BUF_SIZE = 1024;

extern char g_webStatusBuf[WEB_STATUS_BUF_SIZE];
extern char g_webConfigBuf[WEB_CONFIG_BUF_SIZE];

struct SessionStatus {
    bool uploadActive;
    char currentFolder[33];
    int  filesUploaded;
    int  filesTotal;
};

extern volatile SessionStatus g_smbSessionStatus;
extern volatile SessionStatus g_cloudSessionStatus;
