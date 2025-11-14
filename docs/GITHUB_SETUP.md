# GitHub Repository Setup Checklist

This document provides a checklist for setting up this project on GitHub.

## Pre-Upload Checklist

### 1. Clean Build Artifacts

```bash
# Remove build artifacts
pio run --target clean
rm -rf .pio/build

# Remove temporary files
rm -f *.o *.a *.elf *.bin
rm -f dependencies.lock
rm -f sdkconfig*
rm -f main.cpp.o

# Verify .gitignore is working
git status
```

### 2. Verify Required Files

Ensure these integration files are present:

- [ ] `components/libsmb2/library.json`
- [ ] `components/libsmb2/library_build.py`
- [ ] `components/libsmb2/lib/esp_compat_wrapper.h`
- [ ] `scripts/setup_libsmb2.sh`
- [ ] `docs/LIBSMB2_INTEGRATION.md`
- [ ] `docs/BUILD_TROUBLESHOOTING.md`
- [ ] `docs/FEATURE_FLAGS.md`

### 3. Documentation Review

- [ ] README.md is up to date
- [ ] All documentation links work
- [ ] Code examples are correct
- [ ] Configuration examples are valid

### 4. Test Build

```bash
# Clean build test
pio run --target clean
pio run

# Verify success
# Expected: "SUCCESS" message with firmware size
```

## GitHub Repository Setup

### 1. Create Repository

```bash
# Initialize git if not already done
git init

# Add remote
git remote add origin https://github.com/YOUR_USERNAME/YOUR_REPO.git
```

### 2. Prepare Commit

```bash
# Check what will be committed
git status

# Add files
git add .

# Review changes
git diff --cached
```

### 3. Initial Commit

```bash
git commit -m "Initial commit: ESP32 CPAP data uploader with SMB support

Features:
- SD card file management with CPAP machine sharing
- WiFi connectivity with station mode
- SMB/CIFS upload support via libsmb2
- Schedule-based uploads with NTP synchronization
- Upload state persistence and retry logic
- Feature flags for compile-time backend selection

Includes:
- Complete libsmb2 integration for ESP32 Arduino
- Comprehensive documentation
- Setup scripts for easy deployment
- Build troubleshooting guide"
```

### 4. Push to GitHub

```bash
# Push to main branch
git push -u origin main

# Or if using master
git push -u origin master
```

## Repository Configuration

### 1. Add Repository Description

**Suggested description:**
```
ESP32-based automatic CPAP data uploader with SMB/CIFS support. Uploads SD card files to network shares on a schedule while respecting CPAP machine access.
```

### 2. Add Topics/Tags

Suggested tags:
- `esp32`
- `cpap`
- `smb`
- `arduino`
- `platformio`
- `iot`
- `backup`
- `file-upload`
- `libsmb2`

### 3. Create README Badges (Optional)

Add to top of README.md:

```markdown
[![PlatformIO CI](https://github.com/YOUR_USERNAME/YOUR_REPO/workflows/PlatformIO%20CI/badge.svg)](https://github.com/YOUR_USERNAME/YOUR_REPO/actions)
[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
```

### 4. Add License

Create a `LICENSE` file. Suggested licenses:
- MIT License (permissive)
- GPL v3 (copyleft, compatible with libsmb2's LGPL)

**Note:** libsmb2 is LGPL-2.1, which allows linking with proprietary code but requires sharing modifications to libsmb2 itself.

## Post-Upload Tasks

### 1. Verify Repository

- [ ] All files uploaded correctly
- [ ] README displays properly
- [ ] Links in documentation work
- [ ] Code syntax highlighting works

### 2. Test Clone and Build

```bash
# Clone in a new directory
cd /tmp
git clone https://github.com/YOUR_USERNAME/YOUR_REPO.git
cd YOUR_REPO

# Run setup script
./scripts/setup_libsmb2.sh

# Build
pio run
```

### 3. Create Releases (Optional)

For stable versions:

```bash
# Tag a release
git tag -a v1.0.0 -m "Release v1.0.0: Initial stable release"
git push origin v1.0.0
```

Then create a release on GitHub with:
- Release notes
- Compiled firmware binary (optional)
- Configuration examples

### 4. Set Up GitHub Actions (Optional)

Create `.github/workflows/build.yml`:

```yaml
name: PlatformIO CI

on: [push, pull_request]

jobs:
  build:
    runs-on: ubuntu-latest
    steps:
    - uses: actions/checkout@v3
      with:
        submodules: recursive
    
    - name: Set up Python
      uses: actions/setup-python@v4
      with:
        python-version: '3.x'
    
    - name: Install PlatformIO
      run: |
        python -m pip install --upgrade pip
        pip install platformio
    
    - name: Clone libsmb2
      run: |
        git clone https://github.com/sahlberg/libsmb2.git components/libsmb2
    
    - name: Build firmware
      run: pio run
```

## Submodule Setup (Alternative)

If you want libsmb2 as a git submodule instead of requiring manual clone:

```bash
# Add libsmb2 as submodule
git submodule add https://github.com/sahlberg/libsmb2.git components/libsmb2

# Commit submodule
git add .gitmodules components/libsmb2
git commit -m "Add libsmb2 as submodule"

# Push
git push
```

**Update README.md clone instructions:**
```bash
# Clone with submodules
git clone --recursive https://github.com/YOUR_USERNAME/YOUR_REPO.git

# Or if already cloned
git submodule update --init --recursive
```

**Pros:**
- Automatic version tracking
- Easier for users (no manual clone needed)

**Cons:**
- Larger repository
- Submodule management complexity

## Documentation Structure

Ensure your repository has this structure:

```
├── README.md                          # Main documentation
├── LICENSE                            # License file
├── .gitignore                         # Git ignore rules
├── platformio.ini                     # PlatformIO config
├── docs/
│   ├── LIBSMB2_INTEGRATION.md        # libsmb2 setup guide
│   ├── BUILD_TROUBLESHOOTING.md      # Build help
│   ├── FEATURE_FLAGS.md              # Feature configuration
│   └── GITHUB_SETUP.md               # This file
├── scripts/
│   └── setup_libsmb2.sh              # Setup script
├── src/                               # Source code
├── include/                           # Headers
└── components/
    └── libsmb2/                       # SMB library (git clone or submodule)
        ├── library.json               # PlatformIO manifest
        ├── library_build.py           # Build script
        └── lib/
            └── esp_compat_wrapper.h   # ESP32 compatibility
```

## Maintenance

### Regular Updates

```bash
# Update libsmb2
cd components/libsmb2
git pull
cd ../..

# Test build
pio run

# Commit if successful
git add components/libsmb2
git commit -m "Update libsmb2 to latest version"
git push
```

### Version Tagging

Use semantic versioning (MAJOR.MINOR.PATCH):
- MAJOR: Breaking changes
- MINOR: New features (backward compatible)
- PATCH: Bug fixes

```bash
git tag -a v1.0.1 -m "Fix: SMB connection timeout handling"
git push origin v1.0.1
```

## Support

Add a SUPPORT.md or CONTRIBUTING.md file with:
- How to report issues
- How to contribute
- Code style guidelines
- Testing requirements

## Security

If handling sensitive data:
- Add SECURITY.md with security policy
- Enable GitHub security features
- Document secure configuration practices
- Never commit credentials or secrets

## Checklist Summary

Before pushing to GitHub:

- [ ] Clean build artifacts removed
- [ ] All integration files present
- [ ] Documentation complete and accurate
- [ ] Test build successful
- [ ] .gitignore configured
- [ ] License file added
- [ ] README badges added (optional)
- [ ] Repository description set
- [ ] Topics/tags added
- [ ] Test clone and build from fresh directory
- [ ] GitHub Actions configured (optional)
- [ ] Submodules configured (if using)

After pushing:

- [ ] Verify all files uploaded
- [ ] Test clone from GitHub
- [ ] Create initial release (optional)
- [ ] Add issues/discussions (optional)
- [ ] Share with community
