#
# https://github.com/microsoft/vscode-dev-containers/tree/main/containers/debian
#
FROM docker.io/psedoc/cpp-devcontainer:0.12

#
# GDB libc++ pretty printer (works for string, but not for map with llvm-18)
#
ARG USERNAME=vscode
RUN cd /home/${USERNAME} && \
    git clone https://github.com/koutheir/libcxx-pretty-printers.git && \
    chown -R $USERNAME libcxx-pretty-printers && \
    echo >>.gdbinit "python\nimport sys\nsys.path.insert(0, '/home/${USERNAME}/libcxx-pretty-printers/src')\nfrom libcxx.v1.printers import register_libcxx_printers\nregister_libcxx_printers(None)\nend" && \
    chown $USERNAME .gdbinit

#
# ASIO Debugger Extensions
#
# https://github.com/chriskohlhoff/asio-debugger-extensions.git
#
ARG USERNAME=vscode
RUN cd /home/${USERNAME} && \
    git clone https://github.com/chriskohlhoff/asio-debugger-extensions.git && \
    chown -R $USERNAME asio-debugger-extensions && \
    echo >>.gdbinit "source /home/${USERNAME}/asio-debugger-extensions/gdb/awaitable_backtrace.py" && \
    echo >>.gdbinit "source /home/${USERNAME}/asio-debugger-extensions/gdb/pretty_printer.py" && \
    chown $USERNAME .gdbinit


#
# custom curl that does not suffer from https://github.com/curl/curl/issues/10634
#
# FIXME: There is also an older curl in /usr that is required by git and llvm. But we can't easily
#        replace that...
#
RUN apt-get update && export DEBIAN_FRONTEND=noninteractive && \
    apt-get -y install libnghttp2-dev libev-dev && \
    apt-get clean && \
    rm -rf /var/lib/apt/lists/*

RUN cd opt && \
    git clone --depth 1 https://github.com/curl/curl.git && \
    cd curl && autoreconf -fi && \
    ./configure --with-openssl --without-libpsl --with-nghttp2 --prefix /usr/local && \
    make -j$(nproc) && make install && \
    cd .. && rm -rf curl

#
# spdlog
#
# https://github.com/gabime/spdlog
#
RUN cd opt && \
    git clone https://github.com/gabime/spdlog.git && \
    cd spdlog && mkdir build && cd build && \
    cmake .. && make -j$(nproc) && make install && \
    cd ../.. && rm -rf spdlog

#
# gtest-parallel
#
RUN cd opt && \
    git clone https://github.com/google/gtest-parallel.git && \
    cd /usr/local/bin && \
    ln -s /opt/gtest-parallel/gtest-parallel

# --------------------------------------------------------------------------------------------------

RUN echo "/usr/local/lib" > /etc/ld.so.conf.d/local.conf && ldconfig
ENV LD_LIBRARY_PATH=/usr/local/lib

#
# boringssl / nghttp3 / ngtcp2 (using boringssl) / urlparse
#
# NOTE: don't install BoringSSL to /usr/local, we don't want it to interfere with OpenSSL
#
# RUN git clone --depth 1 https://boringssl.googlesource.com/boringssl && \
#     cd boringssl && \
#     mkdir build && cd build && \
#     cmake .. -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DCMAKE_INSTALL_PREFIX=/opt/boringssl && \
#     make -j$(nproc) && make install && \
#     cd ../.. && rm -rf boringssl
ARG AWS_LC_VERSION=v1.51.2
RUN git clone --depth 1 -b ${AWS_LC_VERSION} https://github.com/aws/aws-lc && \
    cd aws-lc && \
    cmake -B build -DDISABLE_GO=ON --install-prefix=/opt/boringssl && \
    make -j$(nproc) -C build && \
    cmake --install build && \
    cd .. && \
    rm -rf aws-lc

ARG NGHTTP3_VERSION=v1.9.0
RUN git clone --recursive --depth 1 --branch ${NGHTTP3_VERSION} https://github.com/ngtcp2/nghttp3 && \
    cd nghttp3 && \
    autoreconf -i && \
    ./configure --prefix=/usr/local --enable-lib-only && \
    make -j$(nproc) check && make install && \
    cd .. && rm -rf nghttp3

ARG NGTCP2_VERSION=v1.12.0
RUN git clone --recursive --depth 1 --branch ${NGTCP2_VERSION} https://github.com/ngtcp2/ngtcp2 && \
    cd ngtcp2 && \
    autoreconf -i && \
    ./configure --prefix=/usr/local \
       BORINGSSL_CFLAGS="-I/opt/boringssl/include" \
       BORINGSSL_LIBS="-L/opt/boringssl/lib -lssl -lcrypto" \
       --with-boringssl && \
    make -j$(nproc) check && make install && \
    cd third-party/urlparse && \
    autoreconf -i && \
    ./configure --prefix=/usr/local && \
    make -j$(nproc) && make install && \
    cd ../.. && \
    cd .. && rm -rf ngctp2
  
# ==================================================================================================

# RUN git clone --depth 1 -b v1.5.0 https://github.com/libbpf/libbpf && \
#     cd libbpf && \
#     PREFIX=/usr/local LIBDIR=/usr/local/lib make -j$(nproc) -C src install && \
#     cd .. && rm -rf libbpf

#
# nghttp2
#
# FIXME: try to add --enable-http3 (for nghttx and h2load only)
# Seems like OpenSSL gets in the way of BoringSSL (aws-lc) somehow.
#
ARG NGHTTP2_VERSION=v1.65.0
RUN git clone --recursive --depth 1 --branch ${NGHTTP2_VERSION} https://github.com/nghttp2/nghttp2.git && \
    cd nghttp2 && \
    autoreconf -i && \
    ./configure --prefix=/usr/local && \
    make -j$(nproc) && make install && \
    cd .. && rm -rf nghttp2

#    ./configure --prefix=/usr/local --with-mruby --enable-http3 --with-libbpf \
#        LIBNGTCP2_CRYPTO_BORINGSSL_CFLAGS="-I/opt/boringssl/include" \
#        LIBNGTCP2_CRYPTO_BORINGSSL_LIBS="-L/opt/boringssl/lib -lssl -lcrypto" && \

# ==================================================================================================

# docker build . -t anyhttp:1.0
