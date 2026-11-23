# Étape 1 : Build
FROM gcc:bookworm AS builder

# Dépendances de compilation pour Mosquitto
RUN apt-get update && apt-get install -y --no-install-recommends \
    libmosquitto-dev \
    && rm -rf /var/lib/apt/lists/*

COPY lib/libfwlib32-linux-x64.so.1.0.5 /usr/local/lib/
RUN ln -s /usr/local/lib/libfwlib32-linux-x64.so.1.0.5 /usr/local/lib/libfwlib32.so && \
    ln -s /usr/local/lib/libfwlib32-linux-x64.so.1.0.5 /usr/local/lib/libfwlib32.so.1 && \
    ldconfig

WORKDIR /build
COPY src/ ./

# Ajout de -lmosquitto pour faire le lien avec la lib Mosquitto
RUN gcc -O2 main.c -L/usr/local/lib -Wl,-rpath=/usr/local/lib -lfwlib32 -lmosquitto -lm -lpthread -o fanuc_monitor

# Étape 2 : Runtime léger
FROM debian:bookworm-slim

# Bibliothèque d'exécution Mosquitto requise au runtime
RUN apt-get update && apt-get install -y --no-install-recommends \
    libmosquitto1 \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

COPY lib/libfwlib32-linux-x64.so.1.0.5 /usr/local/lib/
RUN ln -s /usr/local/lib/libfwlib32-linux-x64.so.1.0.5 /usr/local/lib/libfwlib32.so && \
    ln -s /usr/local/lib/libfwlib32-linux-x64.so.1.0.5 /usr/local/lib/libfwlib32.so.1 && \
    ldconfig

COPY --from=builder /build/fanuc_monitor /fanuc_monitor

CMD ["/fanuc_monitor"]