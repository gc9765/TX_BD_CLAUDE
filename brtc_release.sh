#!/bin/bash

#检查是否传入了版本号
if [ -z "$1" ]; then
				echo "请传入版本号"
				exit 1
fi

rm -rf ./brtc_output
rm BRTC_RTOS_SDK_TXW81x_*.zip
rm TXW81x_FPV_*.zip

OS_SOURCE_ZIP=TXW81x_FPV_v2.5.3.7-33742-brtc-$1-$(date +%Y%m%d).zip
zip -r ./$OS_SOURCE_ZIP ./*

mkdir -p ./brtc_output

mv ./$OS_SOURCE_ZIP ./brtc_output

cp -rf ./doc ./brtc_output

cp -rf sdk/app/baidu ./brtc_output

mkdir -p ./brtc_output/libs
cp  libs/libbrtc.a ./brtc_output/libs/libbrtc.a

OS_OUTPUT_ZIP=BRTC_RTOS_SDK_TXW81x_$1_$(date +%Y%m%d).zip

zip -r ./$OS_OUTPUT_ZIP ./brtc_output



