#--------------------------------------------------------
# VCPKG stage
#--------------------------------------------------------
FROM ci-base:latest

ENV DEBIAN_FRONTEND=noninteractive
ENV VCPKG_ROOT=/opt/vcpkg
ENV VCPKG_FORCE_SYSTEM_BINARIES=1

# Install vcpkg
RUN git clone https://github.com/microsoft/vcpkg.git $VCPKG_ROOT && \
    $VCPKG_ROOT/bootstrap-vcpkg.sh -disableMetrics

# "Warm up"
WORKDIR /tmp/vcpkg-warmup
COPY vcpkg.json .

# Install 'linux'
RUN $VCPKG_ROOT/vcpkg install \
    --triplet x64-linux \
    --no-print-usage \
    --x-install-root=/opt/vcpkg-linux

# # Install 'windows'
RUN $VCPKG_ROOT/vcpkg install \
    --triplet x64-mingw-dynamic \
    --no-print-usage \
    --x-install-root=/opt/vcpkg-win

# Clean up
WORKDIR /workspace
RUN rm -rf /tmp/vcpkg-warmup && \
    rm -rf $VCPKG_ROOT/buildtrees/* && \
    rm -rf $VCPKG_ROOT/downloads/*

# Export values
ENV VCPKG_LINUX_ROOT=/opt/vcpkg-linux
ENV VCPKG_WIN_ROOT=/opt/vcpkg-win