#!/bin/bash
# Build MafiaNet documentation
# Usage: ./build.sh [html|clean|serve]

set -e

DOCS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$DOCS_DIR"

# Check dependencies
check_deps() {
    if ! command -v doxygen &> /dev/null; then
        echo "Error: doxygen is not installed"
        echo "Install with: brew install doxygen (macOS) or apt install doxygen (Linux)"
        exit 1
    fi

    if ! command -v sphinx-build &> /dev/null; then
        echo "Error: sphinx-build is not installed"
        echo "Install with: pip install -r requirements.txt"
        exit 1
    fi
}

# Build Doxygen XML
build_doxygen() {
    echo "==> Running Doxygen..."
    doxygen Doxyfile
}

# Build Sphinx HTML
build_html() {
    check_deps
    build_doxygen
    echo "==> Building Sphinx documentation..."
    sphinx-build -b html . _build/html
    echo ""
    echo "Documentation built successfully!"
    echo "Open _build/html/index.html in your browser"
}

# Clean build artifacts
clean() {
    echo "==> Cleaning build artifacts..."
    rm -rf _build
    echo "Done"
}

# Serve with auto-reload
serve() {
    check_deps
    build_doxygen
    echo "==> Starting documentation server with auto-reload..."
    sphinx-autobuild . _build/html --port 8000
}

# Main
case "${1:-html}" in
    html)
        build_html
        ;;
    clean)
        clean
        ;;
    serve)
        serve
        ;;
    doxygen)
        build_doxygen
        ;;
    *)
        echo "Usage: $0 [html|clean|serve|doxygen]"
        echo ""
        echo "Commands:"
        echo "  html    - Build HTML documentation (default)"
        echo "  clean   - Remove build artifacts"
        echo "  serve   - Build and serve with auto-reload"
        echo "  doxygen - Only generate Doxygen XML"
        exit 1
        ;;
esac
