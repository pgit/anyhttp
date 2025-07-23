#!/bin/bash -e
#
# https://nghttp2.org/documentation/package_README.html
#
git clone --depth 1 -b v1.46.1 https://github.com/aws/aws-lc
cd aws-lc
cmake -B build -DDISABLE_GO=ON --install-prefix=$PWD/opt
make -j$(nproc) -C build
cmake --install build
cd ..

git clone --depth 1 -b v1.8.0 https://github.com/ngtcp2/nghttp3
cd nghttp3
git submodule update --init --depth 1
autoreconf -i
./configure --prefix=$PWD/build --enable-lib-only
make -j$(nproc)
make install
cd ..

git clone --depth 1 -b v1.12.0 https://github.com/ngtcp2/ngtcp2
cd ngtcp2
git submodule update --init --depth 1
autoreconf -i
./configure --prefix=$PWD/build --enable-lib-only --with-boringssl \
    BORINGSSL_CFLAGS="-I$PWD/../aws-lc/opt/include" \
    BORINGSSL_LIBS="-L$PWD/../aws-lc/opt/lib -lssl -lcrypto"
make -j$(nproc)
make install
cd ..

git clone --depth 1 -b v1.5.0 https://github.com/libbpf/libbpf
cd libbpf
PREFIX=$PWD/build make -C src install
cd ..

git clone https://github.com/nghttp2/nghttp2
cd nghttp2
git submodule update --init
autoreconf -i
./configure --with-mruby --enable-http3 --with-libbpf \
    PKG_CONFIG_PATH="$PWD/../aws-lc/opt/lib/pkgconfig:$PWD/../nghttp3/build/lib/pkgconfig:$PWD/../ngtcp2/build/lib/pkgconfig:$PWD/../libbpf/build/lib64/pkgconfig" \
    LDFLAGS="$LDFLAGS -Wl,-rpath,$PWD/../aws-lc/opt/lib -Wl,-rpath,$PWD/../libbpf/build/lib64"
make -j$(nproc)
