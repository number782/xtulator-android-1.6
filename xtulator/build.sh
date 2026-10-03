rm xtulator-debug.apk
adb -s SSHEX010990 uninstall com.xtulator.android
docker run --rm -v /home/andrew/Projects/Project8/xtulator:/src xtulator-android16 /bin/bash -c "cd /src && ./build-apk.sh"
adb -s SSHEX010990 install -r xtulator-debug.apk
adb -s SSHEX010990 logcat -c
