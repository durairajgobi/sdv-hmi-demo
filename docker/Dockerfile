# syntax=docker/dockerfile:1.7
# Multi-target image build for the SDV demo. Each runtime target is a separate
# minimal container, mirroring how SDV platforms package apps as OCI images.
#   docker build -f docker/Dockerfile --target hmi .
ARG UBUNTU=ubuntu:24.04

# ---------------------------------------------------------------- build ----
FROM ${UBUNTU} AS build
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build pkg-config libssl-dev \
        qt6-base-dev qt6-declarative-dev qml6-module-qtquick qml6-module-qtquick-shapes \
        qml6-module-qtquick-window qml6-module-qtqml-workerscript \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt ./
COPY someip someip
COPY vehicle-sim vehicle-sim
COPY hmi hmi
COPY ota-agent ota-agent
COPY cockpit cockpit
COPY tests tests
COPY tools tools
RUN cmake -S . -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/opt/sdv \
 && cmake --build /build \
 && ctest --test-dir /build --output-on-failure \
 && cmake --install /build \
 && tools/build_bundle.sh 1.0.0 --dir /opt/sdv/factory-bundle

# ---------------------------------------------------------- vehicle-sim ----
FROM ${UBUNTU} AS vehicle-sim
COPY --from=build /opt/sdv/bin/vehicle-sim /usr/local/bin/
USER 65534:65534
EXPOSE 30490/udp 30501/udp
ENTRYPOINT ["vehicle-sim"]

# ------------------------------------------------------------ ota-agent ----
FROM ${UBUNTU} AS ota-agent
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        libqt6core6t64 libqt6network6t64 libssl3t64 ca-certificates tar gzip \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /opt/sdv/bin/sdv-ota-agent /usr/local/bin/
ENV HMI_BUNDLE_ROOT=/var/lib/sdv/hmi \
    OTA_PUBLIC_KEY=/etc/sdv/ota_signing_pub.pem \
    OTA_WORK_DIR=/var/lib/sdv/ota-dl
EXPOSE 30491/udp 30511/udp
ENTRYPOINT ["sdv-ota-agent"]

# ------------------------------------------------------------------ hmi ----
FROM ${UBUNTU} AS hmi
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        libqt6gui6t64 libqt6qml6 libqt6quick6 libqt6network6t64 qt6-qpa-plugins \
        qml6-module-qtquick qml6-module-qtquick-shapes qml6-module-qtquick-window \
        qml6-module-qtqml-workerscript fonts-dejavu-core libgl1 libegl1 \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /opt/sdv/bin/sdv-cluster-hmi /usr/local/bin/
COPY --from=build /opt/sdv/factory-bundle /opt/sdv/factory-bundle
COPY tools/provision_factory.sh /usr/local/bin/provision_factory.sh
ENV HMI_BUNDLE_ROOT=/var/lib/sdv/hmi \
    FACTORY_BUNDLE_DIR=/opt/sdv/factory-bundle \
    QT_QUICK_BACKEND=software \
    QT_QPA_PLATFORM=vnc:size=1440x540
# View the cluster with any VNC client on localhost:5900
EXPOSE 5900
ENTRYPOINT ["sdv-cluster-hmi"]

# -------------------------------------------------------------- cockpit ----
# One "SoC" container: sdv-hypervisor runs the cluster partition (Qt HMI) and
# the IVI partition (sdv-ivi-guest) with CPU pinning, priorities, RAM ceilings
# and a heartbeat watchdog; inter-VM traffic goes through /dev/shm.
FROM hmi AS cockpit
COPY --from=build /opt/sdv/bin/sdv-hypervisor /opt/sdv/bin/sdv-ivi-guest /usr/local/bin/
COPY cockpit/partitions.conf /etc/sdv/partitions.conf
ENV SDV_BIN_DIR=/usr/local/bin \
    COCKPIT_SHM_DIR=/dev/shm/sdv-cockpit
EXPOSE 5900
ENTRYPOINT ["sdv-hypervisor", "--config", "/etc/sdv/partitions.conf"]

# ------------------------------------------------------------ ota-server ---
# Backend side: static release server + release tooling (bundle, hash, sign).
FROM ${UBUNTU} AS ota-server
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        python3 openssl tar gzip coreutils \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /opt/ota
COPY ota-server/server.py ota-server/server.py
COPY tools tools
COPY hmi/bundle-src hmi/bundle-src
ENV OTA_SERVER_ROOT=/srv/ota
EXPOSE 8080
ENTRYPOINT ["python3", "ota-server/server.py"]
