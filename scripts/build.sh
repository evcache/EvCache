#!/bin/bash

set -e

command_exists() {
    command -v "$1" >/dev/null 2>&1
}

# to check if a package is installed (works on Debian/Ubuntu)
package_installed() {
    dpkg -l "$1" >/dev/null 2>&1
}

install_packages() {
    if command_exists apt-get; then
        echo "[+] Detected Debian/Ubuntu system, using apt"
        sudo apt-get update
        sudo apt-get install -y \
            build-essential \
            cmake \
            make \
            libbpf-dev \
            libelf-dev \
            zlib1g-dev \
            libzstd-dev \
            python3 \
            python3-venv \
            python3-pip
    elif command_exists yum; then
        echo "[+] Detected RHEL/CentOS, using yum"
        sudo yum install -y \
            gcc \
            gcc-c++ \
            cmake \
            make \
            libbpf-devel \
            elfutils-libelf-devel \
            zlib-devel \
            libzstd-devel \
            python3 \
            python3-pip
    elif command_exists dnf; then
        echo "[+] Detected Fedora, using dnf"
        sudo dnf install -y \
            gcc \
            gcc-c++ \
            cmake \
            make \
            libbpf-devel \
            elfutils-libelf-devel \
            zlib-devel \
            libzstd-devel \
            python3 \
            python3-pip
    elif command_exists pacman; then
        echo "[+] Detected Arch Linux, using pacman"
        sudo pacman -S --needed \
            base-devel \
            cmake \
            make \
            libbpf \
            libelf \
            zlib \
            zstd \
            python \
            python-pip
    else
        echo "Warning: Could not detect package manager. Please install the following packages manually:"
        echo "  - build-essential/gcc/gcc-c++"
        echo "  - cmake"
        echo "  - make"
        echo "  - libbpf-dev/libbpf-devel"
        echo "  - libelf-dev/elfutils-libelf-devel"
        echo "  - zlib1g-dev/zlib-devel"
        echo "  - libzstd-dev/libzstd-devel"
        echo ""
        read -p "Continue anyway? (y/N): " -n 1 -r
        echo
        if [[ ! $REPLY =~ ^[Yy]$ ]]; then
            exit 1
        fi
    fi
}

echo "[+] Installing required build dependencies"
install_packages

# we should be in the project root directory. verify.
if [ ! -f "CMakeLists.txt" ]; then
    echo "Error: CMakeLists.txt not found. Please run this script from the project root directory."
    exit 1
fi

echo "[+] Creating build directory"
if [ -d "build" ]; then
    echo "Build directory already exists. Cleaning..."
    rm -rf build/*
else
    mkdir build
fi

echo "[+] Generating Makefile"
cd build
cmake ..

echo "[+] Building project"
make -j$(nproc)

cd ..
echo "[+] Setting up Python virtual environment"
python3 -m venv scripts/venv
scripts/venv/bin/pip install -r scripts/requirements.txt

echo ""
echo "[+] Build completed successfully"
echo "Built executables:"
echo "  - vev (eviction set constructor)"
echo "  - vset (LLC set monitoring)"
echo "  - vpo (poisoner tool)"
echo "  - vcolor (cache/page coloring tool)"
echo "  - vtest (test tool for \`vcolor\`'s filtered pages)"
echo ""
echo "All executables are located in the build/ directory."
echo "For further builds after changes, simply run \`make -j\` inside the build directory."
echo ""
echo "Note: For vcolor functionality, you may also need to build and load the kernel module:"
echo "  cd vcolor_km"
echo "  sudo ./setup_vcolor"
