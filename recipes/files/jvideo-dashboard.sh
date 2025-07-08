#!/bin/bash

# Color codes for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Paths to different implementations
CPP_MONITOR="/opt/jvideo/bin/queue-monitor-cpp"
PYTHON_MONITOR="/opt/jvideo/services/queue_monitor.py"

# Default version (can be overridden by environment variable)
DEFAULT_VERSION="${JVIDEO_MONITOR_VERSION:-cpp}"
VERSION="${1:-$DEFAULT_VERSION}"

# Function to check if a monitor exists and is executable
check_monitor() {
    local monitor_path="$1"
    local monitor_type="$2"

    if [[ -f "$monitor_path" ]]; then
        if [[ -x "$monitor_path" ]] || [[ "$monitor_path" == *.py ]]; then
            echo -e "${GREEN}✓${NC} $monitor_type monitor available"
            return 0
        else
            echo -e "${YELLOW}!${NC} $monitor_type monitor found but not executable"
            return 1
        fi
    else
        echo -e "${RED}✗${NC} $monitor_type monitor not found"
        return 1
    fi
}

# Function to show usage
usage() {
    echo "JVideo Dashboard Launcher"
    echo "========================="
    echo "Usage: jvideo-dashboard [VERSION] [OPTIONS]"
    echo ""
    echo "VERSION:"
    echo "  cpp, c++     - Run C++ version (default)"
    echo "  python, py   - Run Python version"
    echo "  auto         - Auto-select best available version"
    echo ""
    echo "OPTIONS:"
    echo "  -h, --help   - Show this help message"
    echo "  -l, --list   - List available monitors"
    echo "  -v, --version - Show version information"
    echo ""
    echo "Environment Variables:"
    echo "  JVIDEO_MONITOR_VERSION - Set default version (cpp or python)"
    echo ""
    echo "Examples:"
    echo "  jvideo-dashboard              # Run default version"
    echo "  jvideo-dashboard cpp          # Run C++ version"
    echo "  jvideo-dashboard python       # Run Python version"
    echo "  jvideo-dashboard auto         # Auto-select version"
    echo "  jvideo-dashboard --list       # List available monitors"
    exit 0
}

# Function to list available monitors
list_monitors() {
    echo "Available Queue Monitors:"
    echo "========================"
    check_monitor "$CPP_MONITOR" "C++"
    CPP_AVAILABLE=$?

    check_monitor "$PYTHON_MONITOR" "Python"
    PYTHON_AVAILABLE=$?

    echo ""
    if [[ $CPP_AVAILABLE -eq 0 ]] || [[ $PYTHON_AVAILABLE -eq 0 ]]; then
        echo "Default version: $DEFAULT_VERSION"
    else
        echo -e "${RED}No monitors available!${NC}"
        exit 1
    fi
    exit 0
}

# Function to show version info
show_version() {
    echo "JVideo Dashboard Launcher v1.0"
    echo "Default monitor: $DEFAULT_VERSION"

    if [[ -f "$CPP_MONITOR" ]]; then
        echo -n "C++ monitor: "
        if command -v file >/dev/null 2>&1; then
            file -b "$CPP_MONITOR" | head -1
        else
            echo "installed"
        fi
    fi

    if [[ -f "$PYTHON_MONITOR" ]]; then
        echo "Python monitor: installed"
    fi
    exit 0
}

# Parse options
case "$1" in
    -h|--help|help)
        usage
        ;;
    -l|--list|list)
        list_monitors
        ;;
    -v|--version|version)
        show_version
        ;;
esac

# Auto-select version if requested
if [[ "$VERSION" == "auto" ]]; then
    if [[ -x "$CPP_MONITOR" ]]; then
        VERSION="cpp"
        echo "[Dashboard] Auto-selected C++ version"
    elif [[ -f "$PYTHON_MONITOR" ]]; then
        VERSION="python"
        echo "[Dashboard] Auto-selected Python version"
    else
        echo -e "${RED}Error: No monitor implementation found${NC}"
        exit 1
    fi
fi

# Normalize the version parameter and launch
case "$VERSION" in
    cpp|c++|CPP|C++)
        if [[ ! -f "$CPP_MONITOR" ]]; then
            echo -e "${RED}Error: C++ monitor not found at $CPP_MONITOR${NC}"
            echo "Try: jvideo-dashboard python"
            exit 1
        fi
        echo "[Dashboard] Starting C++ queue monitor..."
        exec "$CPP_MONITOR"
        ;;

    python|py|PYTHON|PY)
        if [[ ! -f "$PYTHON_MONITOR" ]]; then
            echo -e "${RED}Error: Python monitor not found at $PYTHON_MONITOR${NC}"
            echo "Try: jvideo-dashboard cpp"
            exit 1
        fi
        echo "[Dashboard] Starting Python queue monitor..."
        exec /usr/bin/python3 "$PYTHON_MONITOR"
        ;;

    *)
        echo -e "${RED}Error: Unknown version '$VERSION'${NC}"
        echo ""
        usage
        ;;
esac
