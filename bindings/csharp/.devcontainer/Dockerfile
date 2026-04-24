FROM ubuntu:24.04

ARG LLVM_VERSION=21
ARG CMAKE_VERSION=3.31.6

ENV DEBIAN_FRONTEND=noninteractive
ENV DOTNET_CLI_TELEMETRY_OPTOUT=1
ENV DOTNET_NOLOGO=1

RUN apt-get update -qq && apt-get install -y --no-install-recommends \
    ca-certificates curl wget git gpg gnupg \
    software-properties-common lsb-release \
	ninja-build pkg-config libc6-dev lldb \
	valgrind gdb procps htop strace tar \
	unzip sudo \
    && rm -rf /var/lib/apt/lists/*

COPY ci/install_llvm.sh /tmp/install_llvm.sh
RUN bash /tmp/install_llvm.sh ${LLVM_VERSION} && rm /tmp/install_llvm.sh

RUN wget -O - https://apt.kitware.com/keys/kitware-archive-latest.asc 2>/dev/null \
    | gpg --dearmor - \
    | tee /usr/share/keyrings/kitware-archive-keyring.gpg >/dev/null \
    && echo "deb [signed-by=/usr/share/keyrings/kitware-archive-keyring.gpg] \
    https://apt.kitware.com/ubuntu/ noble main" \
    | tee /etc/apt/sources.list.d/kitware.list >/dev/null \
    && apt-get update -qq \
    && apt-get install -y --no-install-recommends cmake \
    && rm -rf /var/lib/apt/lists/*

RUN wget -q https://packages.microsoft.com/config/ubuntu/24.04/packages-microsoft-prod.deb \
    -O /tmp/packages-microsoft-prod.deb \
    && dpkg -i /tmp/packages-microsoft-prod.deb \
    && rm /tmp/packages-microsoft-prod.deb \
    && apt-get update -qq \
    && apt-get install -y --no-install-recommends \
    dotnet-sdk-8.0 dotnet-sdk-10.0 \
    && rm -rf /var/lib/apt/lists/*

RUN dotnet tool install --global csharpier

RUN dotnet tool install --global BenchmarkDotNet.Tool 2>/dev/null || true

ENV PATH="${PATH}:/root/.dotnet/tools"

RUN usermod -l code -d /home/code -m ubuntu \
    && groupmod -n code ubuntu \
    && echo "code ALL=(ALL) NOPASSWD:ALL" >> /etc/sudoers.d/code \
    && chmod 0440 /etc/sudoers.d/code \
    && mkdir -p /home/code/.dotnet/tools \
    && cp -r /root/.dotnet/tools /home/code/.dotnet/ \
    && chown -R code:code /home/code/.dotnet

USER code

ENV PATH="${PATH}:/home/code/.dotnet/tools"
ENV DOTNET_CLI_HOME=/home/code/.dotnet

WORKDIR /workspace
