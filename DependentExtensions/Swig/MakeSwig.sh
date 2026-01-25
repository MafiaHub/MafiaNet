# settings
BZIP2_VERSION=1.0.6
SWIG_VERSION=2.0.12

function syntaxError
{
	echo "Usage:"
	echo "  MakeSwig.bat <mafianet_root_path> [<dependent_extension>]"
	echo "    [--rakNetCompatibility]"
	echo
	echo "  mafianet_root_path  Path to the MafiaNet root directory."
	echo "  dependent_extension The dependent extension which should be included."
	echo "                      Supported values: MYSQL_AUTOPATCHER, SQLITE"
	echo
	echo "Options:"
	echo "  --rakNetCompatibility If specified, creates the C# wrapper in RakNet"
	echo "                        compatibility mode."
}

# read mandatory parameter
ROOT_DIR=${1%/} # remove trailing '/'

if [ "$ROOT_DIR" == "" ]; then
	echo "Invalid number of parameters"
	syntaxError
	exit 1
fi

# initialize variables to defaults
RAKNET_COMPATIBILITY_MODE=0
BUILD_BZIP2=0
SOURCE_DIR=$ROOT_DIR/Source
SWIG_ADDITIONAL_INCLUDES=
SWIG_INCLUDES="-I$SOURCE_DIR/include/mafianet -ISwigInterfaceFiles"
SWIG_DEFINES=
GCC_ADDITIONAL_FILES=
GCC_ADDITIONAL_INCLUDES=
GCC_ADDITIONAL_LIBS=
GCC_DEFINES=
NAMESPACE=SLNet
OUTPUT_DIR=../../bindings/csharp/interfaces
OUTPUT_WRAPPER_FILENAME=../../bindings/csharp/wrapper/mafianet_wrapper.cpp
SO_NAME=MafiaNet

# check if we have a dependent extension specified in the 2nd argument
if [ "$2" == "SQLITE" ]; then
	# TODO #med - find a way around the additional variables and rather only work with a single one (aka: SWIG_INCLUDES)
	SWIG_ADDITIONAL_INCLUDES="-I$ROOT_DIR/DependentExtensions/SQLite3Plugin"
	SWIG_DEFINES=-DSWIG_ADDITIONAL_SQL_LITE
	GCC_ADDITIONAL_FILES="$ROOT_DIR/DependentExtensions/SQLite3Plugin/SQLite3ClientPlugin.cpp $ROOT_DIR/DependentExtensions/SQLite3Plugin/SQLite3PluginCommon.cpp $ROOT_DIR/DependentExtensions/SQLite3Plugin/Logger/ClientOnly/SQLiteClientLoggerPlugin.cpp $ROOT_DIR/DependentExtensions/SQLite3Plugin/Logger/SQLiteLoggerCommon.cpp"
	GCC_ADDITIONAL_INCLUDES="-I$ROOT_DIR/DependentExtensions/SQLite3Plugin/Logger/ClientOnly -I$ROOT_DIR/DependentExtensions/SQLite3Plugin/Logger -I$ROOT_DIR/DependentExtensions/SQLite3Plugin"
elif [ "$2" == "MYSQL_AUTOPATCHER" ]; then
	SWIG_ADDITIONAL_INCLUDES="-I$ROOT_DIR/DependentExtensions/Autopatcher -I$ROOT_DIR/DependentExtensions/MySQLInterface"
	SWIG_DEFINES=-DSWIG_ADDITIONAL_AUTOPATCHER_MYSQL
	GCC_ADDITIONAL_FILES="blocksort.o bzip2.o bzlib.o compress.o crctable.o decompress.o dlltest.o huffman.o randtable.o $ROOT_DIR/DependentExtensions/Autopatcher/ApplyPatch.cpp $ROOT_DIR/DependentExtensions/Autopatcher/AutopatcherClient.cpp $ROOT_DIR/DependentExtensions/Autopatcher/AutopatcherMySQLRepository/AutopatcherMySQLRepository.cpp $ROOT_DIR/DependentExtensions/Autopatcher/AutopatcherServer.cpp $ROOT_DIR/DependentExtensions/Autopatcher/CreatePatch.cpp $ROOT_DIR/DependentExtensions/Autopatcher/MemoryCompressor.cpp $ROOT_DIR/DependentExtensions/MySQLInterface/MySQLInterface.cpp"
	GCC_ADDITIONAL_INCLUDES="-I/usr/include/mysql/ -I$ROOT_DIR/DependentExtensions/Autopatcher/AutopatcherMySQLRepository -I$ROOT_DIR/DependentExtensions/Autopatcher -I$ROOT_DIR/DependentExtensions/bzip2-$BZIP2_VERSION -I$ROOT_DIR/DependentExtensions/MySQLInterface"
	GCC_ADDITIONAL_LIBS=-lmysqlclient
	BUILD_BZIP2=1
elif [ "$2" == "--rakNetCompatibility" ]; then
	# TODO #med - improve the handling of optional parameters (according to how it's done in MakeSwig.bat)
	RAKNET_COMPATIBILITY_MODE=1
elif [ ! "$2" == "" ]; then
	echo "Unsupported dependent extension (or option): '$2'"
	syntaxError
	exit 1
fi

# parse optional parameters
if [ "$3" == "--rakNetCompatibility" ]; then
	RAKNET_COMPATIBILITY_MODE=1
elif [ ! "$3" == "" ]; then
	echo "Unsupported option: '$3'"
	syntaxError
	exit 1
fi

# install SWIG, if missing
type -P swig &>/dev/null
if [ ! $? == 0 ]; then
	echo "SWIG is not installed. Do you want this script to download, make, and install SWIG version $SWIG_VERSION? ( y/N )"
	read answer
	if [ ! "$answer" = "y" ] && [ ! "$answer" = "Y" ]; then
		echo "Aborting"
		exit 2
	fi

	if [ ! -d "swig-$SWIG_VERSION" ]; then
		wget https://prdownloads.sourceforge.net/swig/swig-$SWIG_VERSION.tar.gz
		tar xzf swig-$SWIG_VERSION.tar.gz
	fi

	cd swig-$SWIG_VERSION
	./configure
	make
	echo "Installing SWIG requires root privileges, enter password"
	sudo make install
	cd ..

	# remove the downloaded SWIG source, after it was installed
	rm -R swig-$SWIG_VERSION
	rm swig-$SWIG_VERSION.tar.gz
fi

# adjust variables for RakNet compatibility mode
if [ $RAKNET_COMPATIBILITY_MODE == 1 ]; then
	NAMESPACE=RakNet
	SWIG_DEFINES="$SWIG_DEFINES -DRAKNET_COMPATIBILITY"
	GCC_DEFINES=-DRAKNET_COMPATIBILITY=1
	OUTPUT_DIR=../../bindings/raknet_backwards_compatibility/csharp/interfaces
	OUTPUT_WRAPPER_FILENAME=../../bindings/raknet_backwards_compatibility/csharp/wrapper/RakNet_wrap.cxx
	SO_NAME=RakNet
fi

echo "Generating C# wrapper/interface files"
# clear output folder
rm -f $OUTPUT_DIR/*

swig -c++ -csharp -namespace $NAMESPACE $SWIG_INCLUDES $SWIG_ADDITIONAL_INCLUDES $SWIG_DEFINES -outdir $OUTPUT_DIR -o $OUTPUT_WRAPPER_FILENAME SwigInterfaceFiles/RakNet.i
if [ ! $? == 0 ]; then
	echo "SWIG had an error while generating the C# wrappers/interfaces"
	exit 3
fi
echo "C# wrapper/interface files generated succesfully"

OLD_DIR=`pwd`
cd $SOURCE_DIR/src

if [ $BUILD_BZIP2 == 1 ]; then
	echo "Building BZip2 $BZIP2_VERSION"
	gcc -c $ROOT_DIR/DependentExtensions/bzip2-$BZIP2_VERSION/blocksort.c $ROOT_DIR/DependentExtensions/bzip2-$BZIP2_VERSION/bzip2.c $ROOT_DIR/DependentExtensions/bzip2-$BZIP2_VERSION/bzlib.c $ROOT_DIR/DependentExtensions/bzip2-$BZIP2_VERSION/compress.c $ROOT_DIR/DependentExtensions/bzip2-$BZIP2_VERSION/crctable.c $ROOT_DIR/DependentExtensions/bzip2-$BZIP2_VERSION/decompress.c $ROOT_DIR/DependentExtensions/bzip2-$BZIP2_VERSION/dlltest.c $ROOT_DIR/DependentExtensions/bzip2-$BZIP2_VERSION/huffman.c $ROOT_DIR/DependentExtensions/bzip2-$BZIP2_VERSION/randtable.c
	if [ ! $? == 0 ]; then
		echo "Error building BZip2".
		cd $OLD_DIR
		exit 4
	fi
	echo "BZip2 was built successfully"
fi

echo "Building the C# capable shared library"
g++ *.cpp $OUTPUT_WRAPPER_FILENAME $GCC_ADDITIONAL_FILES $GCC_DEFINES -std=c++0x -fPIC -lpthread $GCC_ADDITIONAL_LIBS -I$SOURCE_DIR/include $GCC_ADDITIONAL_INCLUDES -shared -o $SO_NAME
if [ ! $? == 0 ]; then
	echo "There was an error building the shared library"
	cd $OLD_DIR
	exit 5
fi
echo "Building the C# capable shared library succeeded"

echo "Copying to /usr/lib/ requires root privileges; enter password"
sudo cp ./$SO_NAME /usr/lib
if [ ! $? == 0 ]; then
	echo "$SO_NAME lib copy failed"
	cd $OLD_DIR
	exit 6
fi

echo "$SO_NAME lib copied"
cd $OLD_DIR