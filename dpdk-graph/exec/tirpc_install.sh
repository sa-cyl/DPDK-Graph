cd  ./rdmalib/
chmod +x configure
./configure
make
make install
ldconfig
cd ./rpcbind-0.2.0/
service rpcbind stop
chmod +x configure
./configure
make
make install
service rpcbind start
