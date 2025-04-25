if [ ! -d "./build_all" ];then
mkdir build_all
fi
cd build_all
cmake -DCMAKE_BUILD_TYPE=Release .. 
# cmake -DCMAKE_BUILD_TYPE=Release .. -DICONV_LIBRARY=/opt/libiconv/lib/libiconv.so -DICONV_INCLUDE_DIR=/opt/libiconv/include
make -j4