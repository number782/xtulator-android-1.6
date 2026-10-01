rm xtulator-debug.apk
adb uninstall com.xtulator.android
docker run --rm -v /home/andrew/Projects/Project8/xtulator:/src xtulator-android16 /bin/bash -c "cd /src && ./build-apk.sh"
adb install -r xtulator-debug.apk
adb logcat -c
