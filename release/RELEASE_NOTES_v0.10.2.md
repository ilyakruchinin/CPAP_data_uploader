# CPAP Data Uploader v0.10.2 Release Notes

## Features & Improvements

### 🆘 Emergency Boot Error Dump
**Feature:** If the system suffers a fatal boot error that prevents WiFi from starting (such as a syntax error in `config.txt`, a missing config file, or an incorrect WiFi password), it will now dump the diagnostic crash logs directly to a new `/uploader_error.txt` file on the root of the physical SD card. 
**Benefit:** This ensures you can always retrieve the exact failure reason and debug the system by plugging the SD card directly into a computer, even if the device's Web UI is unreachable.
