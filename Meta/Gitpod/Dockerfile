FROM gitpod/workspace-full-vnc

RUN sudo add-apt-repository -yn ppa:ubuntu-toolchain-r/test \
    && sudo apt-get update \
    && sudo apt-get install -y build-essential cmake curl libmpfr-dev libmpc-dev libgmp-dev e2fsprogs ninja-build qemu-system-gui qemu-system-x86 qemu-utils ccache rsync unzip texinfo \
    && sudo apt-get install -y gcc-11 g++-11 \
    && sudo apt-get install -y libgtk-3-dev libpixman-1-dev libsdl2-dev libspice-server-dev
