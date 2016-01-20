#/bin/sh

PE=""
if [ "$PELIB_PATH" ] ; then
	PE="--with-pelib=${PELIB_PATH}"
fi

if [ -z "$DISMOUNT_PATH" ] ; then
	echo "Please specify DISMOUNT_PATH"
elif [ -z "$XSTL_PATH" ] ; then
        echo "Please specify XSTL_PATH"
else
    PWD=`pwd`
    ./autogen.sh && ./configure --prefix=${PWD}/out --enable-tests --enable-debug --enable-unicode $PE --with-xstl=${XSTL_PATH} && make -j `nproc` && make install
fi
