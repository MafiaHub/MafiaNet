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

# Build (integration suite via samples, GoogleTest unit suite via tests)
RUN cmake -B build -DMAFIANET_BUILD_SAMPLES=ON -DMAFIANET_BUILD_TESTS=ON \
    && cmake --build build --config Release --parallel $(nproc)

# Run tests through CTest: each test is its own process (isolation), with a
# per-test timeout and bounded retries for transient timing misses on loaded
# runners. JUnit XML is written to /results (mount it to collect the report).
RUN mkdir -p /results
CMD ["ctest", "--test-dir", "build", "--output-on-failure", "--repeat", "until-pass:3", "--timeout", "600", "--output-junit", "/results/junit.xml"]
