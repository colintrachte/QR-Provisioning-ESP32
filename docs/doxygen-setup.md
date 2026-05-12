# Doxygen Setup Guide

Doxygen generates browsable HTML API documentation from your C source comments. This project uses it to keep the help guide "baked in" with the code.

---

## Installation

### Ubuntu / Debian

```bash
sudo apt-get update
sudo apt-get install doxygen graphviz
```

### macOS

```bash
brew install doxygen graphviz
```

### Windows

Download the installer from [doxygen.nl](https://www.doxygen.nl/download.html). Graphviz is optional but recommended for call graphs.

---

## Quick Start

```bash
# From project root
cd /path/to/robot-provisioning

# Generate docs
doxygen Doxyfile

# Open in browser
open docs/api/html/index.html        # macOS
xdg-open docs/api/html/index.html    # Linux
```

---

## Configuration (Doxyfile)

Create `Doxyfile` in the project root. Key settings for this project:

```
# Project info
PROJECT_NAME           = "Robot Provisioning"
PROJECT_NUMBER         = "1.0.0"
PROJECT_BRIEF          = "ESP32-S3 WiFi provisioning and RC control firmware"

# Input sources
INPUT                  = main/ components/ docs/
FILE_PATTERNS          = *.c *.h *.md
RECURSIVE              = YES

# Output location
OUTPUT_DIRECTORY       = docs/api
GENERATE_HTML          = YES
GENERATE_LATEX         = NO

# Extract all — we want docs even for "static" functions
EXTRACT_ALL            = YES
EXTRACT_STATIC         = YES

# Call graphs (requires graphviz)
HAVE_DOT               = YES
CALL_GRAPH             = YES
CALLER_GRAPH           = YES

# Preprocessor — expand macros so docs show actual types
ENABLE_PREPROCESSING   = YES
MACRO_EXPANSION        = YES
EXPAND_ONLY_PREDEF     = NO

# C-specific
OPTIMIZE_OUTPUT_FOR_C  = YES
TYPEDEF_HIDES_STRUCT   = YES

# Use markdown files as pages
USE_MDFILE_AS_MAINPAGE = docs/README.md

# Source browsing — click function names to see implementation
SOURCE_BROWSER         = YES
INLINE_SOURCES         = NO

# Warnings — fail CI on undocumented public APIs
WARN_IF_UNDOCUMENTED   = YES
WARN_IF_DOC_ERROR      = YES
WARN_NO_PARAMDOC       = YES
WARN_AS_ERROR          = NO   # Set YES in CI

# Exclude patterns
EXCLUDE                = components/u8g2/ components/QR-Code-generator/
EXCLUDE_PATTERNS       = */test/* */tests/*

# Strip paths for cleaner file names in output
STRIP_FROM_PATH        = .
STRIP_FROM_INC_PATH    = .

# HTML styling
HTML_EXTRA_STYLESHEET  = docs/doxygen-style.css
HTML_COLORSTYLE        = LIGHT
```

---

## Comment Style Guide

Use these patterns consistently so Doxygen parses them correctly.

### File Header

```c
/**
 * @file ctrl_drive.h
 * @brief Differential drive controller with ramp limiting and command watchdog.
 *
 * Mixing: arcade drive. left = y + x, right = y - x.
 * Both outputs clamped to [-1, +1] after mixing.
 *
 * @author Your Name
 * @date 2024
 */
```

### Function Documentation

```c
/**
 * @brief Feed the command watchdog.
 *
 * Must be called each time a valid drive command is received from the
 * WebSocket handler. ctrl_drive_tick() checks elapsed time; if no command
 * arrives within drive_watchdog_ms the controller disarms itself.
 *
 * @param[in] x  Lateral axis, -1.0 (full left) to +1.0 (full right)
 * @param[in] y  Longitudinal axis, -1.0 (full reverse) to +1.0 (full forward)
 *
 * @note Inputs are clamped defensively even though the JS client already
 *       does this. Deadband is applied here from settings_get()->drive_deadband.
 * @warning This function is called from the httpd task (Core 0). The shared
 *          state uses int16_t atomic stores for thread safety.
 * @see ctrl_drive_tick(), ctrl_drive_set_armed()
 */
void ctrl_drive_set_axes(float x, float y);
```

### Struct / Typedef

```c
/**
 * @brief Runtime-tunable settings stored in NVS as JSON.
 *
 * All string fields are null-terminated and bounded by their max length.
 * The palette field stores a JSON sub-object as a flat string.
 *
 * @note settings_validate() enforces invariants (e.g. password length,
 *       numeric ranges). Always validate before saving.
 */
typedef struct {
    int   schema_version;        /**< @brief Current schema version for migration */
    char  device_name[32];       /**< @brief mDNS and UI display name */
    float drive_deadband;        /**< @brief Joystick deadband, [0.0, 0.5] */
    float drive_ramp_rate;       /**< @brief Max delta per 10 ms tick, (0.0, 1.0] */
} robot_settings_t;
```

### Enum

```c
/**
 * @brief LED blink patterns.
 *
 * Each pattern is a 50 ms tick sequence. The timer callback advances
 * s_step and sets duty according to the current pattern.
 */
typedef enum {
    LED_PATTERN_OFF = 0,         /**< @brief Solid off */
    LED_PATTERN_ON,              /**< @brief Solid on at max brightness */
    LED_PATTERN_SLOW_BLINK,      /**< @brief 1 Hz: 500 ms on, 500 ms off */
    LED_PATTERN_FAST_BLINK,      /**< @brief 5 Hz: 200 ms on, 200 ms off */
    LED_PATTERN_DOUBLE_BLINK,    /**< @brief Two quick flashes then pause */
    LED_PATTERN_HEARTBEAT,       /**< @brief Long on, dim pulse, off */
} led_pattern_t;
```

### Define / Macro

```c
/**
 * @brief Fixed-point scale factor for drive axes.
 *
 * Range -10000..10000 maps to -1.0..1.0. int16_t stores are atomic
 * on Xtensa LX7, avoiding torn-read hazards with float.
 */
#define AXES_SCALE  10000
```

### Module Grouping

Use `@defgroup` to organize modules in the HTML sidebar:

```c
/**
 * @defgroup display Display Subsystem
 * @brief SSD1306 OLED driver and draw primitives.
 *
 * The display subsystem manages the u8g2 HAL, power sequencing,
 * and mutex-guarded buffer access. All draw calls are thread-safe.
 *
 * @{
 */

/* ... all display functions and structs ... */

/** @} */  /* end of display */
```

Add to `Doxyfile`:

```
GROUP_NESTED_STRUCTS = YES
```

---

## CI Integration (GitHub Actions)

Add `.github/workflows/docs.yml`:

```yaml
name: Documentation

on:
  push:
    branches: [main]
  pull_request:

jobs:
  build:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
        with:
          submodules: recursive

      - name: Install Doxygen
        run: sudo apt-get install doxygen graphviz

      - name: Generate docs
        run: doxygen Doxyfile

      - name: Check for undocumented public APIs
        run: |
          if grep -q "warning: Member.*is not documented" docs/api/warnings.log; then
            echo "Undocumented public APIs found!"
            cat docs/api/warnings.log
            exit 1
          fi

      - name: Deploy to GitHub Pages
        if: github.ref == 'refs/heads/main'
        uses: peaceiris/actions-gh-pages@v3
        with:
          github_token: ${{ secrets.GITHUB_TOKEN }}
          publish_dir: docs/api/html
```

Enable Pages in repo settings: **Settings → Pages → Source → GitHub Actions**.

---

## VS Code Integration

Install the **Doxygen Documentation Generator** extension (cschlosser.doxdocgen). It auto-generates comment blocks when you type `/**` above a function.

Add to `.vscode/settings.json`:

```json
{
    "doxdocgen.file.copyrightTag": ["@copyright MIT License"],
    "doxdocgen.generic.authorTag": "@author Your Name",
    "doxdocgen.generic.useGitUserName": true,
    "doxdocgen.generic.useGitUserEmail": true,
    "doxdocgen.generic.paramTemplate": "@param[in] {param} ",
    "doxdocgen.generic.returnTemplate": "@return {type} "
}
```

---

## Custom CSS

Create `docs/doxygen-style.css` for branding:

```css
/* Dark header matching the project theme */
#top {
    background: #1a1a2e;
    border-bottom: 2px solid #e94560;
}
#projectname {
    color: #e94560;
    font-family: 'Segoe UI', sans-serif;
}
/* Better code blocks */
div.fragment {
    background: #f8f9fa;
    border: 1px solid #dee2e6;
    border-radius: 4px;
    padding: 12px;
}
```

---

## Tips

- **Run Doxygen often** — fix warnings as you write code, not at release time.
- **Document the "why"** — Doxygen captures the "what" automatically from signatures. Use `@note` and `@warning` for design rationale.
- **Link related functions** — `@see` creates bidirectional navigation in HTML output.
- **Keep `@brief` short** — one sentence. Details go in the main description.
- **Use `@internal` for private notes** — these appear only in the "detailed" view, not the summary tables.
