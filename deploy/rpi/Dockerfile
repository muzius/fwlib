# Étape 1 : Compilation ARM 32 bits
FROM arm32v7/gcc:bookworm AS builder

# 1. Copie et création des liens symboliques pour FOCAS armv7
COPY libfwlib32-linux-armv7.so.1.0.5 /usr/local/lib/
RUN ln -s /usr/local/lib/libfwlib32-linux-armv7.so.1.0.5 /usr/local/lib/libfwlib32.so && \
    ln -s /usr/local/lib/libfwlib32-linux-armv7.so.1.0.5 /usr/local/lib/libfwlib32.so.1 && \
    ldconfig

WORKDIR /usr/src/fwlib

# 2. Copie du header Fanuc et du code source
COPY fwlib32.h ./
COPY examples/c-minimal/main.c ./

# 3. Compilation avec -I. et -lm (math)
RUN gcc -I. main.c -L/usr/local/lib -Wl,-rpath=/usr/local/lib -lfwlib32 -lm -lpthread -o fanuc_minimal

# Étape 2 : Image finale légère (Debian ARM 32 bits)
FROM arm32v7/debian:bookworm-slim

COPY libfwlib32-linux-armv7.so.1.0.5 /usr/local/lib/
RUN ln -s /usr/local/lib/libfwlib32-linux-armv7.so.1.0.5 /usr/local/lib/libfwlib32.so && \
    ln -s /usr/local/lib/libfwlib32-linux-armv7.so.1.0.5 /usr/local/lib/libfwlib32.so.1 && \
    ldconfig

COPY --from=builder /usr/src/fwlib/fanuc_minimal /fanuc_minimal

CMD ["/fanuc_minimal"]