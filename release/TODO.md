# TODO

## Completed

### v0.10.3 — Network Pre-Connect Optimizations
- ✅ Cloud pre-connect removed from LISTENING (TLS only after preflight confirms work)
- ✅ SMB pre-connect removed from LISTENING (connects on-demand in FileUploader)

Both backends now connect on-demand when actual work is confirmed.

### v2.0i-alpha3 — SMB Upload Fixes Under Heap Pressure
- ✅ Unconditional TLS cleanup before SMB phase (stale mbedTLS buffers were fragmenting heap)
- ✅ Fixed `isSmbPduAllocationError()` — was dead code (matched `"Failed to allocate pdu"` but libsmb2 overwrites with `"Failed to create * command"`)
- ✅ TCP send buffer drain every 16 KB during SMB writes (prevents EAGAIN→EBADF at 32 KB boundary)
- ✅ PDU allocation failures now classified as recoverable in open/write error handlers

## Remaining (v2.0i roadmap)

- [ ] Validate SMB large-file uploads end-to-end after alpha3 fixes (field test needed)
- [ ] Investigate further heap fragmentation reduction (UploadStateManager churn, DATALOG size-only tracking)
- [ ] Consider lwIP tuning (`CONFIG_LWIP_TCP_SND_BUF_DEFAULT`) if TCP drain pause proves insufficient
