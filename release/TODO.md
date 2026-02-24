# TODO

All network pre-connect optimizations have been implemented in v0.10.3:

- ✅ Cloud pre-connect removed from LISTENING (TLS only after preflight confirms work)
- ✅ SMB pre-connect removed from LISTENING (connects on-demand in FileUploader)

Both backends now connect on-demand when actual work is confirmed.
