FROM ubuntu:24.04

# Install packages
RUN apt-get update
RUN apt-get install -y software-properties-common
RUN add-apt-repository -y ppa:deadsnakes/ppa
RUN apt-get install -y git curl cmake ninja-build clang-17 libpq-dev python3.10 python3.10-dev pipenv graphviz graphviz-dev postgresql texlive-latex-extra dvipng cm-super texlive-fonts-extra


# Set working directory for all following commands
COPY ./mutable_fork_resultdb ./mutable_fork_resultdb
WORKDIR ./mutable_fork_resultdb


# Install Python packages
RUN curl -sSL https://bootstrap.pypa.io/get-pip.py -o get-pip.py
RUN python3.10 get-pip.py
RUN pipenv run python3.10 -m pip install --upgrade setuptools
RUN pipenv sync --python 3.10


# Set up artificial git repository
RUN git config --global user.email "you@example.com"
RUN git config --global user.name "Your Name"
RUN git init
RUN git add README.md
RUN git commit -m "dummy commit"


# Build mutable
RUN cmake -S . -B build/release \
-G Ninja \
-DCMAKE_C_COMPILER=clang-17 \
-DCMAKE_CXX_COMPILER=clang++-17 \
-DCMAKE_BUILD_TYPE=Release \
-DBUILD_SHARED_LIBS=OFF \
-DENABLE_SANITIZERS=OFF \
-DENABLE_SANITY_FIELDS=OFF \
-DWITH_V8=ON

RUN cmake --build build/release -t gitversion
RUN cmake --build build/release -t V8
RUN ln -sf /usr/bin/clang-17 /usr/bin/clang
RUN ln -sf /usr/bin/clang++-17 /usr/bin/clang++
RUN cmake --build build/release -t Boost
RUN cmake --build build/release


# Make shellscripts executable
RUN chmod +x ./benchmark/result-db/algorithm/run_benchmarks.sh
RUN chmod +x ./benchmark/result-db/data/imdb/setup_postgres.sh
