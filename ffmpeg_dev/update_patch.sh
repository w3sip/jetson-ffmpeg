#!/bin/bash
SCRIPT_PATH="`dirname \"$0\"`"


#clone
rm -rf $SCRIPT_PATH/ffmpeg?.?
git clone git://source.ffmpeg.org/ffmpeg.git -b release/4.2 --depth=1 $SCRIPT_PATH/ffmpeg4.2
git clone git://source.ffmpeg.org/ffmpeg.git -b release/4.4 --depth=1 $SCRIPT_PATH/ffmpeg4.4
git clone git://source.ffmpeg.org/ffmpeg.git -b release/6.0 --depth=1 $SCRIPT_PATH/ffmpeg6.0

#copy data
cp -r $SCRIPT_PATH/4.2/* $SCRIPT_PATH/ffmpeg4.2/
cp -r $SCRIPT_PATH/4.4/* $SCRIPT_PATH/ffmpeg4.4/
cp -r $SCRIPT_PATH/6.0/* $SCRIPT_PATH/ffmpeg6.0/
cp -r $SCRIPT_PATH/common/* $SCRIPT_PATH/ffmpeg4.2/
cp -r $SCRIPT_PATH/common/* $SCRIPT_PATH/ffmpeg4.4/
cp -r $SCRIPT_PATH/common/* $SCRIPT_PATH/ffmpeg6.0/

#
pushd $SCRIPT_PATH/ffmpeg4.2
git add -A .
git diff --cached > $SCRIPT_PATH/../ffmpeg_patches/ffmpeg4.2_nvmpi.patch
popd

#
pushd $SCRIPT_PATH/ffmpeg4.4
git add -A .
git diff --cached > $SCRIPT_PATH/../ffmpeg_patches/ffmpeg4.4_nvmpi.patch
popd

pushd $SCRIPT_PATH//ffmpeg6.0
git add -A .
git diff --cached > $SCRIPT_PATH/../ffmpeg_patches/ffmpeg6.0_nvmpi.patch
popd
