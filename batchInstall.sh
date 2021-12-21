#!/bin/bash
if [ "$(expr substr $(uname -s) 1 5)" == "Linux" ]; then
    # Do something under Mac OS X platform        
    SYSTEM="linux"
elif [ "$(expr substr $(uname -s) 1 10)" == "MINGW64_NT" ]; then
    # Do something under 64 bits Windows NT platform
    SYSTEM="windows"
else
exit
fi

echo " --- DEEPC Installer $SYSTEM --- "
echo ''                                                                     
echo '                                                                    , %@  '
echo '                                                                   *.@@@&       '
echo '                                                                    @@@/        '
echo '                                                                  ,@(           '
echo '                                                                .@.             '
echo '                                                  ,,          #@                '
echo '                                                 /@@@@@@@@@@@@                  '
echo '                                       (@@@@@@@@@@@@@@@@@@@@@@@@@.              '
echo '                    %@@@@@@@/     &@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@,              '
echo '       . /@@@%/.    @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@.  .@@@@@@&               '
echo '    .@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@.       ,       '
echo '     @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@,    * .@@@@    '
echo '    ,@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@  .&@@@@@@(    '
echo '     *@@@@@@@@@@@@@@ @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@%       '
echo '       ,@@@@@@@@(   @@@@@      @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@         '
echo '                                ,@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@          '
echo '                                  .@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@           '
echo '                                     .@@@@@@@@@@@@@@@@@@@@@@@@@@@               '
echo '                                           ,&@@@@@@@@@@@@/                      '
echo ''
echo 'DeepC Build/Install Batch Helper'
echo ''
echo ''
echo ''
echo ''


echo Enter Path to Nuke Folders
read FOLDER

echo Enter Path to Install Nuke Plugins default: install
read INSTALL

if [ -z $INSTALL ]; then
    INSTALL=install
fi

BASEDIR="${PWD}"
if [ ! -d build ]; then
    mkdir build
fi

echo "$FOLDER/Nuke*"
for nukeFolder in "$FOLDER/Nuke"*; do
    if [ -d "$nukeFolder" ]; then
        VERSION=${nukeFolder[@]/"$FOLDER/Nuke"/""}

        mkdir build/$VERSION
        if [ $SYSTEM = "windows" ]; then
            cmake -G "Visual Studio 15 2017" -A x64 -S $BASEDIR -DCMAKE_INSTALL_PREFIX="$INSTALL/$VERSION" -DNuke_ROOT="$nukeFolder" -B "build/$VERSION"
        else
            cmake -S $BASEDIR -D CMAKE_INSTALL_PREFIX="$INSTALL/$VERSION" -D Nuke_ROOT="$nukeFolder" -B "build/$VERSION"
        fi
        
        cmake --build "build/$VERSION" --config Release
        cmake --install "build/$VERSION"
        echo '-------'
        echo '-------'

        # Create zip archivs
	mkdir release
        if [ $SYSTEM = "windows" ]; then
            tar.exe -c -a -f "./release/DeepC-Windows-$VERSION.tar" -C "$BASEDIR/$INSTALL/$VERSION" "./"
        else
            zip -r ./release/DeepC-Linux-$VERSION.zip $BASEDIR/$INSTALL/$VERSION
        fi       
    fi
done