FROM mcr.microsoft.com/vscode/devcontainers/base:0-ubuntu-18.04

RUN sed -i 's/archive\.ubuntu\.com/au.archive.ubuntu.com/g' /etc/apt/sources.list

# [Option] Install zsh
ARG INSTALL_ZSH="true"
# [Option] Upgrade OS packages to their latest versions
ARG UPGRADE_PACKAGES="false"
# [Option] Select CLI version
ARG CLI_VERSION="latest"

ARG DEBIAN_FRONTEND=noninteractive
ARG APT_KEY_DONT_WARN_ON_DANGEROUS_USAGE=1

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        apt-utils \
        unzip \
        tar \
        curl \
        wget \
        xz-utils \
        alien \
        clinfo \
        software-properties-common \
    && localedef -i en_AU -c -f UTF-8 -A /usr/share/locale/locale.alias en_AU.UTF-8 && \
    rm -rf /var/lib/apt/lists/*

RUN wget https://github.com/intel/compute-runtime/releases/download/21.13.19438/intel-gmmlib_20.4.1_amd64.deb
RUN wget https://github.com/intel/intel-graphics-compiler/releases/download/igc-1.0.6748/intel-igc-core_1.0.6748_amd64.deb
RUN wget https://github.com/intel/intel-graphics-compiler/releases/download/igc-1.0.6748/intel-igc-opencl_1.0.6748_amd64.deb
RUN wget https://github.com/intel/compute-runtime/releases/download/21.13.19438/intel-opencl_21.13.19438_amd64.deb
RUN wget https://github.com/intel/compute-runtime/releases/download/21.13.19438/intel-ocloc_21.13.19438_amd64.deb
RUN wget https://github.com/intel/compute-runtime/releases/download/21.13.19438/intel-level-zero-gpu_1.0.19438_amd64.deb

RUN dpkg -i *.deb

RUN apt-get update -y && apt-get install -y \
        libc6-dev \
        libc6 \
        libc-bin \
        build-essential \
        pkg-config \
        cmake \
        ca-certificates \
        gnupg \
        clang-tidy \
        zlib1g-dev \
        libglm-dev \
        googletest \
        libtbb-dev \
        libpdal-dev \
        doxygen \
        libpng-dev \
        libeigen3-dev


RUN apt-get install -y gpg-agent wget
RUN wget -qO - https://repositories.intel.com/graphics/intel-graphics.key | apt-key add -
RUN apt-add-repository 'deb [arch=amd64] https://repositories.intel.com/graphics/ubuntu focal main'

RUN cd /tmp
RUN wget https://apt.repos.intel.com/intel-gpg-keys/GPG-PUB-KEY-INTEL-SW-PRODUCTS.PUB
RUN apt-key add GPG-PUB-KEY-INTEL-SW-PRODUCTS.PUB
RUN rm GPG-PUB-KEY-INTEL-SW-PRODUCTS.PUB

RUN echo "deb https://apt.repos.intel.com/oneapi all main" | tee /etc/apt/sources.list.d/oneAPI.list
RUN apt update

RUN apt-get update -y && apt-get install -y software-properties-common gpg-agent wget
RUN wget -qO - https://repositories.intel.com/graphics/intel-graphics.key | apt-key add -
RUN apt-add-repository 'deb [arch=amd64] https://repositories.intel.com/graphics/ubuntu focal main'

RUN apt-get update -y && \
    apt-get install -y --no-install-recommends \
        intel-oneapi-runtime-ccl \
        intel-oneapi-runtime-compilers \
        intel-oneapi-runtime-dal \
        intel-oneapi-runtime-dnnl \
        intel-oneapi-runtime-dpcpp-cpp \
        intel-oneapi-runtime-dpcpp-library \
        intel-oneapi-runtime-fortran \
        intel-oneapi-runtime-ipp \
        intel-oneapi-runtime-ipp-crypto \
        intel-oneapi-runtime-libs \
        intel-oneapi-runtime-mkl \
        intel-oneapi-runtime-mpi \
        intel-oneapi-runtime-opencl \
        intel-oneapi-runtime-openmp \
        intel-oneapi-runtime-tbb \
        intel-oneapi-runtime-vpl

# keep command history
RUN SNIPPET="export PROMPT_COMMAND='history -a' && export HISTFILE=/commandhistory/.bash_history" \
    && mkdir /commandhistory \
    && touch /commandhistory/.bash_history \
    && chown -R vscode /commandhistory \
    && echo $SNIPPET >> "/home/vscode/.bashrc"
