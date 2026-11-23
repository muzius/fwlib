# Étape 1 : Build
# Étape 1 : Build
FROM arm32v7/gcc:bookworm AS builder

# Dépendances pour compiler avec Mosquitto
RUN apt-get update && apt-get install -y --no-install-recommends \
    libmosquitto-dev \
    && rm -rf /var/lib/apt/lists/*

COPY lib/libfwlib32-linux-armv7.so.1.0.5 /usr/local/lib/
RUN ln -s /usr/local/lib/libfwlib32-linux-armv7.so.1.0.5 /usr/local/lib/libfwlib32.so && \
    ln -s /usr/local/lib/libfwlib32-linux-armv7.so.1.0.5 /usr/local/lib/libfwlib32.so.1 && \
    ldconfig

WORKDIR /build
COPY src/ ./

# Ajout de -lmosquitto pour le lien avec la lib MQTT
RUN gcc -O2 main.c -L/usr/local/lib -Wl,-rpath=/usr/local/lib -lfwlib32 -lmosquitto -lm -lpthread -o fanuc_monitor

# Étape 2 : Runtime léger
FROM arm32v7/debian:bookworm-slim

# Bibliothèque d'exécution Mosquitto
RUN apt-get update && apt-get install -y --no-install-recommends \
    libmosquitto1 \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

COPY lib/libfwlib32-linux-armv7.so.1.0.5 /usr/local/lib/
RUN ln -s /usr/local/lib/libfwlib32-linux-armv7.so.1.0.5 /usr/local/lib/libfwlib32.so && \
    ln -s /usr/local/lib/libfwlib32-linux-armv7.so.1.0.5 /usr/local/lib/libfwlib32.so.1 && \
    ldconfig

COPY --from=builder /build/fanuc_monitor /fanuc_monitor

CMD ["/fanuc_monitor"]