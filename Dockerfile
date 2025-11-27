# Matching Engine - Multi-stage Docker Build
# 
# Build:  docker build -t matching-engine .
# Run:    docker run -p 1234:1234 -p 5000:5000/udp matching-engine
#
# Run modes:
#   TCP only:      docker run -p 1234:1234 matching-engine --tcp 1234
#   With multicast: docker run -p 1234:1234 -p 5000:5000/udp matching-engine --tcp 1234 --multicast 239.255.0.1:5000
#   Single processor: docker run -p 1234:1234 matching-engine --tcp 1234 --single-processor

# ============================================================================
# Stage 1: Build
# ============================================================================
FROM ubuntu:24.04 AS builder

# Install build dependencies
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    ninja-build \
    && rm -rf /var/lib/apt/lists/*

# Copy source code
WORKDIR /build
COPY CMakeLists.txt ./
COPY src/ ./src/
COPY include/ ./include/
COPY tools/ ./tools/
COPY tests/ ./tests/

# Build release version
RUN cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build

# ============================================================================
# Stage 2: Runtime
# ============================================================================
FROM ubuntu:24.04 AS runtime

# Install minimal runtime dependencies (just libc, libm, pthreads - already included)
RUN apt-get update && apt-get install -y --no-install-recommends \
    && rm -rf /var/lib/apt/lists/*

# Create non-root user for security
RUN useradd -r -s /bin/false matching

# Copy binaries from builder
WORKDIR /app
COPY --from=builder /build/build/matching_engine .
COPY --from=builder /build/build/tcp_client .
COPY --from=builder /build/build/multicast_subscriber .
COPY --from=builder /build/build/binary_client .
COPY --from=builder /build/build/binary_decoder .

# Set ownership
RUN chown -R matching:matching /app

# Switch to non-root user
USER matching

# Expose ports
# 1234 - TCP for client connections
# 5000 - UDP for multicast market data
EXPOSE 1234/tcp
EXPOSE 5000/udp

# Health check - verify process is running
HEALTHCHECK --interval=30s --timeout=3s --start-period=5s --retries=3 \
    CMD pgrep matching_engine || exit 1

# Default: run TCP server with multicast on dual-processor mode
ENTRYPOINT ["./matching_engine"]
CMD ["--tcp", "1234", "--multicast", "239.255.0.1:5000"]
