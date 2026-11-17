# Terminal-only Ubuntu 26.04 environment for the MiniPOSIX workshop.
# The image holds the tools only; mount the repository at run time (see README).
FROM mcr.microsoft.com/devcontainers/base:3.0.8-ubuntu26.04

ARG LLVM_VERSION=22

# Build tools, GoogleTest and Clang 22. Uses the Ubuntu archive when it has
# clang-22 and falls back to apt.llvm.org for this release otherwise.
RUN set -eux; \
    export DEBIAN_FRONTEND=noninteractive; \
    apt-get update; \
    apt-get install -y build-essential ca-certificates curl gnupg git cmake ninja-build libgtest-dev; \
    if ! apt-get install -y "clang-${LLVM_VERSION}"; then \
        . /etc/os-release; \
        install -d -m 0755 /etc/apt/keyrings; \
        curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key \
            | gpg --dearmor -o /etc/apt/keyrings/apt.llvm.org.gpg; \
        echo "deb [signed-by=/etc/apt/keyrings/apt.llvm.org.gpg] https://apt.llvm.org/${VERSION_CODENAME}/ llvm-toolchain-${VERSION_CODENAME}-${LLVM_VERSION} main" \
            > /etc/apt/sources.list.d/llvm-${LLVM_VERSION}.list; \
        apt-get update; \
        apt-get install -y "clang-${LLVM_VERSION}"; \
    fi; \
    rm -rf /var/lib/apt/lists/*; \
    "clang-${LLVM_VERSION}" --version; \
    "clang++-${LLVM_VERSION}" --version

# uv and OpenCode for the non-root user that the base image provides.
USER vscode
ENV PATH="/home/vscode/.local/bin:/home/vscode/.opencode/bin:${PATH}" \
    UV_LINK_MODE=copy
RUN set -eux; \
    curl -LsSf https://astral.sh/uv/install.sh | sh; \
    curl -fsSL https://opencode.ai/install | bash; \
    uv --version; \
    opencode --version

WORKDIR /work
CMD ["bash"]
