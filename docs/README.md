# MafiaNet Documentation

This directory contains the Sphinx + Breathe documentation for MafiaNet.

## Prerequisites

Install system dependencies:

```bash
# macOS
brew install doxygen

# Ubuntu/Debian
sudo apt install doxygen

# Fedora
sudo dnf install doxygen
```

Install Python dependencies:

```bash
pip install -r requirements.txt
```

## Building Documentation

### Quick Build

```bash
./build.sh
```

### Using Make

```bash
make html
```

### Manual Build

```bash
# Generate Doxygen XML
doxygen Doxyfile

# Build Sphinx HTML
sphinx-build -b html . _build/html
```

## Live Preview

For development with auto-reload:

```bash
./build.sh serve
# or
make livehtml
```

Then open http://localhost:8000 in your browser.

## Output

Built documentation is in `_build/html/`. Open `_build/html/index.html` to view.

## Structure

```
docs/
├── Doxyfile              # Doxygen configuration (XML output)
├── conf.py               # Sphinx configuration
├── index.rst             # Main documentation index
├── requirements.txt      # Python dependencies
├── build.sh              # Build script
├── Makefile              # Make targets
├── getting-started/      # Getting started guides
├── guide/                # User guides
└── api/                  # API reference (uses Breathe)
```
