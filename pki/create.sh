#!/bin/bash -e
# cspell: disable
if ! [[ -f out/root.pem ]]
then
   cfssl gencert -initca root.json | cfssljson -bare out/root
fi
cfssl certinfo -cert out/root.pem | jq '.subject'

if ! [[ -f out/intermediate.pem ]]
then
   cfssl gencert -initca intermediate.json | cfssljson -bare out/intermediate
   cfssl sign -ca out/root.pem -ca-key out/root-key.pem -config config.json -profile intermediate_ca out/intermediate.csr | cfssljson -bare out/intermediate
fi
cfssl certinfo -cert out/intermediate.pem | jq '.subject'

cfssl gencert -initca server.json | cfssljson -bare out/server
cfssl sign -ca out/intermediate.pem -ca-key out/intermediate-key.pem -config config.json -profile server out/server.csr | cfssljson -bare out/server

cfssl gencert -initca client.json | cfssljson -bare out/client
cfssl sign -ca out/intermediate.pem -ca-key out/intermediate-key.pem -config config.json -profile client out/client.csr | cfssljson -bare out/client
