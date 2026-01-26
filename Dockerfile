FROM ubuntu:22.04

# Avoid interactive prompts
ENV DEBIAN_FRONTEND=noninteractive
# Set CI flag for test runner
ENV CI=true

# Install build dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    git \
    libssl-dev \
    python3 \
    && rm -rf /var/lib/apt/lists/*

# Set working directory
WORKDIR /mafianet

# Copy source code
COPY . .

# Build
RUN cmake -B build -DMAFIANET_BUILD_SAMPLES=ON \
    && cmake --build build --config Release --parallel $(nproc)

# Run tests
CMD ["./build/Samples/Tests/Tests"]
