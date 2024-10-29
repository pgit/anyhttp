#
# https://github.com/microsoft/vscode-dev-containers/tree/main/containers/debian
#
FROM docker.io/psedoc/cpp-devcontainer:0.9

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
    apt-get -y install libnghttp2-dev libnghttp3-dev && \
    apt-get clean && \
    rm -rf /var/lib/apt/lists/*

RUN git clone --depth 1 https://github.com/curl/curl.git && \
    cd curl && autoreconf -fi && \
    ./configure --with-openssl --without-libpsl --with-nghttp2 --prefix /usr/local && \
    make -j 20 && make install && \
    cd .. && rm -rf curl

#
# spdlog
#
# https://github.com/gabime/spdlog
#
RUN git clone https://github.com/gabime/spdlog.git && \
    cd spdlog && mkdir build && cd build && \
    cmake .. && make -j && make install && \
    cd ../.. && rm -rf spdlog

#
# gtest-parallel
#
RUN git clone https://github.com/google/gtest-parallel.git && \
    cd /usr/local/bin && \
    ln -s /gtest-parallel/gtest-parallel

RUN echo "export LESS=RS" >/etc/profile.d/less.sh

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
