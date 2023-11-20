#!/bin/bash
set -x

SCRIPT_PATH="`dirname \"$0\"`"
SCRIPT_PATH=`realpath $SCRIPT_PATH`

PATCH_DST=$1
echo "Updating patches in ${PATCH_DST}"

function checkRet() {
    retVal=$1
    msg=$2
    if [ $retVal -ne 0 ]; then
        echo "ERROR: ${msg}"
        exit -1
    fi
}

for ver in "4.2" "4.4" "6.0"
do
    git clone git://source.ffmpeg.org/ffmpeg.git -b release/${ver} --depth=1 ${SCRIPT_PATH}/ffmpeg${ver}
    checkRet $? "Cloning ${ver} to $SCRIPT_PATH/ffmpeg${ver}"
    cp -r $SCRIPT_PATH/${ver}/* $SCRIPT_PATH/ffmpeg${ver}/
    checkRet $? "copying version-specific files to $SCRIPT_PATH/ffmpeg${ver}/"
    cp -r $SCRIPT_PATH/common/* $SCRIPT_PATH/ffmpeg${ver}/
    checkRet $? "copying common files to $SCRIPT_PATH/ffmpeg${ver}/"
    pushd $SCRIPT_PATH/ffmpeg${ver}
    git add -A .
    checkRet $? "git add"
    git diff --cached > "${PATCH_DST}/ffmpeg${ver}_nvmpi.patch"
    checkRet $? "creating patch to ${PATCH_DST}/ffmpeg${ver}_nvmpi.patch"
    popd
done


