FROM docker.io/library/ubuntu:24.04
# Desktop graphics/audio support, without Qt or the compiler/build dependencies.
RUN apt-get update && apt-get install -y --no-install-recommends python3 binutils libgl1 libegl1 libvulkan1 libasound2t64 libpulse0 libusb-1.0-0 libx11-6 libxext6 libxrender1 libxcb1 libxcb-shm0 libxcb-randr0 libxcb-xfixes0 libxkbcommon0 libxkbcommon-x11-0 libwayland-client0 libwayland-egl1 libwayland-cursor0 libfontconfig1 libfreetype6 libdbus-1-3 zlib1g fonts-dejavu-core && rm -rf /var/lib/apt/lists/*
COPY validate-appimage.py /usr/local/bin/validate-appimage.py
ENTRYPOINT ["python3", "/usr/local/bin/validate-appimage.py", "--ubuntu-baseline", "/package"]
