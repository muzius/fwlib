# Étape 1 : Build
FROM arm32v7/gcc:bookworm AS builder

COPY lib/libfwlib32-linux-armv7.so.1.0.5 /usr/local/lib/
RUN ln -s /usr/local/lib/libfwlib32-linux-armv7.so.1.0.5 /usr/local/lib/libfwlib32.so && \
    ln -s /usr/local/lib/libfwlib32-linux-armv7.so.1.0.5 /usr/local/lib/libfwlib32.so.1 && \
    ldconfig

WORKDIR /build
COPY src/ ./

RUN gcc -O2 main.c -L/usr/local/lib -Wl,-rpath=/usr/local/lib -lfwlib32 -lm -lpthread -o fanuc_monitor

# Étape 2 : Runtime léger
FROM arm32v7/debian:bookworm-slim

COPY lib/libfwlib32-linux-armv7.so.1.0.5 /usr/local/lib/
RUN ln -s /usr/local/lib/libfwlib32-linux-armv7.so.1.0.5 /usr/local/lib/libfwlib32.so && \
    ln -s /usr/local/lib/libfwlib32-linux-armv7.so.1.0.5 /usr/local/lib/libfwlib32.so.1 && \
    ldconfig

COPY --from=builder /build/fanuc_monitor /fanuc_monitor

CMD ["/fanuc_monitor"]