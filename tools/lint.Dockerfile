# tools/build.sh builds in this base too, so lint, the gate and a plain build share one toolchain.
FROM devkitpro/devkitarm@sha256:116afba8df8453961de2936ffab20dd441edf4d682856c1ec8b0e53d7ed0bbf5

RUN apt-get update \
	&& apt-get install -y --no-install-recommends \
		clang-format-14=1:14.0.6-12 \
		clang-tidy-14=1:14.0.6-12 \
		cppcheck=2.10-2 \
		shellcheck=0.9.0-1 \
		shfmt=3.6.0-1+b2 \
		zlib1g-dev=1:1.2.13.dfsg-1 \
	&& rm -rf /var/lib/apt/lists/*
