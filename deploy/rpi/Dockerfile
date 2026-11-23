# Étape 1 : Compilation ARM 32-bit
FROM arm32v7/gcc:bookworm AS builder

# Fanuc lib armv7 (32-bit)
COPY libfwlib32-linux-armv7.so.1.0.5 /usr/local/lib/libfwlib32.so
RUN ldconfig

WORKDIR /usr/src/fwlib
COPY fwlib32.h ./
COPY examples/c-minimal/main.c ./

RUN gcc -Wall -O2 main.c -L/usr/local/lib -Wl,-rpath=/usr/local/lib -lfwlib32 -lpthread -o fanuc_minimal

# Étape 2 : Image finale légère ARM 32-bit
FROM arm32v7/debian:bookworm-slim

COPY libfwlib32-linux-armv7.so.1.0.5 /usr/local/lib/libfwlib32.so
RUN ldconfig

COPY --from=builder /usr/src/fwlib/fanuc_minimal /fanuc_minimal

CMD ["/fanuc_minimal"]