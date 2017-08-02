#!/bin/bash
action=$1

if [ "$action" = "all" ]; then
    ./configure --prefix=/usr/local --enable-shared --disable-static
fi

if [ "$action" = "all" ] || [ "$action" = "compile" ]; then
    make -j4 all
fi

if [ "$action" = "all" ] || [ "$action" = "compile" ] || [ "$action" = "install" ]; then
    # Install to a different location
    rm -rf install_dir
    mkdir install_dir
    make install DESTDIR=`pwd`/install_dir
fi

ffm=`env |grep LD_LIBRARY_PATH |grep ffmpeg`
if [ -z "$ffm" ]; then
    export LD_LIBRARY_PATH=`pwd`/install_dir/usr/local/lib:$LD_LIBRARY_PATH
fi

