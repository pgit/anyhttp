#
# https://github.com/microsoft/vscode-dev-containers/tree/main/containers/debian
#
FROM docker.io/psedoc/cpp-devcontainer:0.10

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

RUN echo "export LESS=RS" >/etc/profile.d/less.sh

#
# TEST: build ngtcp2 with BoringSSL backend
#
RUN cd opt && git clone --recursive https://github.com/ngtcp2/nghttp3
RUN cd opt && git clone --recursive https://github.com/ngtcp2/ngtcp2

RUN cd opt && git clone https://boringssl.googlesource.com/boringssl
RUN cd opt && \
    cd boringssl && \
    cmake -B build -DCMAKE_POSITION_INDEPENDENT_CODE=ON && \
    make -j$(nproc) -C build  && \
    cd .. && \
    cd nghttp3 && \
    autoreconf -i && \
    ./configure --prefix=$PWD/build --enable-lib-only && \
    make -j$(nproc) check && \
    make install && \
    cd .. && \
    cd ngtcp2 && \
    autoreconf -i && \
    ./configure PKG_CONFIG_PATH=$PWD/../nghttp3/build/lib/pkgconfig \
       BORINGSSL_LIBS="-L$PWD/../boringssl/build/ssl -lssl -L$PWD/../boringssl/build/crypto -lcrypto" \
       BORINGSSL_CFLAGS="-I$PWD/../boringssl/include" \
       --with-boringssl && \
    make -j$(nproc) check && \
    make install

# RUN cd opt && git clone --depth 1 -b v4.0.0 https://github.com/libressl/portable.git libressl
# RUN cd opt && \
#     cd libressl && \
#     export LIBRESSL_GIT_OPTIONS="-b libressl-v4.0.0" && \
#     ./autogen.sh && \
#     ./configure --prefix=$PWD/build && \
#     make -j$(nproc) install && \
#     cd .. && \
#     cd nghttp3 && \
#     autoreconf -i && \
#     ./configure --prefix=$PWD/build --enable-lib-only && \
#     make -j$(nproc) check && \
#     make install && \
#     cd .. && \
#     cd ngtcp2 && \
#     autoreconf -i && \
#     ./configure PKG_CONFIG_PATH=$PWD/../nghttp3/build/lib/pkgconfig:$PWD/../libressl/build/lib/pkgconfig && \
#     make -j$(nproc) check && \
#     make install

#
# enable persistent bash history in container
#
# https://code.visualstudio.com/remote/advancedcontainers/persist-bash-history
#
# To actually make it persistent across re-creation of the container, you need to
# mount a volume to /commandhistory. You can do this in devcontainer.json like this:
#
# 	"mounts": [
# 		"source=cpp-devcontainer-bashhistory,target=/commandhistory,type=volume"
#   ]
#
ARG USERNAME=vscode
RUN SNIPPET="export PROMPT_COMMAND='history -a' && export HISTFILE=/commandhistory/.bash_history" \
    && mkdir /commandhistory \
    && touch /commandhistory/.bash_history \
    && chown -R $USERNAME /commandhistory \
    && echo "$SNIPPET" >> "/home/$USERNAME/.bashrc"

SHELL ["/bin/bash", "-c"]
